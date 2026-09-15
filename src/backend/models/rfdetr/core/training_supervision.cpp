#include "detail/training_supervision.h"

#include <ATen/Context.h>
#include <ATen/CPUGeneratorImpl.h>
#include <ATen/TensorIndexing.h>
#include <ATen/cuda/CUDAGeneratorImpl.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAFunctions.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/nn/functional/activation.h>
#include <torch/nn/functional/loss.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/detection_geometry.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include "src/common/math/checked_arithmetic.h"

import mmltk.common.logging.profile_utils;

namespace mmltk::backend::models::rfdetr {
namespace {

using namespace torch::indexing;
namespace F = torch::nn::functional;

constexpr const char* kGtProjectionMetric = "rfdetr.supervision.match_free.gt_projection_gpu_ns";
constexpr const char* kAffinityMetric = "rfdetr.supervision.match_free.affinity_gpu_ns";
constexpr const char* kSparseMetric = "rfdetr.supervision.match_free.sparse_gpu_ns";
constexpr const char* kBroadcastCostMetric = "rfdetr.supervision.match_free.broadcast_cost_gpu_ns";
constexpr const char* kObjectiveMetric = "rfdetr.supervision.match_free.objective_gpu_ns";
constexpr const char* kDenoisingPrepareMetric = "rfdetr.supervision.dn.prepare_gpu_ns";
constexpr const char* kDenoisingDecoderMetric = "rfdetr.supervision.dn.decoder_gpu_ns";
constexpr const char* kDenoisingObjectiveMetric = "rfdetr.supervision.dn.objective_gpu_ns";
constexpr const char* kSupervisedStepMetric = "rfdetr.train.supervised_step_gpu_ns";
constexpr const char* kCriterionMetric = "rfdetr.supervision.criterion_gpu_ns";

std::uint64_t tagged_seed(std::uint64_t seed, const std::uint64_t tag) {
    seed ^= tag + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    seed ^= seed >> 30U;
    seed *= 0xbf58476d1ce4e5b9ULL;
    seed ^= seed >> 27U;
    seed *= 0x94d049bb133111ebULL;
    return seed ^ (seed >> 31U);
}

std::uint64_t denoising_step_seed(const TrainingStepIdentity& identity) {
    auto seed = tagged_seed(identity.seed, 0x444e5f53544550ULL);
    seed = tagged_seed(seed, identity.epoch);
    seed = tagged_seed(seed, static_cast<std::uint64_t>(identity.rank));
    return tagged_seed(seed, identity.batch_sequence);
}

void initialize_linear(torch::nn::Linear& linear, const std::uint64_t seed) {
    auto generator = at::detail::createCPUGenerator(seed);
    const double bound = 1.0 / std::sqrt(static_cast<double>(linear->weight.size(1)));
    linear->weight.uniform_(-bound, bound, generator);
    if (linear->bias.defined()) { linear->bias.uniform_(-bound, bound, generator); }
}

void initialize_embedding(torch::nn::Embedding& embedding, const std::uint64_t seed) {
    auto generator = at::detail::createCPUGenerator(seed);
    embedding->weight.normal_(0.0, 1.0, generator);
}

torch::Tensor scalar_edge(const torch::Tensor& tensor) { return tensor.reshape({-1}).narrow(0, 0, 1).sum() * 0.0F; }

void require_denoising_variates(const DenoisingVariates& variates, const std::vector<int64_t>& slot_shape, const torch::Device& device,
                                const int64_t object_classes) {
    const std::vector<int64_t> coordinate_shape{slot_shape[0], slot_shape[1], slot_shape[2], 2};
    const auto require = [&](const torch::Tensor& tensor, const std::vector<int64_t>& expected, const c10::ScalarType dtype,
                             const char* name) {
        if (!tensor.defined() || tensor.sizes().vec() != expected || tensor.device() != device || tensor.scalar_type() != dtype) {
            throw std::runtime_error(std::string("invalid injected DN ") + name + " variates");
        }
    };
    require(variates.center, coordinate_shape, torch::kFloat32, "center");
    require(variates.size, coordinate_shape, torch::kFloat32, "size");
    require(variates.label_flip, slot_shape, torch::kFloat32, "label-flip");
    if (object_classes > 1) { require(variates.other_label, slot_shape, torch::kInt64, "other-label"); }
}

}  // namespace

struct TrainingSupervisionImpl::PaddedTargets {
    torch::Tensor labels;
    torch::Tensor boxes;
    torch::Tensor valid;
    torch::Tensor indices;
    int64_t maximum_count = 0;
};

struct TrainingSupervisionImpl::DenoisingScratch {
    void ensure(const torch::Device& requested_device, const int64_t batch, const int64_t maximum_count) {
        static_cast<void>(mmltk::common::math::checked_multiply(
            static_cast<std::uint64_t>(batch), static_cast<std::uint64_t>(maximum_count), "RF-DETR DN padding extent overflow"));
        if (!device || *device != requested_device) {
            device = requested_device;
            batch_capacity = 0;
            target_capacity = 0;
            positions = torch::Tensor{};
            host_counts = torch::Tensor{};
            host_offsets = torch::Tensor{};
        }
        const auto integer_options = torch::TensorOptions().dtype(torch::kInt64).device(requested_device);
        if (maximum_count > target_capacity) {
            positions = torch::arange(maximum_count, integer_options);
            target_capacity = maximum_count;
        }
        if (requested_device.is_cpu() && batch > batch_capacity) {
            host_counts = torch::empty({batch}, integer_options);
            host_offsets = torch::empty({batch}, integer_options);
            batch_capacity = batch;
        }
    }

    [[nodiscard]] torch::Tensor slot_positions(const int64_t maximum_count) const { return positions.narrow(0, 0, maximum_count); }

    std::optional<torch::Device> device;
    int64_t batch_capacity = 0;
    int64_t target_capacity = 0;
    torch::Tensor positions;
    torch::Tensor host_counts;
    torch::Tensor host_offsets;
};

struct TrainingSupervisionImpl::TimingState {
    enum class Stage : std::uint8_t {
        GroundTruthProjection,
        Affinity,
        Sparse,
        BroadcastCost,
        Objective,
        DenoisingPrepare,
        DenoisingDecoder,
        DenoisingObjective,
        SupervisedStep,
        Criterion,
    };

    enum class LeaseState : std::uint8_t {
        Free,
        Recording,
        Pending,
        Failed,
    };

    struct Slot {
        cudaEvent_t start = nullptr;
        cudaEvent_t stop = nullptr;
        Stage stage = Stage::GroundTruthProjection;
        LeaseState state = LeaseState::Free;
    };

    TimingState(const c10::DeviceIndex device, const std::size_t loss_capacity, const std::size_t lease_capacity)
        : device_id(device), maximum_accumulated_losses(loss_capacity), slots(lease_capacity) {
        c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(device_id));
        try {
            for (auto& slot : slots) {
                if (cudaEventCreate(&slot.start) != cudaSuccess || cudaEventCreate(&slot.stop) != cudaSuccess) {
                    throw std::runtime_error("failed to create RF-DETR supervision timing events");
                }
            }
        } catch (...) {
            for (auto& slot : slots) {
                if (slot.start != nullptr) { static_cast<void>(cudaEventDestroy(slot.start)); }
                if (slot.stop != nullptr) { static_cast<void>(cudaEventDestroy(slot.stop)); }
            }
            throw;
        }
    }

    ~TimingState() {
        c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(device_id));
        for (auto& slot : slots) {
            if (slot.start != nullptr) { static_cast<void>(cudaEventDestroy(slot.start)); }
            if (slot.stop != nullptr) { static_cast<void>(cudaEventDestroy(slot.stop)); }
        }
    }

    [[nodiscard]] bool matches(const c10::DeviceIndex device, const std::size_t loss_capacity) const noexcept {
        return device_id == device && maximum_accumulated_losses == loss_capacity;
    }

    [[nodiscard]] std::size_t outstanding() const noexcept { return slots.size() - free_count; }

    Slot& begin(const Stage stage) {
        c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(device_id));
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const std::size_t index = (next_slot + offset) % slots.size();
            auto& slot = slots[index];
            if (slot.state == LeaseState::Free) {
                if (cudaEventRecord(slot.start, c10::cuda::getCurrentCUDAStream(device_id).stream()) != cudaSuccess) {
                    slot.state = LeaseState::Failed;
                    --free_count;
                    throw std::runtime_error("failed to record RF-DETR supervision timing start");
                }
                slot.stage = stage;
                slot.state = LeaseState::Recording;
                --free_count;
                next_slot = (index + 1) % slots.size();
                return slot;
            }
        }
        throw std::runtime_error("RF-DETR supervision timing lease capacity exhausted");
    }

    void end(Slot& slot) {
        c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(device_id));
        if (slot.state != LeaseState::Recording) { throw std::runtime_error("RF-DETR supervision timing lease is not recording"); }
        if (cudaEventRecord(slot.stop, c10::cuda::getCurrentCUDAStream(device_id).stream()) != cudaSuccess) {
            slot.state = LeaseState::Failed;
            throw std::runtime_error("failed to record RF-DETR supervision timing stop");
        }
        slot.state = LeaseState::Pending;
    }

    template <class Operation>
    decltype(auto) measure(const Stage stage, Operation&& operation) {
        auto& slot = begin(stage);
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<Operation&&>>) {
                std::forward<Operation>(operation)();
                end(slot);
                return;
            } else {
                decltype(auto) result = std::forward<Operation>(operation)();
                end(slot);
                return result;
            }
        } catch (...) {
            if (slot.state == LeaseState::Recording) { end(slot); }
            throw;
        }
    }

    [[nodiscard]] SupervisionTimingHandoff harvest() {
        c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(device_id));
        SupervisionTimingHandoff handoff;
        for (auto& slot : slots) {
            if (slot.state == LeaseState::Failed) { throw std::runtime_error("RF-DETR supervision timing contains a failed lease"); }
            if (slot.state != LeaseState::Pending) { continue; }
            const auto query = cudaEventQuery(slot.stop);
            if (query == cudaErrorNotReady) { continue; }
            if (query != cudaSuccess) {
                slot.state = LeaseState::Failed;
                throw std::runtime_error("failed to query RF-DETR supervision timing event");
            }
            float milliseconds = 0.0F;
            if (cudaEventElapsedTime(&milliseconds, slot.start, slot.stop) != cudaSuccess) {
                slot.state = LeaseState::Failed;
                throw std::runtime_error("failed to read RF-DETR supervision timing event");
            }
            mmltk::common::logging::profile_add_value(metric_name(slot.stage),
                                                      static_cast<std::uint64_t>(std::llround(static_cast<double>(milliseconds) * 1.0e6)));
            slot.state = LeaseState::Free;
            ++free_count;
            ++handoff.completed_leases;
        }
        handoff.outstanding_leases = outstanding();
        return handoff;
    }

    // CLEANUP-IGNORE: Timing stages and ONNX tensor types are unrelated exhaustive domain mappings.
    static const char* metric_name(const Stage stage) {
        switch (stage) {
            // CLEANUP-IGNORE: These timing cases map metrics, not ONNX serialization constants.
            case Stage::GroundTruthProjection:
                return kGtProjectionMetric;
            case Stage::Affinity:
                return kAffinityMetric;
            case Stage::Sparse:
                return kSparseMetric;
            case Stage::BroadcastCost:
                return kBroadcastCostMetric;
            case Stage::Objective:
                return kObjectiveMetric;
            case Stage::DenoisingPrepare:
                return kDenoisingPrepareMetric;
            case Stage::DenoisingDecoder:
                return kDenoisingDecoderMetric;
            case Stage::DenoisingObjective:
                return kDenoisingObjectiveMetric;
            case Stage::SupervisedStep:
                return kSupervisedStepMetric;
            case Stage::Criterion:
                return kCriterionMetric;
        }
        throw std::runtime_error("unknown RF-DETR supervision timing stage");
    }

    c10::DeviceIndex device_id = -1;
    std::size_t maximum_accumulated_losses = 0;
    std::vector<Slot> slots;
    std::size_t free_count = slots.size();
    std::size_t next_slot = 0;
    Slot* active_denoising_decoder = nullptr;
    Slot* active_supervised_step = nullptr;
    Slot* active_criterion = nullptr;
};

class TrainingSupervisionImpl::ProbeMlpImpl final : public torch::nn::Module {
   public:
    ProbeMlpImpl(const int64_t input_width, const int64_t hidden_width)
        : linear1(register_module(std::string(detail::kProbeInputLayer), torch::nn::Linear(input_width, hidden_width))),
          linear2(register_module("linear2", torch::nn::Linear(hidden_width, hidden_width))) {}

    torch::Tensor forward(const torch::Tensor& input) { return linear2->forward(torch::relu(linear1->forward(input))); }

    torch::Tensor forward_ground_truth(const torch::Tensor& labels, const torch::Tensor& boxes, const int64_t object_classes) {
        const auto class_weights = linear1->weight.slice(1, 0, object_classes).transpose(0, 1);
        auto first = class_weights.index({labels});
        first = first + torch::matmul(boxes, linear1->weight.slice(1, object_classes, object_classes + 4).transpose(0, 1));
        if (linear1->bias.defined()) { first = first + linear1->bias; }
        return linear2->forward(torch::relu(first));
    }

    torch::nn::Linear linear1{nullptr};
    torch::nn::Linear linear2{nullptr};
};

TrainingSupervisionImpl::TrainingSupervisionImpl(const NativeRfDetrConfig& config, const std::int64_t foreground_count) : config_(config), foreground_count_(foreground_count) {
    if (!training_supervision_enabled(config.training_supervision)) {
        throw std::runtime_error("inactive RF-DETR training supervision must not be constructed");
    }
    if (!training_supervision_model_config_valid(config)) {
        throw std::runtime_error("invalid RF-DETR training supervision model configuration");
    }
    if (config.training_supervision.denoising.enabled &&
        !training_supervision_query_layout_valid(config.training_supervision, static_cast<std::size_t>(config.num_queries),
                                                 static_cast<std::size_t>(config.group_detr),
                                                 static_cast<std::size_t>(config.num_queries))) {
        throw std::runtime_error("RF-DETR worst-case DN query layout exceeds allocation capacity");
    }
    if (config.training_supervision.assignment == TrainAssignmentKind::MatchFree) {
        const int64_t object_classes = foreground_count_;
        if (object_classes < 1) { throw std::runtime_error("Match-Free requires at least one real object class and one reserved channel"); }
        // The source paper specifies independent hidden-width MLPs but leaves
        // their depth and activation open; RF-DETR completes them as two-layer ReLU MLPs.
        ground_truth_mlp_ = register_module(std::string(detail::kMatchFreeClassAxis.module_name()), std::make_shared<ProbeMlpImpl>(object_classes + 4, config.hidden_dim));
        query_mlp_ = register_module("query_mlp", std::make_shared<ProbeMlpImpl>(config.hidden_dim, config.hidden_dim));
        query_projection_ = register_module("query_projection",
                                            torch::nn::Linear(torch::nn::LinearOptions(config.hidden_dim, config.hidden_dim).bias(false)));
        key_projection_ = register_module("key_projection",
                                          torch::nn::Linear(torch::nn::LinearOptions(config.hidden_dim, config.hidden_dim).bias(false)));
    }
    if (config.training_supervision.denoising.enabled) {
        const int64_t object_classes = foreground_count_;
        if (object_classes < 1) { throw std::runtime_error("DN requires at least one real object class and one reserved channel"); }
        denoising_scratch_ = std::make_unique<DenoisingScratch>();
        denoising_label_embedding_ = register_module(std::string(detail::kDenoisingClassAxis.module_name()), torch::nn::Embedding(object_classes, config.hidden_dim));
        denoising_task_embedding_ = register_module("denoising_task_embedding", torch::nn::Embedding(1, config.hidden_dim));
    }
}

void TrainingSupervisionImpl::append_class_axes(std::vector<detail::ClassTensorAxis>& axes) const {
    const auto append = [&](const auto& module, const detail::ClassTensorFamily& family) {
        if (!module) return;
        const auto prefix = family.prefix();
        for (const auto& parameter : module->named_parameters(true)) {
            const auto name = prefix + parameter.key();
            if (const auto axis = family.Match(name)) axes.push_back({name, axis->dimension, axis->coordinates});
        }
    };
    append(denoising_label_embedding_, detail::kDenoisingClassAxis);
    append(ground_truth_mlp_, detail::kMatchFreeClassAxis);
}

TrainingSupervisionImpl::~TrainingSupervisionImpl() = default;

TrainingSupervisionImpl::PaddedTargets TrainingSupervisionImpl::pad_targets(const PreparedTargets& targets, const torch::Device& device,
                                                                            const int64_t batch, const bool reuse_construction_scratch) {
    if (static_cast<int64_t>(targets.counts.size()) != batch || static_cast<int64_t>(targets.offsets.size()) != batch) {
        throw std::runtime_error("RF-DETR supervision target metadata must align with the output batch");
    }
    const int64_t maximum_count = targets.counts.empty() ? 0 : *std::max_element(targets.counts.begin(), targets.counts.end());
    if (maximum_count == 0) { return {{}, {}, {}, {}, 0}; }
    if (targets.all_labels.device() != device || targets.all_boxes.device() != device) {
        throw std::runtime_error("RF-DETR supervision targets must already reside on the prediction device");
    }
    if (targets.all_labels.size(0) == 0) { throw std::runtime_error("RF-DETR nonempty target metadata requires target tensors"); }
    if (device.is_cuda() && (!targets.target_counts.defined() || targets.target_counts.device() != device ||
                             !targets.target_offsets.defined() || targets.target_offsets.device() != device)) {
        throw std::runtime_error("RF-DETR supervision CUDA metadata must already reside on the prediction device");
    }

    const auto integer_options = torch::TensorOptions().dtype(torch::kInt64).device(device);
    torch::Tensor positions;
    torch::Tensor counts;
    torch::Tensor offsets;
    if (reuse_construction_scratch) {
        if (!denoising_scratch_) { throw std::runtime_error("RF-DETR DN construction scratch is unavailable"); }
        denoising_scratch_->ensure(device, batch, maximum_count);
        positions = denoising_scratch_->slot_positions(maximum_count).view({1, maximum_count});
        if (targets.target_counts.defined() && targets.target_counts.device() == device && targets.target_offsets.defined() &&
            targets.target_offsets.device() == device) {
            counts = targets.target_counts.to(torch::kInt64);
            offsets = targets.target_offsets.to(torch::kInt64);
        } else {
            auto counts_view = denoising_scratch_->host_counts.narrow(0, 0, batch);
            auto offsets_view = denoising_scratch_->host_offsets.narrow(0, 0, batch);
            auto* counts_data = counts_view.data_ptr<int64_t>();
            auto* offsets_data = offsets_view.data_ptr<int64_t>();
            std::copy(targets.counts.begin(), targets.counts.end(), counts_data);
            std::copy(targets.offsets.begin(), targets.offsets.end(), offsets_data);
            counts = std::move(counts_view);
            offsets = std::move(offsets_view);
        }
    } else {
        positions = torch::arange(maximum_count, integer_options).view({1, maximum_count});
        counts = targets.target_counts.defined() && targets.target_counts.device() == device
                     ? targets.target_counts.to(torch::kInt64)
                     : torch::tensor(targets.counts, integer_options);
        offsets = targets.target_offsets.defined() && targets.target_offsets.device() == device
                      ? targets.target_offsets.to(torch::kInt64)
                      : torch::tensor(targets.offsets, integer_options);
    }

    // Every value below is fresh per invocation. Returned topology can remain
    // graph-reachable while the owner grows or reuses only the source buffers.
    const auto valid = positions < counts.view({batch, 1});
    const auto indices = (offsets.view({batch, 1}) + positions).clamp_max(targets.all_labels.size(0) - 1);
    auto labels = targets.all_labels.to(torch::kInt64).index({indices});
    auto boxes = targets.all_boxes.to(torch::kFloat32).index({indices});
    labels = torch::where(valid, labels, torch::zeros_like(labels));
    const auto dummy = torch::tensor({0.5F, 0.5F, 0.25F, 0.25F}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
    boxes = torch::where(valid.unsqueeze(-1), boxes, dummy.view({1, 1, 4}));
    return {std::move(labels), std::move(boxes), valid, indices, maximum_count};
}

torch::Tensor sparse_match_free_correspondence(const torch::Tensor& dense, const torch::Tensor& valid_rows, const float rho) {
    const auto topology_source = dense.detach();
    const auto valid = valid_rows.unsqueeze(1).unsqueeze(-1);
    const auto winners = std::get<1>(topology_source.max(-1, true));
    auto row_filtered = torch::zeros_like(topology_source);
    const auto winner_values = topology_source.gather(-1, winners);
    row_filtered.scatter_(-1, winners, torch::where(valid, winner_values, torch::zeros_like(winner_values)));
    const auto column_max = std::get<0>(row_filtered.max(-2, true));
    const auto selected = valid & (column_max > 0.0F) & (topology_source >= rho * column_max);
    const auto sparse = torch::where(selected, dense, torch::zeros_like(dense));
    return sparse / (sparse.sum(-1, true) + kSparseCorrespondenceEpsilon);
}

DenoisingTransform transform_denoising_targets(const torch::Tensor& original_labels, const torch::Tensor& original_boxes,
                                               const torch::Tensor& valid_slots, const int64_t object_classes,
                                               const DenoisingSupervisionConfig& config, const DenoisingVariates& variates) {
    if (original_labels.dim() != 3 ||
        original_boxes.sizes().vec() !=
            std::vector<int64_t>{original_labels.size(0), original_labels.size(1), original_labels.size(2), 4} ||
        valid_slots.sizes() != original_labels.sizes() || object_classes < 1) {
        throw std::runtime_error("DN transform inputs do not share a valid target layout");
    }
    require_denoising_variates(variates, original_labels.sizes().vec(), original_labels.device(), object_classes);
    const auto centers = original_boxes.slice(-1, 0, 2);
    const auto extents = original_boxes.slice(-1, 2, 4);
    const auto center_bound = extents * (0.5F * config.center_noise_scale);
    const auto inward_bound = torch::nextafter(center_bound, torch::zeros_like(center_bound));
    auto center_offset = (variates.center * 2.0F - 1.0F) * center_bound;
    center_offset = torch::maximum(-inward_bound, torch::minimum(inward_bound, center_offset));
    center_offset = torch::where(center_bound > 0.0F, center_offset, torch::zeros_like(center_offset));
    const auto extent_scale = 1.0F - config.size_noise_scale + 2.0F * config.size_noise_scale * variates.size;
    auto noised_boxes = torch::cat({centers + center_offset, extents * extent_scale}, -1).clamp(0.0F, 1.0F);
    auto noised_labels = original_labels;
    if (object_classes > 1) {
        const auto other = variates.other_label + (variates.other_label >= original_labels).to(torch::kInt64);
        const auto flip = valid_slots & (variates.label_flip < config.label_noise_ratio);
        noised_labels = torch::where(flip, other, original_labels);
    }
    return {noised_labels, noised_boxes, center_offset, extent_scale};
}

torch::Tensor isolated_group_self_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target,
                                            const torch::Tensor& query_position, const DecoderQueryLayout& layout) {
    if (target.dim() != 3 || query_position.sizes() != target.sizes() || target.size(1) != layout.total_queries() ||
        layout.ordinary.groups <= 0 || layout.ordinary.queries_per_group <= 0) {
        throw std::runtime_error("decoder query tensors do not match their typed group layout");
    }
    const int64_t batch = target.size(0);
    const int64_t width = target.size(2);
    const int64_t ordinary_count = layout.ordinary.total_queries();
    // DN-DETR Equation 7 blocks matching queries from observing DN queries and
    // blocks cross-DN-group traffic. RF-DETR deliberately makes the boundary
    // symmetric and retains its pre-existing isolation between ordinary
    // Group-DETR groups; reshaping groups into the batch dimension realizes
    // those blocks without a quadratic all-query mask.
    const auto ordinary_target =
        target.narrow(1, 0, ordinary_count).view({batch, layout.ordinary.groups, layout.ordinary.queries_per_group, width});
    const auto ordinary_position =
        query_position.narrow(1, 0, ordinary_count).view({batch, layout.ordinary.groups, layout.ordinary.queries_per_group, width});
    const auto ordinary_q = (ordinary_target + ordinary_position)
                                .permute({2, 0, 1, 3})
                                .reshape({layout.ordinary.queries_per_group, batch * layout.ordinary.groups, width});
    const auto ordinary_v =
        ordinary_target.permute({2, 0, 1, 3}).reshape({layout.ordinary.queries_per_group, batch * layout.ordinary.groups, width});
    auto ordinary_output = std::get<0>(attention->forward(ordinary_q, ordinary_q, ordinary_v, {}, false));
    ordinary_output = ordinary_output.view({layout.ordinary.queries_per_group, batch, layout.ordinary.groups, width})
                          .permute({1, 2, 0, 3})
                          .reshape({batch, ordinary_count, width});

    if (!layout.has_denoising()) { return ordinary_output; }
    const std::vector<int64_t> expected_padding{batch, layout.denoising_groups, layout.denoising_queries_per_group};
    if (!layout.denoising_key_padding.defined() || layout.denoising_key_padding.sizes().vec() != expected_padding ||
        layout.denoising_key_padding.scalar_type() != torch::kBool || layout.denoising_key_padding.device() != target.device() ||
        !layout.denoising_valid_slots.defined() || layout.denoising_valid_slots.sizes().vec() != expected_padding ||
        layout.denoising_valid_slots.scalar_type() != torch::kBool || layout.denoising_valid_slots.device() != target.device()) {
        throw std::runtime_error("DN key padding does not match its typed group layout");
    }
    auto denoising_target = target.narrow(1, ordinary_count, layout.denoising_queries())
                                .view({batch, layout.denoising_groups, layout.denoising_queries_per_group, width});
    auto denoising_position = query_position.narrow(1, ordinary_count, layout.denoising_queries())
                                  .view({batch, layout.denoising_groups, layout.denoising_queries_per_group, width});
    denoising_target = torch::where(layout.denoising_valid_slots.unsqueeze(-1), denoising_target, torch::zeros_like(denoising_target));
    denoising_position =
        torch::where(layout.denoising_valid_slots.unsqueeze(-1), denoising_position, torch::zeros_like(denoising_position));
    const auto denoising_q = (denoising_target + denoising_position)
                                 .permute({2, 0, 1, 3})
                                 .reshape({layout.denoising_queries_per_group, batch * layout.denoising_groups, width});
    const auto denoising_v =
        denoising_target.permute({2, 0, 1, 3}).reshape({layout.denoising_queries_per_group, batch * layout.denoising_groups, width});
    const auto key_padding = layout.denoising_key_padding.reshape({batch * layout.denoising_groups, layout.denoising_queries_per_group});
    auto denoising_output = std::get<0>(attention->forward(denoising_q, denoising_q, denoising_v, key_padding, false));
    denoising_output = denoising_output.view({layout.denoising_queries_per_group, batch, layout.denoising_groups, width})
                           .permute({1, 2, 0, 3})
                           .reshape({batch, layout.denoising_queries(), width});
    return torch::cat({ordinary_output, denoising_output}, 1);
}

void TrainingSupervisionImpl::initialize(const std::uint64_t request_seed) {
    if (initialized_) { throw std::runtime_error("RF-DETR training supervision may only be initialized once"); }
    torch::NoGradGuard no_grad;
    if (match_free_enabled()) {
        initialize_linear(ground_truth_mlp_->linear1, tagged_seed(request_seed, 0x47545f4d4c5031ULL));
        initialize_linear(ground_truth_mlp_->linear2, tagged_seed(request_seed, 0x47545f4d4c5032ULL));
        initialize_linear(query_mlp_->linear1, tagged_seed(request_seed, 0x515f4d4c5031ULL));
        initialize_linear(query_mlp_->linear2, tagged_seed(request_seed, 0x515f4d4c5032ULL));
        initialize_linear(query_projection_, tagged_seed(request_seed, 0x575f51ULL));
        initialize_linear(key_projection_, tagged_seed(request_seed, 0x575f4bULL));
    }
    if (denoising_enabled()) {
        initialize_embedding(denoising_label_embedding_, tagged_seed(request_seed, 0x444e5f4c4142454cULL));
        initialize_embedding(denoising_task_embedding_, tagged_seed(request_seed, 0x444e5f5441534bULL));
    }
    initialized_ = true;
}

void TrainingSupervisionImpl::install_replicated_initialized_runtime(const TrainingSupervisionConfig& source_config) {
    if (initialized_) { throw std::runtime_error("RF-DETR training supervision runtime may only be installed once"); }
    if (config_.training_supervision != source_config) {
        throw std::runtime_error("RF-DETR training supervision replicas have incompatible topology");
    }
    initialized_ = true;
}

bool TrainingSupervisionImpl::initialized() const noexcept { return initialized_; }

bool TrainingSupervisionImpl::match_free_enabled() const noexcept {
    return config_.training_supervision.assignment == TrainAssignmentKind::MatchFree;
}

bool TrainingSupervisionImpl::denoising_enabled() const noexcept { return config_.training_supervision.denoising.enabled; }

std::optional<DenoisingQueryBatch> TrainingSupervisionImpl::prepare_denoising(const PreparedTargets& targets,
                                                                              const TrainingStepIdentity& identity,
                                                                              const torch::Device& device,
                                                                              const c10::ScalarType decoder_dtype,
                                                                              const DenoisingVariates* injected_variates) {
    if (!denoising_enabled() || !initialized_) {
        if (denoising_enabled()) { throw std::runtime_error("DN preparation was used before one-shot initialization"); }
        return std::nullopt;
    }
    if (decoder_dtype != torch::kFloat16 && decoder_dtype != torch::kBFloat16 && decoder_dtype != torch::kFloat32 &&
        decoder_dtype != torch::kFloat64) {
        throw std::runtime_error("DN decoder content requires a floating-point dtype");
    }
    const int64_t batch = static_cast<int64_t>(targets.counts.size());
    if (batch <= 0) { throw std::runtime_error("DN preparation requires nonempty batch metadata"); }
    if (static_cast<int64_t>(targets.offsets.size()) != batch) {
        throw std::runtime_error("RF-DETR supervision target metadata must align with the output batch");
    }
    const int64_t maximum_count = *std::max_element(targets.counts.begin(), targets.counts.end());
    if (maximum_count == 0) { return std::nullopt; }
    if (maximum_count < 0) { throw std::runtime_error("DN target counts must be nonnegative"); }
    const int64_t groups = config_.training_supervision.denoising.groups;
    const auto dn_queries = mmltk::common::math::checked_multiply(
        static_cast<std::uint64_t>(groups), static_cast<std::uint64_t>(maximum_count), "RF-DETR DN query count overflow");
    const auto dn_slots =
        mmltk::common::math::checked_multiply(static_cast<std::uint64_t>(batch), dn_queries, "RF-DETR DN batch query count overflow");
    static_cast<void>(mmltk::common::math::checked_multiply(dn_slots, static_cast<std::uint64_t>(config_.hidden_dim),
                                                            "RF-DETR DN content extent overflow"));
    static_cast<void>(mmltk::common::math::checked_multiply(dn_slots, 4U, "RF-DETR DN box extent overflow"));
    const auto ordinary_queries = mmltk::common::math::checked_multiply(static_cast<std::uint64_t>(config_.num_queries),
                                                                        static_cast<std::uint64_t>(std::max(1, config_.group_detr)),
                                                                        "RF-DETR ordinary query count overflow");
    static_cast<void>(mmltk::common::math::checked_add(ordinary_queries, dn_queries, "RF-DETR total decoder query count overflow"));
    if (dn_queries > static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("RF-DETR DN query count exceeds tensor indexing capacity");
    }

    auto operation = [&]() -> DenoisingQueryBatch {
        mmltk::backend::ml::cuda::TorchAutocastScope fp32_scope(false, torch::kFloat32);
        const auto padded = pad_targets(targets, device, batch, true);
        const std::vector<int64_t> slot_shape{batch, groups, padded.maximum_count};
        const std::vector<int64_t> coordinate_shape{batch, groups, padded.maximum_count, 2};
        const auto float_options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
        const auto integer_options = torch::TensorOptions().dtype(torch::kInt64).device(device);

        if (!denoising_generator_ || denoising_generator_->device() != device) {
            denoising_generator_ = device.is_cuda() ? at::cuda::detail::createCUDAGenerator(static_cast<c10::DeviceIndex>(
                                                          device.has_index() ? device.index() : c10::cuda::current_device()))
                                                    : at::detail::createCPUGenerator();
        }
        denoising_generator_->set_current_seed(denoising_step_seed(identity));

        DenoisingVariates generated;
        const DenoisingVariates* variates = injected_variates;
        if (variates == nullptr) {
            generated.center = at::rand(coordinate_shape, *denoising_generator_, float_options);
            generated.size = at::rand(coordinate_shape, *denoising_generator_, float_options);
            generated.label_flip = at::rand(slot_shape, *denoising_generator_, float_options);
            if (foreground_count_ > 1) {
                generated.other_label = at::randint(config_.num_classes - 2, slot_shape, *denoising_generator_, integer_options);
            }
            variates = &generated;
        } else {
            require_denoising_variates(*variates, slot_shape, device, foreground_count_);
        }

        const auto valid = padded.valid.unsqueeze(1).expand(slot_shape);
        const auto labels = padded.labels.unsqueeze(1).expand(slot_shape);
        const auto boxes = padded.boxes.unsqueeze(1).expand({batch, groups, padded.maximum_count, 4});
        const int64_t object_classes = foreground_count_;
        auto transformed =
            transform_denoising_targets(labels, boxes, valid, object_classes, config_.training_supervision.denoising, *variates);
        // RF-DETR keeps pretrained ordinary query width and semantics intact.
        // The additive task embedding is the dimension-preserving adaptation
        // of DN-DETR's appended indicator; it exists only on valid DN slots.
        const auto content_fp32 = denoising_label_embedding_->forward(transformed.labels) +
                                  denoising_task_embedding_->weight.index({0}).view({1, 1, 1, config_.hidden_dim});
        const auto content = torch::where(valid.unsqueeze(-1), content_fp32, torch::zeros_like(content_fp32))
                                 .reshape({batch, static_cast<int64_t>(dn_queries), config_.hidden_dim})
                                 .to(decoder_dtype);
        const auto inert_box = torch::tensor({0.5F, 0.5F, 0.25F, 0.25F}, float_options).view({1, 1, 1, 4});
        transformed.boxes = torch::where(valid.unsqueeze(-1), transformed.boxes, inert_box);

        auto key_padding = ~valid;
        const auto empty_group = ~valid.any(-1);
        auto first_slot = torch::zeros_like(key_padding);
        first_slot.index_put_({Slice(), Slice(), 0}, empty_group);
        key_padding = key_padding & ~first_slot;

        const auto target_indices =
            torch::where(valid, padded.indices.unsqueeze(1).expand(slot_shape), torch::full(slot_shape, -1, integer_options));
        DecoderQueryLayout layout;
        layout.ordinary = {std::max<int64_t>(1, config_.group_detr), config_.num_queries};
        layout.denoising_groups = groups;
        layout.denoising_queries_per_group = padded.maximum_count;
        layout.denoising_key_padding = key_padding;
        layout.denoising_valid_slots = valid;
        return {
            content,
            transformed.boxes.reshape({batch, static_cast<int64_t>(dn_queries), 4}),
            labels,
            boxes,
            valid,
            target_indices,
            std::move(layout),
        };
    };

    if (!timing_) { return operation(); }
    if (!device.is_cuda() || timing_->device_id != device.index()) {
        throw std::runtime_error("DN timing leases must match the decoder CUDA device");
    }
    return timing_->measure(TimingState::Stage::DenoisingPrepare, operation);
}

void TrainingSupervisionImpl::configure_timing(const SupervisionTimingSetup& setup) {
    if (!setup.profiling_enabled) {
        if (timing_ && timing_->outstanding() != 0) {
            throw std::runtime_error("RF-DETR supervision timing cannot be disabled with outstanding leases");
        }
        timing_.reset();
        return;
    }
    if (!setup.device.is_cuda()) { throw std::runtime_error("RF-DETR supervision GPU timing requires a CUDA device"); }
    if (setup.maximum_accumulated_losses == 0) {
        throw std::runtime_error("RF-DETR supervision timing requires a positive accumulation-wave capacity");
    }
    const std::uint64_t supervised_layers =
        config_.aux_loss ? mmltk::common::math::checked_add(static_cast<std::uint64_t>(config_.dec_layers), config_.two_stage ? 1U : 0U,
                                                            "RF-DETR supervision timing layer capacity overflow")
                         : 1U;
    std::uint64_t scopes_per_loss = 2U;
    if (match_free_enabled()) {
        scopes_per_loss = mmltk::common::math::checked_add(
            scopes_per_loss,
            mmltk::common::math::checked_add(std::uint64_t{1},
                                             mmltk::common::math::checked_multiply(std::uint64_t{4}, supervised_layers,
                                                                                   "RF-DETR supervision timing scope capacity overflow"),
                                             "RF-DETR supervision timing scope capacity overflow"),
            "RF-DETR supervision timing scope capacity overflow");
    }
    if (denoising_enabled()) {
        scopes_per_loss = mmltk::common::math::checked_add(scopes_per_loss, 3U, "RF-DETR supervision timing scope capacity overflow");
    }
    const std::uint64_t lease_capacity =
        mmltk::common::math::checked_multiply(static_cast<std::uint64_t>(setup.maximum_accumulated_losses), scopes_per_loss,
                                              "RF-DETR supervision timing accumulation capacity overflow");
    if (lease_capacity > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("RF-DETR supervision timing capacity exceeds the host size limit");
    }
    const c10::DeviceIndex device_id = setup.device.has_index() ? setup.device.index() : c10::cuda::current_device();
    if (timing_ && timing_->matches(device_id, setup.maximum_accumulated_losses)) { return; }
    if (timing_ && timing_->outstanding() != 0) {
        throw std::runtime_error("RF-DETR supervision timing cannot be reconfigured with outstanding leases");
    }
    timing_ = std::make_unique<TimingState>(device_id, setup.maximum_accumulated_losses, static_cast<std::size_t>(lease_capacity));
}

torch::Tensor TrainingSupervisionImpl::project_ground_truth(const torch::Tensor& labels, const torch::Tensor& boxes) {
    return query_projection_->forward(ground_truth_mlp_->forward_ground_truth(labels, boxes, foreground_count_));
}

torch::Tensor TrainingSupervisionImpl::dense_correspondence(const torch::Tensor& probes, const torch::Tensor& query_features) {
    const auto keys = key_projection_->forward(query_mlp_->forward(query_features.to(torch::kFloat32)));
    return torch::softmax(torch::einsum("bmd,bgnd->bgmn", {probes, keys}) / std::sqrt(static_cast<double>(config_.hidden_dim)), -1);
}

MatchFreeCorrespondence TrainingSupervisionImpl::correspondence(const torch::Tensor& padded_labels, const torch::Tensor& padded_boxes,
                                                                const torch::Tensor& valid_rows, const torch::Tensor& query_features) {
    if (!initialized_) { throw std::runtime_error("Match-Free supervision was used before one-shot initialization"); }
    mmltk::backend::ml::cuda::TorchAutocastScope fp32_scope(false, torch::kFloat32);
    const auto probes = project_ground_truth(padded_labels, padded_boxes);
    const auto dense = dense_correspondence(probes, query_features);

    return {dense, sparse_match_free_correspondence(dense, valid_rows, config_.training_supervision.match_free.rho)};
}

MatchFreeCost TrainingSupervisionImpl::broadcast_cost(const torch::Tensor& padded_labels, const torch::Tensor& padded_boxes,
                                                      const torch::Tensor& valid_rows, const OutputLayer& layer) const {
    if (!layer.query_layout) { throw std::runtime_error("Match-Free output layer is missing its typed query layout"); }
    mmltk::backend::ml::cuda::TorchAutocastScope fp32_scope(false, torch::kFloat32);
    const auto layout = *layer.query_layout;
    const int64_t batch = layer.pred_logits.size(0);
    if (layer.pred_logits.dim() != 3 || layer.pred_boxes.dim() != 3 || layer.pred_logits.size(2) != config_.num_classes ||
        layer.pred_boxes.size(2) != 4 || layer.pred_logits.size(1) != layout.total_queries() ||
        layer.pred_boxes.size(1) != layout.total_queries()) {
        throw std::runtime_error("Match-Free output layer does not match its configured query/class layout");
    }
    const auto logits = layer.pred_logits.to(torch::kFloat32).view({batch, layout.groups, layout.queries_per_group, config_.num_classes});
    const auto boxes = layer.pred_boxes.to(torch::kFloat32).view({batch, layout.groups, layout.queries_per_group, 4});
    const auto probability = logits.sigmoid();
    const auto negative = (1.0 - config_.focal_alpha) * probability.square() * F::softplus(logits);
    const auto positive = config_.focal_alpha * (1.0 - probability).square() * F::softplus(-logits);
    const auto negative_sum = negative.sum(-1).unsqueeze(-2);
    const auto label_index =
        padded_labels.unsqueeze(1).unsqueeze(2).expand({batch, layout.groups, layout.queries_per_group, padded_labels.size(1)});
    const auto correction = (positive - negative).gather(-1, label_index).permute({0, 1, 3, 2});
    auto classification = negative_sum + correction;
    auto box = (boxes.unsqueeze(-3) - padded_boxes.unsqueeze(1).unsqueeze(-2)).abs().sum(-1);
    auto giou = 1.0F - batched_pairwise_generalized_box_iou(
                           box_cxcywh_to_xyxy(padded_boxes.unsqueeze(1).expand({batch, layout.groups, padded_boxes.size(1), 4}),
                                              BoxExtentPolicy::Preserve),
                           box_cxcywh_to_xyxy(boxes, BoxExtentPolicy::Preserve));
    const auto valid = valid_rows.unsqueeze(1).unsqueeze(-1);
    classification = torch::where(valid, classification, torch::zeros_like(classification));
    box = torch::where(valid, box, torch::zeros_like(box));
    giou = torch::where(valid, giou, torch::zeros_like(giou));
    classification = classification * config_.set_cost_class;
    box = box * config_.set_cost_bbox;
    giou = giou * config_.set_cost_giou;
    return {classification, box, giou, classification + box + giou};
}

TrainingLoss TrainingSupervisionImpl::empty_loss(const ModelOutputs& outputs) const {
    torch::Tensor zero;
    auto add_layer = [&](const OutputLayer& layer) {
        torch::Tensor layer_zero;
        if (layer.query_layout) {
            const auto layout = *layer.query_layout;
            layer_zero =
                layer.pred_logits.view({layer.pred_logits.size(0), layout.groups, layout.queries_per_group, layer.pred_logits.size(-1)})
                    .index({0, Slice(), 0, 0})
                    .sum() *
                0.0F;
            layer_zero = layer_zero + layer.pred_boxes.view({layer.pred_boxes.size(0), layout.groups, layout.queries_per_group, 4})
                                              .index({0, Slice(), 0, 0})
                                              .sum() *
                                          0.0F;
        } else {
            layer_zero = scalar_edge(layer.pred_logits) + scalar_edge(layer.pred_boxes);
        }
        if (layer.query_features) { layer_zero = layer_zero + layer.query_features->index({0, Slice(), 0, 0}).sum() * 0.0F; }
        if (layer.pred_masks) { layer_zero = layer_zero + scalar_edge(*layer.pred_masks); }
        if (layer.sparse_pred_masks) {
            layer_zero = layer_zero + scalar_edge(layer.sparse_pred_masks->spatial_features) +
                         scalar_edge(layer.sparse_pred_masks->query_features) + scalar_edge(layer.sparse_pred_masks->bias);
        }
        zero = zero.defined() ? zero + layer_zero : layer_zero;
    };
    add_layer(outputs.main);
    if (config_.aux_loss) {
        for (const auto& layer : outputs.aux_outputs) {
            add_layer(layer);
        }
        if (config_.two_stage && outputs.enc_outputs) { add_layer(*outputs.enc_outputs); }
    }
    for (const auto& parameter : parameters()) {
        zero = zero + scalar_edge(parameter);
    }
    return {zero, zero, zero, zero, zero, zero};
}

TrainingLoss TrainingSupervisionImpl::denoising_loss(const DenoisingOutputs& outputs, const DeviceLossNormalizer& normalizer) const {
    if (!denoising_enabled() || !initialized_) { throw std::runtime_error("DN loss requires active initialized supervision"); }
    if (!normalizer.target_count.defined() || normalizer.target_count.dim() != 0 ||
        normalizer.target_count.scalar_type() != torch::kFloat32 || normalizer.target_count.device() != outputs.main.pred_logits.device()) {
        throw std::runtime_error("DN loss requires one prediction-device target-count scalar");
    }
    const std::vector<int64_t> target_shape{outputs.main.pred_logits.size(0), outputs.groups, outputs.queries_per_group};
    if (outputs.groups != config_.training_supervision.denoising.groups || outputs.queries_per_group <= 0 ||
        outputs.original_labels.sizes().vec() != target_shape ||
        outputs.original_boxes.sizes().vec() != std::vector<int64_t>{target_shape[0], target_shape[1], target_shape[2], 4} ||
        outputs.valid_slots.sizes().vec() != target_shape) {
        throw std::runtime_error("DN outputs do not match their direct-target layout");
    }
    mmltk::backend::ml::cuda::TorchAutocastScope fp32_scope(false, torch::kFloat32);
    // CLEANUP-OFF: DN loss layers exclude encoder outputs and have a distinct prediction type.
    auto divisor = (normalizer.target_count.to(torch::kFloat32).reshape({}) * outputs.groups).clamp_min(1.0F);
    auto zero = scalar_edge(outputs.main.pred_logits);
    TrainingLoss result{zero, zero, zero, zero, zero, zero};
    std::vector<const DenoisingOutputLayer*> layers{&outputs.main};
    if (config_.aux_loss) {
        for (const auto& layer : outputs.aux_outputs) {
            layers.push_back(&layer);
        }
    }
    // CLEANUP-ON
    const auto valid = outputs.valid_slots;
    const auto labels = outputs.original_labels;
    const auto targets = outputs.original_boxes.to(torch::kFloat32);
    auto objective = [&]() {
        for (const auto* layer : layers) {
            if (layer->pred_logits.sizes().vec() !=
                    std::vector<int64_t>{target_shape[0], target_shape[1], target_shape[2], config_.num_classes} ||
                layer->pred_boxes.sizes().vec() != std::vector<int64_t>{target_shape[0], target_shape[1], target_shape[2], 4}) {
                throw std::runtime_error("DN prediction layer does not match its target layout");
            }
            const auto logits = layer->pred_logits.to(torch::kFloat32);
            const auto probability = logits.sigmoid();
            const auto negative = (1.0 - config_.focal_alpha) * probability.square() * F::softplus(logits);
            const auto positive = config_.focal_alpha * (1.0 - probability).square() * F::softplus(-logits);
            auto classification = negative.sum(-1) + (positive - negative).gather(-1, labels.unsqueeze(-1)).squeeze(-1);
            auto box = (layer->pred_boxes.to(torch::kFloat32) - targets).abs().sum(-1);
            auto giou =
                1.0F - aligned_generalized_box_iou(box_cxcywh_to_xyxy(layer->pred_boxes.to(torch::kFloat32), BoxExtentPolicy::Preserve),
                                                   box_cxcywh_to_xyxy(targets, BoxExtentPolicy::Preserve));
            classification = torch::where(valid, classification, torch::zeros_like(classification));
            box = torch::where(valid, box, torch::zeros_like(box));
            giou = torch::where(valid, giou, torch::zeros_like(giou));
            result.classification = result.classification + config_.cls_loss_coef * classification.sum() / divisor;
            result.box = result.box + config_.bbox_loss_coef * box.sum() / divisor;
            result.giou = result.giou + config_.giou_loss_coef * giou.sum() / divisor;
        }
        result.total = result.classification + result.box + result.giou;
        result.denoising = result.total;
    };
    if (!timing_) {
        objective();
        return result;
    }
    timing_->measure(TimingState::Stage::DenoisingObjective, objective);
    return result;
}

TrainingLoss TrainingSupervisionImpl::loss(const ModelOutputs& outputs, const PreparedTargets& targets,
                                           const DeviceLossNormalizer& normalizer, const bool training_mode) {
    if (!initialized_) { throw std::runtime_error("RF-DETR supervision loss requires one-shot initialization"); }
    if (!match_free_enabled()) {
        if (outputs.denoising) { return denoising_loss(*outputs.denoising, normalizer); }
        return empty_loss(outputs);
    }
    if (!normalizer.target_count.defined() || normalizer.target_count.dim() != 0 ||
        normalizer.target_count.scalar_type() != torch::kFloat32) {
        throw std::runtime_error("Match-Free loss requires one device target-count scalar");
    }
    if (normalizer.target_count.device() != outputs.main.pred_logits.device()) {
        throw std::runtime_error("Match-Free target-count scalar must reside on the prediction device");
    }
    if (timing_ && (!outputs.main.pred_logits.is_cuda() || timing_->device_id != outputs.main.pred_logits.get_device())) {
        throw std::runtime_error("Match-Free timing leases must match the prediction CUDA device");
    }
    mmltk::backend::ml::cuda::TorchAutocastScope fp32_scope(false, torch::kFloat32);
    const int64_t batch = outputs.main.pred_logits.size(0);
    const auto padded = pad_targets(targets, outputs.main.pred_logits.device(), batch, false);
    if (padded.maximum_count == 0) { return empty_loss(outputs); }

    const int64_t groups = training_mode ? config_.group_detr : 1;
    auto divisor = normalizer.target_count.to(torch::kFloat32).reshape({}).clamp_min(1.0F);
    if (!config_.sum_group_losses) { divisor = divisor * groups; }
    auto timed = [this](const TimingState::Stage stage, auto&& operation) {
        if (!timing_) { return operation(); }
        return timing_->measure(stage, std::forward<decltype(operation)>(operation));
    };
    const auto probes = timed(TimingState::Stage::GroundTruthProjection, [&] { return project_ground_truth(padded.labels, padded.boxes); });

    auto zero = scalar_edge(outputs.main.pred_logits);
    TrainingLoss result{zero, zero, zero, zero, zero, zero};
    std::vector<const OutputLayer*> layers{&outputs.main};
    if (config_.aux_loss) {
        for (const auto& layer : outputs.aux_outputs) {
            layers.push_back(&layer);
        }
        if (config_.two_stage && outputs.enc_outputs) { layers.push_back(&*outputs.enc_outputs); }
    }
    for (const auto* layer : layers) {
        if (!layer->query_features || !layer->query_layout) {
            throw std::runtime_error("Match-Free supervised layer lacks captured query features");
        }
        if (layer->query_layout->groups != groups) {
            throw std::runtime_error("Match-Free supervised layer has an inconsistent query group layout");
        }
        const auto dense = timed(TimingState::Stage::Affinity, [&] { return dense_correspondence(probes, *layer->query_features); });
        const auto sparse = timed(TimingState::Stage::Sparse, [&] {
            return sparse_match_free_correspondence(dense, padded.valid, config_.training_supervision.match_free.rho);
        });
        const auto costs =
            timed(TimingState::Stage::BroadcastCost, [&] { return broadcast_cost(padded.labels, padded.boxes, padded.valid, *layer); });

        const auto alpha = config_.training_supervision.match_free.correspondence_weight;
        const auto beta = config_.training_supervision.match_free.query_weight;
        auto objective = [&](const torch::Tensor& term) { return (alpha * (dense * term).sum() + beta * (sparse * term).sum()) / divisor; };
        const auto terms = timed(TimingState::Stage::Objective, [&] {
            return TrainingLoss{
                {},
                objective(costs.classification),
                objective(costs.box),
                objective(costs.giou),
                alpha * (dense * costs.total).sum() / divisor,
                {},
            };
        });
        result.classification = result.classification + terms.classification;
        result.box = result.box + terms.box;
        result.giou = result.giou + terms.giou;
        result.correspondence = result.correspondence + terms.correspondence;
    }
    result.total = result.classification + result.box + result.giou;
    if (outputs.denoising) {
        const auto dn = denoising_loss(*outputs.denoising, normalizer);
        result.total = result.total + dn.total;
        result.classification = result.classification + dn.classification;
        result.box = result.box + dn.box;
        result.giou = result.giou + dn.giou;
        result.denoising = dn.denoising;
    }
    if (mmltk::common::logging::profile_enabled()) {
        mmltk::common::logging::profile_add_value("rfdetr.supervision.match_free.layers", layers.size());
        std::uint64_t valid_pairs = 0;
        for (const auto count : targets.counts) {
            for (const auto* layer : layers) {
                const auto group_pairs = mmltk::common::math::checked_multiply(
                    static_cast<std::uint64_t>(layer->query_layout->groups),
                    static_cast<std::uint64_t>(layer->query_layout->queries_per_group), "Match-Free valid-pair group product overflow");
                const auto image_pairs = mmltk::common::math::checked_multiply(static_cast<std::uint64_t>(count), group_pairs,
                                                                               "Match-Free valid-pair image product overflow");
                valid_pairs = mmltk::common::math::checked_add(valid_pairs, image_pairs, "Match-Free valid-pair count overflow");
            }
        }
        mmltk::common::logging::profile_add_value("rfdetr.supervision.match_free.valid_pairs", valid_pairs);
    }
    return result;
}

SupervisionTimingHandoff TrainingSupervisionImpl::harvest_timing() { return timing_ ? timing_->harvest() : SupervisionTimingHandoff{}; }

void TrainingSupervisionImpl::begin_denoising_decoder_timing() {
    if (!timing_) { return; }
    if (timing_->active_denoising_decoder != nullptr) { throw std::runtime_error("DN decoder timing scope is already active"); }
    timing_->active_denoising_decoder = &timing_->begin(TimingState::Stage::DenoisingDecoder);
}

void TrainingSupervisionImpl::begin_supervised_step_timing() {
    if (!timing_) return;
    if (timing_->active_supervised_step != nullptr) throw std::runtime_error("RF-DETR supervised-step timing scope is already active");
    timing_->active_supervised_step = &timing_->begin(TimingState::Stage::SupervisedStep);
}

void TrainingSupervisionImpl::end_supervised_step_timing() {
    if (!timing_) return;
    if (timing_->active_supervised_step == nullptr) throw std::runtime_error("RF-DETR supervised-step timing scope is not active");
    auto* lease = std::exchange(timing_->active_supervised_step, nullptr);
    timing_->end(*lease);
}

void TrainingSupervisionImpl::begin_criterion_timing() {
    if (!timing_) return;
    if (timing_->active_criterion != nullptr) throw std::runtime_error("RF-DETR criterion timing scope is already active");
    timing_->active_criterion = &timing_->begin(TimingState::Stage::Criterion);
}

void TrainingSupervisionImpl::end_criterion_timing() {
    if (!timing_) return;
    if (timing_->active_criterion == nullptr) throw std::runtime_error("RF-DETR criterion timing scope is not active");
    auto* lease = std::exchange(timing_->active_criterion, nullptr);
    timing_->end(*lease);
}

void TrainingSupervisionImpl::end_denoising_decoder_timing() {
    if (!timing_) { return; }
    if (timing_->active_denoising_decoder == nullptr) { throw std::runtime_error("DN decoder timing scope is not active"); }
    auto* lease = std::exchange(timing_->active_denoising_decoder, nullptr);
    timing_->end(*lease);
}

}  // namespace mmltk::backend::models::rfdetr
