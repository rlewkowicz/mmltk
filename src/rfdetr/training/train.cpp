#include "mmltk/rfdetr/train.h"

#include "dataset_loader.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/detection_ops.h"
#include "mmltk/rfdetr/modules.h"
#include "mmltk/rfdetr/target_builder.h"
#include "mmltk/rfdetr/training_ops.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/cuda_utils.h"
#include "rfdetr/native_optimizer.h"
#include "spdmon/spdmon.hpp"
#include "rfdetr/runtime.h"
#include "rfdetr/validate_internal.h"
#include "mmltk/rfdetr/draw_cuda.h"

#include "rfdetr/common/dataset_utils.h"
#include "rfdetr/common/tensor_utils.h"

#include <c10/cuda/CUDAGuard.h>
#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif
#include <torch/csrc/autograd/autograd.h>
#include <nlohmann/json.hpp>
#include <torch/nn/utils/clip_grad.h>
#include <torch/serialize.h>
#include <torch/version.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

using json = nlohmann::json;
using namespace validate_detail;

namespace {

struct OptimizerBuildResult {
    NativeOptimizer optimizer;
    std::vector<double> base_lrs;
};

struct EvalPassResult {
    double loss = 0.0;
    EvalSummary summary;
    PhaseTiming timing;
};

struct CapturedEvalSample {
    torch::Tensor image;
    torch::Tensor boxes;
    torch::Tensor labels;
    torch::Tensor masks;
};

struct ResumeState {
    int start_epoch = 0;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    std::vector<StateDictEntry> ema_state;
};

struct DistributedContext {
    bool enabled = false;
    int rank = 0;
    int world_size = 1;
#if defined(USE_C10D_NCCL)
    c10::intrusive_ptr<c10d::Store> store;
    c10::intrusive_ptr<c10d::Backend> process_group;
#endif
};

void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor);

int rfdetr_output_class_count(uint32_t dataset_class_count) {
    // Mirror rf-detr/src/rfdetr/models/lwdetr.py: build_model() and
    // build_criterion_and_postprocessors() both use args.num_classes + 1.
    return static_cast<int>(dataset_class_count) + 1;
}

int checked_inference_batch_size(size_t batch_size) {
    const size_t effective_batch_size = std::max<size_t>(1, batch_size);
    if (effective_batch_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("batch_size exceeds supported inference compilation range");
    }
    return static_cast<int>(effective_batch_size);
}

std::string phase_progress_label(const char* phase, int epoch, int total_epochs) {
    return std::string(phase) + " " + std::to_string(epoch + 1) + "/" + std::to_string(total_epochs);
}

size_t eval_prediction_slot_count(const RuntimeSplit& split) {
    const size_t lane_threads = static_cast<size_t>(std::max(1, split.lane_threads));
    const size_t cpu_threads = static_cast<size_t>(std::max(1, split.cpu_threads));
    return std::max<size_t>(2, 1 + ((cpu_threads + lane_threads - 1) / lane_threads));
}

struct TrainLaneContext {
    c10::cuda::CUDAStream stream;
    TargetScratch target_scratch;
    std::shared_ptr<NativeRfDetrModel> model;
    std::vector<torch::Tensor> grad_params;
    size_t synced_parameter_version = std::numeric_limits<size_t>::max();
};

struct TrainLaneResult {
    torch::Tensor loss;
    torch::Tensor class_loss;
    torch::Tensor box_loss;
    std::vector<torch::Tensor> gradients;
    std::shared_ptr<CudaEventHandle> ready_event;
};

int effective_train_lanes(const TrainOptions& options) {
    if (options.lanes < 0) {
        throw std::runtime_error("RF-DETR train --lanes must be non-negative");
    }
    return std::max(1, options.lanes);
}

size_t micro_batches_per_optimizer_step(const TrainOptions& options, int train_lane_count) {
    return static_cast<size_t>(std::max(1, options.grad_accum_steps)) *
           static_cast<size_t>(std::max(1, train_lane_count));
}

size_t effective_batch_per_rank(const TrainOptions& options, int train_lane_count) {
    return static_cast<size_t>(options.batch_size) * micro_batches_per_optimizer_step(options, train_lane_count);
}

size_t effective_batch_global(const TrainOptions& options,
                              const DistributedContext& distributed,
                              int train_lane_count) {
    return effective_batch_per_rank(options, train_lane_count) *
           static_cast<size_t>(std::max(1, distributed.world_size));
}

std::shared_ptr<CudaEventHandle> record_cuda_event(cudaStream_t stream, const char* context) {
    cudaEvent_t event = nullptr;
    ensure_cuda_ok(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), context);
    auto handle = std::make_shared<CudaEventHandle>(event);
    ensure_cuda_ok(cudaEventRecord(handle->get(), stream), context);
    return handle;
}

std::shared_ptr<CudaEventHandle> record_current_stream_event(int device_id, const char* context) {
    return record_cuda_event(c10::cuda::getCurrentCUDAStream(checked_device_index(device_id)).stream(), context);
}

int64_t batch_target_instance_count(const Batch& batch) {
    int64_t total = 0;
    for (size_t image_pos = 0; image_pos < batch.num_images; ++image_pos) {
        const uint32_t dataset_index = batch.image_indices[image_pos];
        total += static_cast<int64_t>(batch.label_index[dataset_index].num_instances);
    }
    return total;
}

double resolve_num_boxes_value(int64_t num_boxes_int,
                               const DetectionConfig& config,
                               bool training_mode,
                               const DistributedContext& distributed,
                               const torch::Device& device) {
    const int64_t group_detr = training_mode ? config.group_detr : 1;
    if (!config.sum_group_losses) {
        num_boxes_int *= group_detr;
    }

    double num_boxes_value = std::max(
        static_cast<double>(num_boxes_int) / static_cast<double>(std::max<int64_t>(1, config.world_size)),
        1.0
    );
    if (!distributed.enabled) {
        return num_boxes_value;
    }

    auto num_boxes = torch::tensor(
        {static_cast<float>(num_boxes_int)},
        torch::TensorOptions().dtype(torch::kFloat32).device(device)
    );
    distributed_all_reduce_tensor(distributed, num_boxes);
    return torch::clamp_min(num_boxes / std::max<int64_t>(1, config.world_size), 1.0).item<double>();
}

void wait_for_lane_result(const TrainLaneResult& lane_result, int device_id) {
    if (!lane_result.ready_event) {
        throw std::runtime_error("parallel RF-DETR train result is missing a completion event");
    }
    ensure_cuda_ok(
        cudaStreamWaitEvent(
            c10::cuda::getCurrentCUDAStream(checked_device_index(device_id)).stream(),
            lane_result.ready_event->get(),
            0),
        "cudaStreamWaitEvent for parallel train lane result");
}

void merge_lane_gradients(const TrainLaneResult& lane_result,
                          std::vector<torch::Tensor>& parameters,
                          int device_id) {
    wait_for_lane_result(lane_result, device_id);

    torch::NoGradGuard no_grad;
    const size_t limit = std::min(parameters.size(), lane_result.gradients.size());
    for (size_t index = 0; index < limit; ++index) {
        const auto& gradient = lane_result.gradients[index];
        if (!gradient.defined()) {
            continue;
        }
        if (!parameters[index].grad().defined()) {
            parameters[index].mutable_grad() = gradient.detach().clone();
        } else {
            parameters[index].mutable_grad().add_(gradient);
        }
    }
}

void copy_module_state(torch::nn::Module& destination, const torch::nn::Module& source) {
    torch::NoGradGuard no_grad;

    auto destination_parameters = destination.named_parameters(true);
    for (const auto& item : source.named_parameters(true)) {
        auto* target = destination_parameters.find(item.key());
        if (target == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing parameter: " + item.key());
        }
        target->copy_(item.value());
        target->requires_grad_(item.value().requires_grad());
    }

    auto destination_buffers = destination.named_buffers(true);
    for (const auto& item : source.named_buffers(true)) {
        auto* target = destination_buffers.find(item.key());
        if (target == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing buffer: " + item.key());
        }
        target->copy_(item.value());
    }
}

std::shared_ptr<NativeRfDetrModel> make_train_lane_model(NativeRfDetrModel& model, int device_id) {
    auto lane_model = std::make_shared<NativeRfDetrModel>(model.config());
    lane_model->to(cuda_device(device_id));
    lane_model->train();
    copy_module_state(*lane_model, model);
    return lane_model;
}

std::vector<torch::Tensor> lane_grad_parameters(const NativeRfDetrModel& lane_model,
                                                const std::vector<std::string>& parameter_names) {
    const auto named_parameters = lane_model.named_parameters(true);
    std::vector<torch::Tensor> parameters;
    parameters.reserve(parameter_names.size());
    for (const auto& name : parameter_names) {
        auto* tensor = named_parameters.find(name);
        if (tensor == nullptr) {
            throw std::runtime_error("parallel RF-DETR lane is missing trainable parameter: " + name);
        }
        parameters.push_back(*tensor);
    }
    return parameters;
}

std::optional<std::string> first_running_stats_buffer_name(NativeRfDetrModel& model) {
    for (const auto& item : model.named_buffers(true)) {
        if (item.key().find("running_mean") != std::string::npos ||
            item.key().find("running_var") != std::string::npos ||
            item.key().find("num_batches_tracked") != std::string::npos) {
            return item.key();
        }
    }
    return std::nullopt;
}

void ensure_train_lane_model_supported(NativeRfDetrModel& model, int train_lane_count) {
    if (train_lane_count <= 1) {
        return;
    }
    const auto running_stats = first_running_stats_buffer_name(model);
    if (running_stats.has_value()) {
        throw std::runtime_error(
            "parallel RF-DETR train --lanes requires a model without running-stat buffers; found " +
            *running_stats);
    }
}

void record_result_stream(const TensorMap& result, const c10::cuda::CUDAStream& stream) {
    for (const auto& [name, value] : result) {
        (void)name;
        if (value.defined() && value.device().is_cuda()) {
            value.record_stream(stream);
        }
    }
}

bool is_rank_zero(const DistributedContext& distributed) {
    return distributed.rank == 0;
}

std::string train_progress_postfix(double average_class_loss,
                                   double average_box_loss,
                                   double average_loss,
                                   double step_class_loss,
                                   double step_box_loss,
                                   double step_loss,
                                   double images_per_second,
                                   int64_t optimizer_steps,
                                   int64_t steps_per_epoch) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(4);
    stream << "cl=" << average_class_loss
           << ", bl=" << average_box_loss
           << ", l=" << average_loss
           << ", scl=" << step_class_loss
           << ", sbl=" << step_box_loss
           << ", sl=" << step_loss;
    stream.precision(2);
    stream << ", img/s=" << images_per_second;
    stream.precision(0);
    stream << ", step=" << optimizer_steps << "/" << steps_per_epoch;
    return stream.str();
}

torch::Tensor loss_value_or_zero(const TensorMap& loss_dict,
                                 const torch::Device& device,
                                 std::string_view key) {
    const auto found = loss_dict.find(std::string(key));
    if (found != loss_dict.end()) {
        return found->second;
    }
    return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
}

std::string format_nonfinite_loss_report(const TensorMap& loss_dict,
                                         const std::vector<torch::Tensor>& parameters,
                                         const std::vector<std::string>& parameter_names) {
    std::ostringstream report;
    bool wrote_loss = false;
    for (const auto& [name, value] : loss_dict) {
        if (!torch::isfinite(value).item<bool>()) {
            if (!wrote_loss) {
                report << " nonfinite_losses=[";
                wrote_loss = true;
            } else {
                report << ", ";
            }
            report << name << "=" << value.item<double>();
        }
    }
    if (wrote_loss) {
        report << "]";
    }

    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& param = parameters[index];
        if (param.defined() && !torch::isfinite(param).all().item<bool>()) {
            report << " nonfinite_param=" << parameter_names[index];
            break;
        }
        if (param.grad().defined() && !torch::isfinite(param.grad()).all().item<bool>()) {
            report << " nonfinite_grad=" << parameter_names[index];
            break;
        }
    }

    return report.str();
}

const char* scalar_type_name(const at::ScalarType scalar_type) {
    switch (scalar_type) {
    case torch::kFloat:
        return "fp32";
    case torch::kFloat16:
        return "fp16";
    case torch::kBFloat16:
        return "bf16";
    default:
        return "other";
    }
}

void append_json_line(const std::filesystem::path& path, const json& payload) {
    std::ofstream stream(path, std::ios::app);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to append RF-DETR training log: " + path.string());
    }
    stream << payload.dump() << '\n';
}

void write_json_file(const std::filesystem::path& path, const json& payload) {
    const std::filesystem::path temporary_path = path.string() + ".tmp";
    std::ofstream stream(temporary_path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write RF-DETR JSON file: " + temporary_path.string());
    }
    stream << payload.dump(2) << '\n';
    stream.close();
    if (!stream) {
        throw std::runtime_error("failed to flush RF-DETR JSON file: " + temporary_path.string());
    }
    std::filesystem::rename(temporary_path, path);
}

json metric_summary_json(const MetricSummary& summary) {
    return json{
        {"ap", summary.ap},
        {"ap50", summary.ap50},
        {"ap75", summary.ap75},
    };
}

json eval_summary_json(const EvalSummary& summary) {
    return json{
        {"bbox", metric_summary_json(summary.bbox)},
        {"mask", metric_summary_json(summary.mask)},
    };
}

DistributedContext make_distributed_context(const TrainOptions& options) {
    DistributedContext distributed;
    if (!options.distributed_worker || options.distributed_world_size <= 1) {
        return distributed;
    }
    if (options.distributed_store_path.empty()) {
        throw std::runtime_error("distributed RF-DETR worker requires --dist-store-file");
    }
    if (options.distributed_rank < 0 || options.distributed_rank >= options.distributed_world_size) {
        throw std::runtime_error("distributed RF-DETR worker rank is out of range");
    }

#if !defined(USE_C10D_NCCL)
    throw std::runtime_error(
        "distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#else
    distributed.enabled = true;
    distributed.rank = options.distributed_rank;
    distributed.world_size = options.distributed_world_size;
    c10::cuda::CUDAGuard device_guard(cuda_device_index(options.device_id));
    distributed.store =
        c10::make_intrusive<c10d::FileStore>(options.distributed_store_path.string(), options.distributed_world_size);

    auto pg_options = c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
    pg_options->timeout = c10d::kProcessGroupNCCLDefaultTimeout;
    distributed.process_group = c10::make_intrusive<c10d::ProcessGroupNCCL>(
        distributed.store,
        distributed.rank,
        distributed.world_size,
        std::move(pg_options));
    return distributed;
#endif
}

void distributed_barrier(const DistributedContext& distributed) {
    if (!distributed.enabled) {
        return;
    }
#if defined(USE_C10D_NCCL)
    distributed.process_group->barrier()->wait();
#else
    (void)distributed;
    throw std::runtime_error(
        "distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor) {
    if (!distributed.enabled) {
        return;
    }
#if defined(USE_C10D_NCCL)
    std::vector<torch::Tensor> tensors = {tensor};
    distributed.process_group->allreduce(tensors)->wait();
#else
    (void)distributed;
    (void)tensor;
    throw std::runtime_error(
        "distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

void distributed_all_reduce_coalesced(const DistributedContext& distributed, std::vector<torch::Tensor>& tensors) {
    if (!distributed.enabled || tensors.empty()) {
        return;
    }
#if defined(USE_C10D_NCCL)
    distributed.process_group->allreduce_coalesced(tensors)->wait();
#else
    (void)distributed;
    (void)tensors;
    throw std::runtime_error(
        "distributed RF-DETR training requires a LibTorch build with NCCL/c10d enabled");
#endif
}

bool is_encoder_param(std::string_view name) {
    return name.find("backbone.0.encoder") != std::string_view::npos;
}

bool encoder_zero_weight_decay(std::string_view name) {
    return name.find("gamma") != std::string_view::npos ||
           name.find("bias") != std::string_view::npos ||
           name.find("norm") != std::string_view::npos ||
           name.find("embeddings") != std::string_view::npos ||
           name.find("position_embeddings") != std::string_view::npos ||
           name.find("rel_pos") != std::string_view::npos;
}

int encoder_layer_id(std::string_view name) {
    constexpr int kNumVitLayers = 13;
    if (name.find("embeddings") != std::string_view::npos) {
        return 0;
    }
    const auto layer_pos = name.find(".layer.");
    if (layer_pos != std::string_view::npos && name.find(".residual.") == std::string_view::npos) {
        const auto digits_pos = layer_pos + std::string_view(".layer.").size();
        size_t digits_len = 0;
        while (digits_pos + digits_len < name.size() && std::isdigit(static_cast<unsigned char>(name[digits_pos + digits_len]))) {
            ++digits_len;
        }
        if (digits_len > 0) {
            return std::stoi(std::string(name.substr(digits_pos, digits_len))) + 1;
        }
    }
    return kNumVitLayers + 1;
}

double parameter_lr(std::string_view name, const TrainOptions& options) {
    if (is_encoder_param(name)) {
        constexpr int kNumVitLayers = 13;
        const int layer_id = encoder_layer_id(name);
        return options.lr_encoder *
               std::pow(options.encoder_layer_decay, static_cast<double>(kNumVitLayers + 1 - layer_id)) *
               options.lr_component_decay * options.lr_component_decay;
    }
    if (name.find("transformer.decoder") != std::string_view::npos) {
        return options.lr * options.lr_component_decay;
    }
    return options.lr;
}

double parameter_weight_decay(std::string_view name, const TrainOptions& options) {
    if (is_encoder_param(name)) {
        return encoder_zero_weight_decay(name) ? 0.0 : options.weight_decay;
    }
    return options.weight_decay;
}

bool is_muon_hidden_weight(std::string_view name, const torch::Tensor& param) {
    if ((param.dim() != 2 && param.dim() != 4) || name.find(".weight") == std::string_view::npos) {
        return false;
    }
    if (name.find("embeddings") != std::string_view::npos ||
        name.find("position_embeddings") != std::string_view::npos ||
        name.find("cls_token") != std::string_view::npos ||
        name.find("mask_token") != std::string_view::npos ||
        name.find("query_feat") != std::string_view::npos ||
        name.find("refpoint_embed") != std::string_view::npos ||
        name.find("class_embed") != std::string_view::npos ||
        name.find("bbox_embed") != std::string_view::npos ||
        name.find("segmentation_head") != std::string_view::npos) {
        return false;
    }
    return true;
}

OptimizerBuildResult build_optimizer(NativeRfDetrModel& model, const TrainOptions& options) {
    struct GroupSpec {
        double lr = 0.0;
        double weight_decay = 0.0;
        bool use_muon = false;
        std::vector<size_t> param_indices;
    };

    std::unordered_map<std::string, size_t> group_index;
    std::vector<GroupSpec> groups;
    std::vector<std::pair<std::string, torch::Tensor>> named_params;

    for (const auto& item : model.named_parameters(true)) {
        const auto& param = item.value();
        if (!param.requires_grad()) {
            continue;
        }

        const double lr = parameter_lr(item.key(), options);
        const double weight_decay = parameter_weight_decay(item.key(), options);
        const bool use_muon = options.optimizer == TrainOptimizerKind::Muon && is_muon_hidden_weight(item.key(), param);
        const std::string key = std::string(use_muon ? "muon:" : "aux:") +
                                std::to_string(lr) + ":" + std::to_string(weight_decay);
        auto found = group_index.find(key);
        if (found == group_index.end()) {
            found = group_index.emplace(key, groups.size()).first;
            groups.push_back(GroupSpec{lr, weight_decay, use_muon, {}});
        }
        named_params.emplace_back(item.key(), param);
        groups[found->second].param_indices.push_back(named_params.size() - 1);
    }

    if (groups.empty() || named_params.empty()) {
        throw std::runtime_error("RF-DETR runtime found no trainable parameters");
    }

    std::vector<double> base_lrs;
    base_lrs.reserve(groups.size());
    if (options.optimizer == TrainOptimizerKind::Muon) {
        std::vector<NativeMuonWithAuxAdam::Group> optimizer_groups;
        std::vector<NativeMuonWithAuxAdam::NamedParameter> optimizer_params;
        optimizer_groups.reserve(groups.size());
        optimizer_params.reserve(named_params.size());
        for (const auto& named_param : named_params) {
            optimizer_params.push_back(NativeMuonWithAuxAdam::NamedParameter{named_param.first, named_param.second});
        }
        for (auto& group : groups) {
            base_lrs.push_back(group.lr);
            optimizer_groups.push_back(NativeMuonWithAuxAdam::Group{
                NativeMuonGroupConfig{group.lr, group.weight_decay, options.momentum, group.use_muon, true},
                std::move(group.param_indices)});
        }
        return OptimizerBuildResult{
            NativeOptimizer(NativeMuonWithAuxAdam(std::move(optimizer_groups), std::move(optimizer_params))),
            std::move(base_lrs),
        };
    }

    std::vector<NativeAdamW::Group> optimizer_groups;
    std::vector<NativeAdamW::NamedParameter> optimizer_params;
    optimizer_groups.reserve(groups.size());
    optimizer_params.reserve(named_params.size());
    for (const auto& named_param : named_params) {
        optimizer_params.push_back(NativeAdamW::NamedParameter{named_param.first, named_param.second});
    }
    for (auto& group : groups) {
        base_lrs.push_back(group.lr);
        optimizer_groups.push_back(
            NativeAdamW::Group{NativeAdamWGroupConfig{group.lr, group.weight_decay, false}, std::move(group.param_indices)});
    }

    std::vector<torch::Tensor> trainable_params;
    trainable_params.reserve(optimizer_params.size());
    for (const auto& named_param : optimizer_params) {
        trainable_params.push_back(named_param.tensor);
    }
    const auto backend = (options.fused_optimizer && native_optimizer_supports_fused(trainable_params))
                             ? NativeOptimizerBackend::fused :
                         native_optimizer_supports_foreach(trainable_params) ? NativeOptimizerBackend::foreach :
                                                                               NativeOptimizerBackend::eager;

    return OptimizerBuildResult{
        NativeOptimizer(NativeAdamW(std::move(optimizer_groups), std::move(optimizer_params), backend)),
        std::move(base_lrs),
    };
}

std::string archive_entry_name(size_t index) {
    std::ostringstream stream;
    stream << "entry_" << std::setw(6) << std::setfill('0') << index;
    return stream.str();
}

void write_string(torch::serialize::OutputArchive& archive, const char* key, std::string_view value) {
    archive.write(key, c10::IValue(std::string(value)));
}

void write_int(torch::serialize::OutputArchive& archive, const char* key, int64_t value) {
    archive.write(key, c10::IValue(value));
}

void write_bool(torch::serialize::OutputArchive& archive, const char* key, bool value) {
    archive.write(key, c10::IValue(value));
}

void write_double(torch::serialize::OutputArchive& archive, const char* key, double value) {
    archive.write(key, c10::IValue(value));
}

std::optional<int64_t> read_optional_int(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value)) {
        return std::nullopt;
    }
    if (!value.isInt()) {
        throw std::runtime_error(std::string("RF-DETR training checkpoint key is not an int: ") + key);
    }
    return value.toInt();
}

std::optional<double> read_optional_double(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value)) {
        return std::nullopt;
    }
    if (!value.isDouble() && !value.isInt()) {
        throw std::runtime_error(std::string("RF-DETR training checkpoint key is not a number: ") + key);
    }
    return value.isDouble() ? value.toDouble() : static_cast<double>(value.toInt());
}

std::optional<std::string> read_optional_string(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    if (!archive.try_read(key, value)) {
        return std::nullopt;
    }
    if (!value.isString()) {
        throw std::runtime_error(std::string("RF-DETR training checkpoint key is not a string: ") + key);
    }
    return std::string(value.toStringRef());
}

int64_t require_int(torch::serialize::InputArchive& archive, const char* key) {
    c10::IValue value;
    archive.read(key, value);
    if (!value.isInt()) {
        throw std::runtime_error(std::string("RF-DETR training checkpoint key is not an int: ") + key);
    }
    return value.toInt();
}

std::vector<StateDictEntry> collect_module_state(
    const torch::nn::Module& module,
    const std::unordered_map<std::string, torch::Tensor>* parameter_overrides = nullptr) {
    std::vector<StateDictEntry> state;

    const auto parameters = module.named_parameters(true);
    state.reserve(parameters.size() + module.named_buffers(true).size());
    for (const auto& item : parameters) {
        StateDictEntry entry;
        entry.name = item.key();
        if (parameter_overrides != nullptr) {
            const auto found = parameter_overrides->find(entry.name);
            if (found != parameter_overrides->end()) {
                entry.tensor = found->second.detach();
                state.push_back(std::move(entry));
                continue;
            }
        }
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }

    for (const auto& item : module.named_buffers(true)) {
        StateDictEntry entry;
        entry.name = item.key();
        entry.tensor = item.value().detach();
        state.push_back(std::move(entry));
    }
    return state;
}

std::vector<StateDictEntry> read_state_archive(torch::serialize::InputArchive& archive, const char* key) {
    torch::serialize::InputArchive state_archive;
    archive.read(key, state_archive);
    const int64_t entry_count = require_int(state_archive, "entry_count");
    if (entry_count < 0) {
        throw std::runtime_error("RF-DETR training checkpoint entry_count is negative");
    }

    std::vector<StateDictEntry> state_dict;
    state_dict.reserve(static_cast<size_t>(entry_count));
    for (int64_t index = 0; index < entry_count; ++index) {
        torch::serialize::InputArchive entry_archive;
        state_archive.read(archive_entry_name(static_cast<size_t>(index)), entry_archive);
        c10::IValue name_value;
        entry_archive.read("name", name_value);
        if (!name_value.isString()) {
            throw std::runtime_error("RF-DETR training checkpoint state entry name is not a string");
        }
        StateDictEntry entry;
        entry.name = name_value.toStringRef();
        entry_archive.read("tensor", entry.tensor);
        state_dict.push_back(std::move(entry));
    }
    return state_dict;
}

NativeCheckpointMetadata checkpoint_metadata(const ResolvedModelArtifacts& artifacts, int64_t num_classes) {
    NativeCheckpointMetadata metadata;
    metadata.preset_name = artifacts.config.preset_name;
    metadata.source_kind = artifacts.input_kind;
    metadata.source_path = artifacts.input_path.string();
    metadata.num_classes = num_classes;
    metadata.sum_group_losses = artifacts.config.sum_group_losses;
    metadata.use_varifocal_loss = artifacts.config.use_varifocal_loss;
    metadata.use_position_supervised_loss = artifacts.config.use_position_supervised_loss;
    metadata.ia_bce_loss = artifacts.config.ia_bce_loss;
    metadata.aux_loss = artifacts.config.aux_loss;
    metadata.mask_point_sample_ratio = artifacts.config.mask_point_sample_ratio;
    metadata.focal_alpha = artifacts.config.focal_alpha;
    metadata.cls_loss_coef = artifacts.config.cls_loss_coef;
    metadata.bbox_loss_coef = artifacts.config.bbox_loss_coef;
    metadata.giou_loss_coef = artifacts.config.giou_loss_coef;
    metadata.mask_ce_loss_coef = artifacts.config.mask_ce_loss_coef;
    metadata.mask_dice_loss_coef = artifacts.config.mask_dice_loss_coef;
    metadata.set_cost_class = artifacts.config.set_cost_class;
    metadata.set_cost_bbox = artifacts.config.set_cost_bbox;
    metadata.set_cost_giou = artifacts.config.set_cost_giou;
    return metadata;
}

std::unordered_map<std::string, torch::Tensor> ema_override_map(const std::vector<std::string>& param_names,
                                                                const ModelEma& ema) {
    const auto& shadow_params = ema.shadow_params();
    if (shadow_params.size() != param_names.size()) {
        throw std::runtime_error("RF-DETR EMA parameter count changed unexpectedly");
    }

    std::unordered_map<std::string, torch::Tensor> overrides;
    overrides.reserve(param_names.size());
    for (size_t index = 0; index < param_names.size(); ++index) {
        overrides.emplace(param_names[index], shadow_params[index]);
    }
    return overrides;
}

std::vector<StateDictEntry> ema_state_entries(const std::vector<std::string>& param_names,
                                              const ModelEma& ema) {
    const auto overrides = ema_override_map(param_names, ema);
    std::vector<StateDictEntry> state;
    state.reserve(overrides.size());
    for (const auto& item : param_names) {
        StateDictEntry entry;
        entry.name = item;
        entry.tensor = overrides.at(item).detach();
        state.push_back(std::move(entry));
    }
    return state;
}

void save_resume_checkpoint(const std::filesystem::path& checkpoint_path,
                            NativeRfDetrModel& model,
                            const NativeCheckpointMetadata& metadata,
                            const NativeOptimizer& optimizer,
                            const GradScaler& grad_scaler,
                            const TrainOptions& options,
                            int epoch,
                            double best_regular,
                            double best_ema,
                            const std::vector<std::string>& param_names,
                            const std::optional<ModelEma>& ema) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.total");
    std::filesystem::create_directories(checkpoint_path.parent_path());

    torch::serialize::OutputArchive archive;
    write_string(archive, "format", kNativeCheckpointFormat);
    write_int(archive, "format_version", kNativeCheckpointFormatVersion);
    write_string(archive, "preset_name", metadata.preset_name);
    write_string(archive, "source_kind", metadata.source_kind);
    write_string(archive, "source_path", metadata.source_path);
    write_int(archive, "num_classes", metadata.num_classes);
    if (metadata.sum_group_losses.has_value()) {
        write_bool(archive, "sum_group_losses", *metadata.sum_group_losses);
    }
    if (metadata.use_varifocal_loss.has_value()) {
        write_bool(archive, "use_varifocal_loss", *metadata.use_varifocal_loss);
    }
    if (metadata.use_position_supervised_loss.has_value()) {
        write_bool(archive, "use_position_supervised_loss", *metadata.use_position_supervised_loss);
    }
    if (metadata.ia_bce_loss.has_value()) {
        write_bool(archive, "ia_bce_loss", *metadata.ia_bce_loss);
    }
    if (metadata.aux_loss.has_value()) {
        write_bool(archive, "aux_loss", *metadata.aux_loss);
    }
    if (metadata.mask_point_sample_ratio.has_value()) {
        write_int(archive, "mask_point_sample_ratio", *metadata.mask_point_sample_ratio);
    }
    if (metadata.focal_alpha.has_value()) {
        write_double(archive, "focal_alpha", *metadata.focal_alpha);
    }
    if (metadata.cls_loss_coef.has_value()) {
        write_double(archive, "cls_loss_coef", *metadata.cls_loss_coef);
    }
    if (metadata.bbox_loss_coef.has_value()) {
        write_double(archive, "bbox_loss_coef", *metadata.bbox_loss_coef);
    }
    if (metadata.giou_loss_coef.has_value()) {
        write_double(archive, "giou_loss_coef", *metadata.giou_loss_coef);
    }
    if (metadata.mask_ce_loss_coef.has_value()) {
        write_double(archive, "mask_ce_loss_coef", *metadata.mask_ce_loss_coef);
    }
    if (metadata.mask_dice_loss_coef.has_value()) {
        write_double(archive, "mask_dice_loss_coef", *metadata.mask_dice_loss_coef);
    }
    if (metadata.set_cost_class.has_value()) {
        write_double(archive, "set_cost_class", *metadata.set_cost_class);
    }
    if (metadata.set_cost_bbox.has_value()) {
        write_double(archive, "set_cost_bbox", *metadata.set_cost_bbox);
    }
    if (metadata.set_cost_giou.has_value()) {
        write_double(archive, "set_cost_giou", *metadata.set_cost_giou);
    }
    std::vector<StateDictEntry> model_state;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.collect_state");
        model_state = collect_module_state(model);
    }
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.write_state");
        detail::write_state_archive(archive, "state", model_state);
    }

    torch::serialize::OutputArchive optimizer_archive;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.optimizer_state");
        optimizer.save(optimizer_archive);
    }
    write_string(archive, "optimizer_kind", optimizer.kind_name());
    archive.write("optimizer", optimizer_archive);

    if (ema.has_value()) {
        std::vector<StateDictEntry> ema_state;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.ema_collect_state");
            ema_state = ema_state_entries(param_names, *ema);
        }
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.ema_write_state");
            detail::write_state_archive(archive, "ema_state", ema_state);
        }
    }

    write_int(archive, "epoch", epoch);
    write_string(archive, "lr_scheduler", options.lr_scheduler);
    write_int(archive, "lr_drop", options.lr_drop);
    write_double(archive, "warmup_epochs", options.warmup_epochs);
    write_double(archive, "warmup_momentum", options.warmup_momentum);
    write_double(archive, "lr_min_factor", options.lr_min_factor);
    write_double(archive, "best_regular_metric", best_regular);
    write_double(archive, "best_ema_metric", best_ema);
    write_double(archive, "grad_scaler_scale", static_cast<double>(grad_scaler.current_scale()));
    write_int(archive, "grad_scaler_growth_tracker", grad_scaler.growth_tracker());
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume.archive_save_to");
        archive.save_to(checkpoint_path.string());
    }
}

ResumeState load_resume_checkpoint_state(const std::filesystem::path& checkpoint_path,
                                         NativeOptimizer& optimizer,
                                         GradScaler& grad_scaler,
                                         const TrainOptions& options) {
    if (!is_native_checkpoint_file(checkpoint_path)) {
        throw std::runtime_error("--resume requires a native RF-DETR .pt checkpoint: " + checkpoint_path.string());
    }

    torch::serialize::InputArchive archive;
    archive.load_from(checkpoint_path.string());

    if (const auto lr_scheduler = read_optional_string(archive, "lr_scheduler");
        lr_scheduler.has_value() && *lr_scheduler != options.lr_scheduler) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_scheduler does not match current training options");
    }
    if (const auto lr_drop = read_optional_int(archive, "lr_drop");
        lr_drop.has_value() && *lr_drop != options.lr_drop) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_drop does not match current training options");
    }
    if (const auto warmup_epochs = read_optional_double(archive, "warmup_epochs");
        warmup_epochs.has_value() && *warmup_epochs != options.warmup_epochs) {
        throw std::runtime_error("native RF-DETR resume checkpoint warmup_epochs does not match current training options");
    }
    if (const auto warmup_momentum = read_optional_double(archive, "warmup_momentum");
        warmup_momentum.has_value() && *warmup_momentum != options.warmup_momentum) {
        throw std::runtime_error("native RF-DETR resume checkpoint warmup_momentum does not match current training options");
    }
    if (const auto lr_min_factor = read_optional_double(archive, "lr_min_factor");
        lr_min_factor.has_value() && *lr_min_factor != options.lr_min_factor) {
        throw std::runtime_error("native RF-DETR resume checkpoint lr_min_factor does not match current training options");
    }
    const auto optimizer_kind = read_optional_string(archive, "optimizer_kind");
    if (!optimizer_kind.has_value()) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer_kind: " +
                                 checkpoint_path.string());
    }
    if (*optimizer_kind != optimizer.kind_name()) {
        throw std::runtime_error("native RF-DETR resume checkpoint optimizer_kind does not match current training options");
    }

    torch::serialize::InputArchive optimizer_archive;
    if (!archive.try_read("optimizer", optimizer_archive)) {
        throw std::runtime_error("native RF-DETR resume checkpoint is missing optimizer state: " +
                                 checkpoint_path.string());
    }
    try {
        optimizer.load(optimizer_archive);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "failed to load native RF-DETR optimizer state from " + checkpoint_path.string() + ": " + error.what());
    }

    ResumeState state;
    state.start_epoch = static_cast<int>(read_optional_int(archive, "epoch").value_or(-1) + 1);
    state.best_regular = read_optional_double(archive, "best_regular_metric")
                             .value_or(-std::numeric_limits<double>::infinity());
    state.best_ema = read_optional_double(archive, "best_ema_metric")
                         .value_or(-std::numeric_limits<double>::infinity());

    if (const auto scale = read_optional_double(archive, "grad_scaler_scale"); scale.has_value()) {
        grad_scaler.load_state(
            static_cast<float>(*scale),
            static_cast<int>(read_optional_int(archive, "grad_scaler_growth_tracker").value_or(0)));
    }

    torch::serialize::InputArchive ema_archive;
    if (archive.try_read("ema_state", ema_archive)) {
        state.ema_state = read_state_archive(archive, "ema_state");
    }
    return state;
}

DetectionConfig make_detection_config(const NativeRfDetrConfig& config,
                                      int64_t world_size,
                                      CompilationMode compilation_mode) {
    DetectionConfig detection_config;
    detection_config.num_classes = config.num_classes;
    detection_config.group_detr = config.group_detr;
    detection_config.dec_layers = config.dec_layers;
    detection_config.num_select = config.num_select;
    detection_config.world_size = std::max<int64_t>(1, world_size);
    detection_config.sum_group_losses = config.sum_group_losses;
    detection_config.use_varifocal_loss = config.use_varifocal_loss;
    detection_config.use_position_supervised_loss = config.use_position_supervised_loss;
    detection_config.ia_bce_loss = config.ia_bce_loss;
    detection_config.aux_loss = config.aux_loss;
    detection_config.two_stage = config.two_stage;
    detection_config.include_masks = config.segmentation;
    detection_config.use_jit_traced_loss_ops = (compilation_mode == CompilationMode::kSelective);
    detection_config.mask_point_sample_ratio = config.mask_point_sample_ratio;
    detection_config.focal_alpha = config.focal_alpha;
    detection_config.cls_loss_coef = config.cls_loss_coef;
    detection_config.bbox_loss_coef = config.bbox_loss_coef;
    detection_config.giou_loss_coef = config.giou_loss_coef;
    detection_config.mask_ce_loss_coef = config.mask_ce_loss_coef;
    detection_config.mask_dice_loss_coef = config.mask_dice_loss_coef;
    detection_config.set_cost_class = config.set_cost_class;
    detection_config.set_cost_bbox = config.set_cost_bbox;
    detection_config.set_cost_giou = config.set_cost_giou;
    populate_default_detection_weight_dict(detection_config);
    return detection_config;
}

size_t full_batches_per_rank(size_t total_images,
                             size_t batch_size,
                             size_t world_size,
                             int grad_accum_steps,
                             int train_lane_count) {
    const size_t total_full_batches = total_images / batch_size;
    const size_t local_full_batches = total_full_batches / std::max<size_t>(1, world_size);
    const size_t batches_per_step =
        static_cast<size_t>(std::max(1, grad_accum_steps)) * static_cast<size_t>(std::max(1, train_lane_count));
    return (local_full_batches / batches_per_step) * batches_per_step;
}

void average_gradients(const DistributedContext& distributed, const std::vector<torch::Tensor>& parameters) {
    if (!distributed.enabled) {
        return;
    }

    std::vector<torch::Tensor> gradients;
    gradients.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (parameter.grad().defined()) {
            gradients.push_back(parameter.grad());
        }
    }
    if (gradients.empty()) {
        return;
    }

    distributed_all_reduce_coalesced(distributed, gradients);
    const double scale = 1.0 / static_cast<double>(distributed.world_size);
    for (auto& gradient : gradients) {
        gradient.mul_(scale);
    }
}

std::future<TrainLaneResult> enqueue_train_lane(WorkerPool& lane_pool,
                                                WorkerPool* cpu_pool,
                                                DatasetLoader& loader,
                                                TrainLaneContext& lane,
                                                const Batch& batch,
                                                std::shared_ptr<CudaEventHandle> params_ready,
                                                double num_boxes_value,
                                                double scaled_loss_factor,
                                                size_t parameter_version,
                                                const DetectionConfig& detection_config,
                                                const NativeRfDetrModel& model,
                                                const std::vector<std::string>& parameter_names,
                                                int device_id,
                                                int image_height,
                                                int image_width,
                                                const torch::Tensor& mean,
                                                torch::Tensor std_tensor,
                                                bool amp_enabled,
                                                at::ScalarType autocast_dtype) {
    return lane_pool.enqueue([cpu_pool,
                              &loader,
                              &lane,
                              batch,
                              params_ready = std::move(params_ready),
                              num_boxes_value,
                              scaled_loss_factor,
                              parameter_version,
                              &detection_config,
                              &model,
                              &parameter_names,
                              device_id,
                              image_height,
                              image_width,
                              mean,
                              std_t = std::move(std_tensor),
                              amp_enabled,
                              autocast_dtype]() mutable {
        ScopedWorkerPool worker_scope(cpu_pool);
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.wait_batch");
            loader.wait_batch(batch);
        }

        void* consumer_stream = reinterpret_cast<void*>(lane.stream.stream());
        loader.handoff_batch(batch, consumer_stream);

        c10::cuda::CUDAGuard device_guard(checked_device_index(device_id));
        c10::cuda::CUDAStreamGuard stream_guard(lane.stream);

        torch::Tensor normalized;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.normalize");
            normalized = normalize_batch(batch, device_id, image_height, image_width, mean, std_t).contiguous();
        }
        loader.release_batch(batch, consumer_stream);

        PreparedTargets prepared;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.targets");
            prepared = build_targets(batch,
                                     image_height,
                                     image_width,
                                     detection_config.include_masks,
                                     detection_config.include_masks,
                                     device_id,
                                     lane.target_scratch);
        }

        if (!lane.model) {
            throw std::runtime_error("parallel RF-DETR train lane is missing its model replica");
        }
        if (lane.synced_parameter_version != parameter_version) {
            if (params_ready) {
                ensure_cuda_ok(
                    cudaStreamWaitEvent(lane.stream.stream(), params_ready->get(), 0),
                    "cudaStreamWaitEvent for parallel train parameter readiness");
            }
            copy_module_state(*lane.model, model);
            lane.synced_parameter_version = parameter_version;
        }
        lane.model->train();

        torch::Tensor detached_loss;
        torch::Tensor detached_class_loss;
        torch::Tensor detached_box_loss;
        std::vector<torch::Tensor> gradients;
        {
            TensorMap loss_dict;
            torch::Tensor loss;
            torch::Tensor class_loss;
            torch::Tensor box_loss;
            AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
            ModelOutputs outputs;
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.forward");
                outputs = lane.model->forward(NestedTensor{normalized, prepared.nested_mask});
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.targets_handoff");
                lane.target_scratch.handoff_pending_copy_to_current_stream(device_id);
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.loss_dict");
                loss_dict = detection_loss_dict(outputs, prepared, detection_config, true, num_boxes_value);
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.loss_total");
                loss = weighted_detection_loss(loss_dict, detection_config, normalized.device());
                class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
            }
            if (!torch::isfinite(loss).item<bool>()) {
                throw std::runtime_error(
                    "non-finite RF-DETR loss encountered during native training" +
                    format_nonfinite_loss_report(loss_dict, lane.grad_params, parameter_names));
            }
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.grad");
                gradients = torch::autograd::grad(
                    {loss * scaled_loss_factor},
                    lane.grad_params,
                    {},
                    std::nullopt,
                    false,
                    true);
            }
            detached_loss = loss.detach();
            detached_class_loss = class_loss.detach();
            detached_box_loss = box_loss.detach();
        }

        return TrainLaneResult{
            std::move(detached_loss),
            std::move(detached_class_loss),
            std::move(detached_box_loss),
            std::move(gradients),
            record_cuda_event(lane.stream.stream(), "parallel train lane completion event"),
        };
    });
}

EvalPassResult evaluate_model(const TrainOptions& options,
                              RuntimeContext& runtime,
                              NativeRfDetrModel& model,
                              const std::filesystem::path& compiled_path,
                              const DetectionConfig& detection_config,
                              std::optional<int> current_epoch,
                              std::string progress_label) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.eval.total");
    const size_t eval_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
    CocoDataset dataset = CocoDataset::load_from_binary(compiled_path);
    const std::vector<int> image_ids = dataset.image_ids();
    DatasetLoader loader(make_loader_config(compiled_path.string(),
                                            eval_batch_size,
                                            false,
                                            1,
                                            runtime.split().gather_threads,
                                            runtime.loader_affinity_string(),
                                            options.device_id,
                                            static_cast<uint64_t>(options.seed)));
    ScopedWorkerPool worker_scope(&runtime.cpu_pool());
    TargetScratch target_scratch;

    EvalPassResult result;
    torch::NoGradGuard no_grad;
    model.eval();

    const auto [mean, std] = make_normalization_tensors(options.device_id);
    const auto started_at = std::chrono::steady_clock::now();
    size_t image_count = 0;
    size_t batch_count = 0;
    size_t seen_images = 0;
    double loss_sum = 0.0;
    std::optional<CapturedEvalSample> captured_sample;
    const bool capture_eval_sample =
        current_epoch.has_value() && progress_label.starts_with("val ");
    std::unique_ptr<spdmon::ProgressBar> progress;
    if (options.progress_bar) {
        progress = std::make_unique<spdmon::ProgressBar>(std::move(progress_label), loader.num_images(), "img");
    }
    std::mt19937_64 sample_rng(
        static_cast<uint64_t>(options.seed) ^
        (current_epoch.has_value()
             ? (0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(*current_epoch + 1))
             : 0xd1b54a32d192ed03ULL));

    struct EvalPredictionLane {
        c10::cuda::CUDAStream stream;
        std::shared_ptr<PredictionBufferSlotPool> slot_pool;
    };

    WorkerPool lane_pool(static_cast<size_t>(runtime.split().lane_threads), runtime.lane_cpus(), "rfdtrnlane");
    WorkerPool& cpu_pool = runtime.cpu_pool();
    std::vector<EvalPredictionLane> lanes;
    lanes.reserve(static_cast<size_t>(runtime.split().lane_threads));
    c10::cuda::CUDAGuard device_guard(checked_device_index(options.device_id));
    const size_t slot_count = eval_prediction_slot_count(runtime.split());
    for (int lane_index = 0; lane_index < runtime.split().lane_threads; ++lane_index) {
        lanes.push_back(EvalPredictionLane{
            get_high_priority_cuda_stream(options.device_id),
            std::make_shared<PredictionBufferSlotPool>(slot_count),
        });
    }

    const int max_cpu_futures = std::max(2, runtime.split().cpu_threads);
    std::deque<std::future<StagedPredictionBatch>> lane_futures;
    std::deque<std::future<std::vector<Prediction>>> cpu_futures;
    size_t submitted_images = 0;
    auto drain_lane = [&]() {
        StagedPredictionBatch staged = lane_futures.front().get();
        lane_futures.pop_front();
        cpu_futures.push_back(cpu_pool.enqueue([staged = std::move(staged)]() mutable {
            return encode_staged_predictions(std::move(staged));
        }));
    };
    auto drain_cpu = [&]() {
        std::vector<Prediction> predictions = cpu_futures.front().get();
        cpu_futures.pop_front();
        dataset.add_predictions(std::move(predictions));
        ++image_count;
        if (progress) {
            progress->add(1);
        }
    };

    loader.begin_epoch();
    Batch batch{};
    while (loader.next_batch(batch)) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.eval.batch");
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.wait_batch");
            loader.wait_batch(batch);
        }
        LoaderBatchGuard batch_guard(loader, batch, options.device_id);
        MMLTK_PROFILE_ADD("rfdetr.train.eval.images", batch.num_images);

        torch::Tensor normalized;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.normalize");
            normalized =
                normalize_batch(batch, options.device_id, loader.image_height(), loader.image_width(), mean, std)
                    .contiguous();
        }

        PreparedTargets prepared;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.targets");
            prepared = build_targets(batch,
                                     static_cast<int>(loader.image_height()),
                                     static_cast<int>(loader.image_width()),
                                     detection_config.include_masks,
                                     detection_config.include_masks,
                                     options.device_id,
                                     target_scratch);
        }

        ModelOutputs outputs;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.forward");
            outputs = model.forward(NestedTensor{normalized, prepared.nested_mask});
        }

        TensorMap loss_dict;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.targets_handoff");
            target_scratch.handoff_pending_copy_to_current_stream(options.device_id);
        }

        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.loss_dict");
            loss_dict = detection_loss_dict(outputs, prepared, detection_config, false, false);
        }

        torch::Tensor loss;
        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.loss_total");
            loss = weighted_detection_loss(loss_dict, detection_config, normalized.device());
        }
        loss_sum += loss.item<double>();
        ++batch_count;

        auto processed = [&]() {
            MMLTK_PROFILE_SCOPE("rfdetr.train.eval.postprocess");
            return postprocess_outputs(outputs, prepared.orig_sizes, model.config().num_select);
        }();
        cudaEvent_t processed_ready_event = nullptr;
        ensure_cuda_ok(cudaEventCreateWithFlags(&processed_ready_event, cudaEventDisableTiming),
                       "cudaEventCreateWithFlags for train eval predictions");
        auto processed_ready = std::make_shared<CudaEventHandle>(processed_ready_event);
        ensure_cuda_ok(
            cudaEventRecord(processed_ready->get(), c10::cuda::getCurrentCUDAStream(checked_device_index(options.device_id)).stream()),
            "cudaEventRecord for train eval predictions");

        const size_t processed_count = std::min(processed.size(), batch.num_images);
        for (size_t image_pos = 0; image_pos < processed_count; ++image_pos) {
            const auto dataset_index = static_cast<int64_t>(batch.image_indices[image_pos]);
            const int image_id = static_cast<int>(image_id_for_dataset_index(image_ids, dataset_index));
            const size_t lane_index = submitted_images % lanes.size();

            if (capture_eval_sample) {
                ++seen_images;
                std::uniform_int_distribution<size_t> select_current(0, seen_images - 1);
                if (select_current(sample_rng) == 0) {
                    const auto& image_result = processed[image_pos];
                    const auto scores = image_result.at("scores");
                    const auto labels = image_result.at("labels");
                    const auto boxes = image_result.at("boxes");
                    const auto masks_found = image_result.find("masks");
                    const torch::Tensor masks =
                        masks_found != image_result.end() ? masks_found->second : torch::Tensor();
                    const auto keep = scores > 0.35f;

                    auto image_chw = make_device_batch_tensor(
                                         batch,
                                         options.device_id,
                                         loader.image_height(),
                                         loader.image_width())
                                         .select(0, static_cast<int64_t>(image_pos))
                                         .clone();
                    auto f_boxes = boxes.index({keep}).clone();
                    auto f_labels = labels.index({keep}).clone();
                    torch::Tensor f_masks;
                    if (masks.defined()) {
                        f_masks = masks.index({keep}).clone();
                    }
                    captured_sample = CapturedEvalSample{
                        std::move(image_chw),
                        std::move(f_boxes),
                        std::move(f_labels),
                        std::move(f_masks),
                    };
                }
            }

            TensorMap image_result = std::move(processed[image_pos]);
            lane_futures.push_back(lane_pool.enqueue([&lanes,
                                                      lane_index,
                                                      processed = std::move(image_result),
                                                      processed_ready,
                                                      image_id,
                                                      category_count = loader.num_classes(),
                                                      max_dets = options.eval_max_dets,
                                                      device_id = options.device_id]() mutable {
                auto& lane = lanes[lane_index];
                PredictionBufferLease lease = lane.slot_pool->acquire();
                ensure_cuda_ok(
                    cudaStreamWaitEvent(lane.stream.stream(), processed_ready->get(), 0),
                    "cudaStreamWaitEvent for train eval prediction staging");
                torch::NoGradGuard lane_no_grad;
                c10::cuda::CUDAGuard lane_device_guard(checked_device_index(device_id));
                c10::cuda::CUDAStreamGuard stream_guard(lane.stream);
                record_result_stream(processed, lane.stream);
                return stage_result_to_predictions(
                    image_id,
                    processed,
                    category_count,
                    max_dets,
                    std::move(lease),
                    device_id,
                    reinterpret_cast<void*>(lane.stream.stream()));
            }));
            ++submitted_images;
            while (static_cast<int>(lane_futures.size()) >= runtime.split().lane_threads) {
                drain_lane();
            }
            while (static_cast<int>(cpu_futures.size()) >= max_cpu_futures) {
                drain_cpu();
            }
        }
    }

    while (!lane_futures.empty()) {
        drain_lane();
        while (static_cast<int>(cpu_futures.size()) >= max_cpu_futures) {
            drain_cpu();
        }
    }
    while (!cpu_futures.empty()) {
        drain_cpu();
    }
    if (capture_eval_sample && captured_sample.has_value()) {
        RenderSampleOptions render_options;
        render_options.num_classes = model.config().num_classes;
        render_options.output_path =
            options.output_dir / "eval_samples" / ("epoch_" + std::to_string(*current_epoch + 1) + ".png");
        draw_eval_sample_async_gpu(captured_sample->image,
                                   captured_sample->boxes,
                                   captured_sample->labels,
                                   captured_sample->masks,
                                   render_options);
    }
    if (progress) {
        progress->close();
    }
    result.loss = batch_count > 0 ? loss_sum / static_cast<double>(batch_count) : 0.0;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.train.eval.metric");
        result.summary = dataset.evaluate(options.eval_max_dets);
    }
    flush_eval_sample_writes();
    result.timing = elapsed_timing(started_at, image_count);
    return result;
}

} // namespace

TrainRunResult run_training(const TrainOptions& options) {
    MMLTK_PROFILE_SCOPE("rfdetr.train.total");
    if (options.train_compiled_path.empty() || options.val_compiled_path.empty() || options.output_dir.empty()) {
        throw std::runtime_error(
            "RF-DETR train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const size_t selected_inputs =
        static_cast<size_t>(!options.weights_path.empty()) + static_cast<size_t>(!options.resume_path.empty());
    if (selected_inputs != 1) {
        throw std::runtime_error("RF-DETR train requires exactly one of --weights or --resume");
    }
    if (options.lr_scheduler != "step" && options.lr_scheduler != "cosine") {
        throw std::runtime_error("RF-DETR train lr_scheduler must be 'step' or 'cosine'");
    }
    if (options.batch_size == 0 || options.epochs <= 0 || options.grad_accum_steps <= 0) {
        throw std::runtime_error("RF-DETR train requires positive batch-size, epochs, and grad-accum-steps");
    }
    if (!options.distributed_worker && options.device_ids.size() > 1) {
        throw std::runtime_error("multi-GPU RF-DETR training must be launched through the CLI spawner");
    }

    DistributedContext distributed = make_distributed_context(options);
    const bool main_process = is_rank_zero(distributed);
    const int requested_train_lanes = effective_train_lanes(options);
    RuntimeContext train_runtime(resolve_runtime_config(options.workers, requested_train_lanes, options.cpu_affinity));
    const int train_lane_count = train_runtime.split().lane_threads;
    ScopedWorkerPool worker_scope(&train_runtime.cpu_pool());

    auto make_loader_config_for = [&](const std::filesystem::path& compiled_path,
                                      size_t loader_batch_size,
                                      bool shuffle,
                                      int prefetch_factor,
                                      bool shard_batches) {
        auto config = make_loader_config(compiled_path.string(),
                                         loader_batch_size,
                                         shuffle,
                                         prefetch_factor,
                                         train_runtime.split().gather_threads,
                                         train_runtime.loader_affinity_string(),
                                         options.device_id,
                                         static_cast<uint64_t>(options.seed));
        if (distributed.enabled && shard_batches) {
            config.batch_shard_rank = static_cast<uint32_t>(distributed.rank);
            config.batch_shard_count = static_cast<uint32_t>(distributed.world_size);
        }
        return config;
    };

    DatasetLoader train_loader(make_loader_config_for(options.train_compiled_path, options.batch_size, true, options.prefetch_factor, true));
    std::unique_ptr<DatasetLoader> val_loader;
    if (main_process) {
        const size_t val_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
        val_loader = std::make_unique<DatasetLoader>(make_loader_config_for(options.val_compiled_path, val_batch_size, false, 1, false));
    }
    std::unique_ptr<DatasetLoader> test_loader;
    if (main_process && !options.test_compiled_path.empty()) {
        const size_t val_batch_size = options.val_batch_size > 0 ? options.val_batch_size : options.batch_size;
        test_loader = std::make_unique<DatasetLoader>(
            make_loader_config(options.test_compiled_path.string(),
                               val_batch_size,
                               false,
                               1,
                               train_runtime.split().gather_threads,
                               train_runtime.loader_affinity_string(),
                               options.device_id,
                               static_cast<uint64_t>(options.seed)));
    }

    const auto source_checkpoint = !options.resume_path.empty() ? options.resume_path : options.weights_path;
    auto artifacts = resolve_upstream_weight_artifacts(source_checkpoint);
    const int dataset_output_classes = rfdetr_output_class_count(train_loader.num_classes());
    if (!options.resume_path.empty()) {
        const auto resume_checkpoint = load_checkpoint(options.resume_path);
        if (resume_checkpoint.metadata.num_classes > 0 &&
            resume_checkpoint.metadata.num_classes != static_cast<int64_t>(dataset_output_classes)) {
            throw std::runtime_error(
                "resume checkpoint class count does not match compiled dataset class count");
        }
    }
    artifacts.config.num_classes = dataset_output_classes;

    const auto validate_loader = [&](const DatasetLoader& loader, const char* split) {
        if (loader.image_width() != static_cast<uint32_t>(artifacts.config.resolution) ||
            loader.image_height() != static_cast<uint32_t>(artifacts.config.resolution)) {
            throw std::runtime_error(std::string(split) + " compiled resolution does not match RF-DETR preset");
        }
        if (loader.num_classes() != train_loader.num_classes()) {
            throw std::runtime_error("compiled class count mismatch across train/val/test splits");
        }
    };
    validate_loader(train_loader, "train");
    if (val_loader) {
        validate_loader(*val_loader, "val");
    }
    if (test_loader) {
        validate_loader(*test_loader, "test");
    }

    NativeRfDetrModel model(artifacts.config);
    model.to(cuda_device(options.device_id));
    const StateDictLoadSummary load_summary = model.load_weights(source_checkpoint, false);
    model.optimize_for_inference(
        checked_inference_batch_size(options.batch_size),
        true,
        options.compilation_mode);
    if (!options.val_compiled_path.empty()) {
        model.optimize_for_inference(
            checked_inference_batch_size(options.batch_size),
            false,
            options.compilation_mode);
    }
    if (main_process) {
        mmltk::logging::logger("rfdetr.train")->info(
            "rfdetr weights: loaded={} missing={} unexpected={} incompatible={} input={}",
            load_summary.loaded_names.size(),
            load_summary.missing_names.size(),
            load_summary.unexpected_names.size(),
            load_summary.incompatible_names.size(),
            source_checkpoint.string());
        for (const auto& name : load_summary.missing_names) {
            mmltk::logging::logger("rfdetr.train")->warn("  missing: {}", name);
        }
        for (const auto& name : load_summary.unexpected_names) {
            mmltk::logging::logger("rfdetr.train")->warn("  unexpected: {}", name);
        }
    }
    ensure_train_lane_model_supported(model, train_lane_count);
    model.train();

    if (options.freeze_encoder) {
        for (auto& item : model.named_parameters(true)) {
            if (is_encoder_param(item.key())) {
                item.value().set_requires_grad(false);
            }
        }
    }

    auto optimizer_build = build_optimizer(model, options);
    auto& optimizer = optimizer_build.optimizer;
    auto& all_params = optimizer.parameters();
    const auto& all_param_names = optimizer.parameter_names();

    const bool amp_enabled = options.amp;
    const auto autocast_dtype = amp_enabled ? resolve_cuda_autocast_dtype() : at::kFloat;
    const bool scaler_enabled = amp_enabled && autocast_dtype == torch::kFloat16;
    GradScaler grad_scaler(scaler_enabled);
    std::optional<ModelEma> ema;
    if (options.use_ema && main_process) {
        ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau));
    }

    int start_epoch = 0;
    double best_regular = -std::numeric_limits<double>::infinity();
    double best_ema = -std::numeric_limits<double>::infinity();
    if (!options.resume_path.empty()) {
        const ResumeState resume_state = load_resume_checkpoint_state(options.resume_path, optimizer, grad_scaler, options);
        start_epoch = resume_state.start_epoch;
        best_regular = resume_state.best_regular;
        best_ema = resume_state.best_ema;
        if (!resume_state.ema_state.empty() && main_process) {
            std::vector<torch::Tensor> shadow_params;
            shadow_params.reserve(resume_state.ema_state.size());
            for (const auto& entry : resume_state.ema_state) {
                shadow_params.push_back(entry.tensor.to(all_params.front().device()));
            }
            if (!ema.has_value()) {
                ema.emplace(all_params, options.ema_decay, static_cast<double>(options.ema_tau));
            }
            ema->load_shadow_params(shadow_params);
        }
    }

    LrScheduleConfig lr_config;
    lr_config.warmup_epochs = options.warmup_epochs;
    lr_config.warmup_momentum = options.warmup_momentum;
    lr_config.lr_scheduler = options.lr_scheduler;
    lr_config.lr_drop = options.lr_drop;
    lr_config.lr_min_factor = options.lr_min_factor;

    if (main_process) {
        mmltk::logging::logger("rfdetr.train")->info(
            "rfdetr train runtime: torch={} autocast={} optimizer={} optimizer_backend={} scaler={} train_lanes={} eval_lanes={} effective_batch_per_rank={} effective_batch_global={}",
            TORCH_VERSION,
            scalar_type_name(autocast_dtype),
            optimizer.kind_name(),
            optimizer.backend_name(),
            grad_scaler.enabled() ? "on" : "off",
            train_lane_count,
            train_runtime.split().lane_threads,
            effective_batch_per_rank(options, train_lane_count),
            effective_batch_global(options, distributed, train_lane_count));
        if (options.optimizer == TrainOptimizerKind::Muon && options.fused_optimizer) {
            mmltk::logging::logger("rfdetr.train")->warn(
                "rfdetr train runtime: optimizer=muon ignores --fused-optimizer and runs with the eager backend only");
        }
    }

    const size_t usable_full_batches = full_batches_per_rank(
        train_loader.num_images(),
        options.batch_size,
        static_cast<size_t>(distributed.world_size),
        options.grad_accum_steps,
        train_lane_count);
    if (usable_full_batches == 0) {
        throw std::runtime_error(
            "compiled train split is too small for one effective batch per rank; reduce batch_size, "
            "grad_accum_steps, --lanes, or world size");
    }
    const size_t batches_per_step = micro_batches_per_optimizer_step(options, train_lane_count);
    const auto steps_per_epoch =
        static_cast<int64_t>(usable_full_batches / batches_per_step);
    const int64_t total_training_steps = std::max<int64_t>(1, steps_per_epoch * options.epochs);
    DetectionConfig detection_config =
        make_detection_config(artifacts.config, distributed.world_size, options.compilation_mode);
    const auto [mean, std] = make_normalization_tensors(options.device_id);
    TargetScratch target_scratch;

    if (main_process) {
        std::filesystem::create_directories(options.output_dir);
    }
    const auto progress_path = options.output_dir / "progress.json";
    const auto log_path = options.output_dir / "log.txt";
    const auto results_path = options.output_dir / "results.json";
    const auto checkpoint_path = options.output_dir / "checkpoint.pt";
    const auto best_regular_checkpoint_path = options.output_dir / "checkpoint_best_regular.pt";
    const auto best_ema_checkpoint_path = options.output_dir / "checkpoint_best_ema.pt";
    const NativeCheckpointMetadata metadata = checkpoint_metadata(artifacts, dataset_output_classes);

    TrainRunResult result;
    result.artifacts = artifacts;
    result.output_dir = options.output_dir;
    result.checkpoint_path = checkpoint_path;
    result.best_regular_checkpoint_path = best_regular_checkpoint_path;
    result.best_ema_checkpoint_path = best_ema_checkpoint_path;
    if (main_process && std::isfinite(best_regular) && std::filesystem::exists(best_regular_checkpoint_path)) {
        result.best_checkpoint_path = best_regular_checkpoint_path;
        result.best_is_ema = false;
    }
    if (main_process && std::isfinite(best_ema) && std::filesystem::exists(best_ema_checkpoint_path) && best_ema >= best_regular) {
        result.best_checkpoint_path = best_ema_checkpoint_path;
        result.best_is_ema = true;
    }
    if (main_process) {
        std::error_code ignored_error;
        std::filesystem::remove(progress_path, ignored_error);
        std::filesystem::remove(log_path, ignored_error);
        std::filesystem::remove(results_path, ignored_error);
        write_json_file(
            progress_path,
            json{
                {"phase", "starting"},
                {"epoch", start_epoch},
                {"total_epochs", options.epochs},
                {"completed_batches", 0},
                {"total_batches", usable_full_batches},
                {"completed_waves", 0},
                {"optimizer_steps", 0},
                {"steps_per_epoch", steps_per_epoch},
                {"train_lanes", train_lane_count},
                {"train_loss", 0.0},
                {"class_loss", 0.0},
                {"box_loss", 0.0},
                {"step_loss", 0.0},
                {"step_class_loss", 0.0},
                {"step_box_loss", 0.0},
                {"batches_per_second", 0.0},
                {"images_per_second", 0.0},
                {"elapsed_seconds", 0.0},
                {"checkpoint_path", std::string{}},
                {"val_loss", 0.0},
                {"val", json()},
            });
    }

    c10::cuda::CUDAGuard device_guard(cuda_device_index(options.device_id));
    std::unique_ptr<WorkerPool> train_lane_pool;
    std::vector<TrainLaneContext> train_lanes;
    if (train_lane_count > 1) {
        train_lane_pool = std::make_unique<WorkerPool>(
            static_cast<size_t>(train_lane_count),
            train_runtime.lane_cpus(),
            "rfdtrtlane");
        train_lanes.reserve(static_cast<size_t>(train_lane_count));
        for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
            train_lanes.push_back(TrainLaneContext{
                get_high_priority_cuda_stream(options.device_id),
                {},
            });
        }
        for (auto& lane : train_lanes) {
            lane.model = make_train_lane_model(model, options.device_id);
            lane.model->optimize_for_inference(
                checked_inference_batch_size(options.batch_size),
                true,
                options.compilation_mode);
            lane.grad_params = lane_grad_parameters(*lane.model, all_param_names);
        }
    }
    size_t parameter_version = 0;
    distributed_barrier(distributed);
    for (int epoch = start_epoch; epoch < options.epochs; ++epoch) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.epoch");
        result.last_epoch = epoch;
        train_loader.begin_epoch();
        model.train();
        optimizer.zero_grad(true);

        auto loss_sum_device = torch::zeros(
            {},
            torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(options.device_id)));
        auto class_loss_sum_device = torch::zeros(
            {},
            torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(options.device_id)));
        auto box_loss_sum_device = torch::zeros(
            {},
            torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(options.device_id)));
        const auto epoch_started = std::chrono::steady_clock::now();
        int64_t local_micro_batches = 0;
        int64_t local_waves = 0;
        int64_t optimizer_steps = 0;
        size_t local_full_batches = 0;
        double host_loss_sum = 0.0;
        double host_class_loss_sum = 0.0;
        double host_box_loss_sum = 0.0;
        double current_step_loss = 0.0;
        double current_step_class_loss = 0.0;
        double current_step_box_loss = 0.0;

        std::unique_ptr<spdmon::ProgressBar> progress;
        if (main_process && options.progress_bar) {
            progress = std::make_unique<spdmon::ProgressBar>(
                phase_progress_label("train", epoch, options.epochs),
                static_cast<size_t>(usable_full_batches) * static_cast<size_t>(options.batch_size),
                "img");
            progress->set_postfix("cl=warming, bl=warming, l=warming");
        }

        auto write_progress_snapshot = [&](std::string_view phase,
                                           double val_loss,
                                           const json& val_summary,
                                           const std::filesystem::path& checkpoint_override) {
            if (!main_process) {
                return;
            }
            const double elapsed_seconds = std::chrono::duration<double>(
                                               std::chrono::steady_clock::now() - epoch_started)
                                               .count();
            const double average_class_loss =
                local_micro_batches > 0
                    ? host_class_loss_sum / static_cast<double>(local_micro_batches)
                    : 0.0;
            const double average_box_loss =
                local_micro_batches > 0
                    ? host_box_loss_sum / static_cast<double>(local_micro_batches)
                    : 0.0;
            const double average_loss =
                local_micro_batches > 0
                    ? host_loss_sum / static_cast<double>(local_micro_batches)
                    : 0.0;
            const double batches_per_second =
                elapsed_seconds > 0.0 ? static_cast<double>(local_micro_batches) / elapsed_seconds : 0.0;
            const double images_per_second =
                elapsed_seconds > 0.0
                    ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds
                    : 0.0;
            write_json_file(
                progress_path,
                json{
                    {"phase", std::string(phase)},
                    {"epoch", epoch},
                    {"total_epochs", options.epochs},
                    {"completed_batches", local_micro_batches},
                    {"total_batches", usable_full_batches},
                    {"completed_waves", local_waves},
                    {"optimizer_steps", optimizer_steps},
                    {"steps_per_epoch", steps_per_epoch},
                    {"train_lanes", train_lane_count},
                    {"train_loss", average_loss},
                    {"class_loss", average_class_loss},
                    {"box_loss", average_box_loss},
                    {"step_loss", current_step_loss},
                    {"step_class_loss", current_step_class_loss},
                    {"step_box_loss", current_step_box_loss},
                    {"batches_per_second", batches_per_second},
                    {"images_per_second", images_per_second},
                    {"elapsed_seconds", elapsed_seconds},
                    {"checkpoint_path",
                     checkpoint_override.empty() ? std::string{} : checkpoint_override.string()},
                    {"val_loss", val_loss},
                    {"val", val_summary},
                });
        };

        auto flush_progress = [&](bool force) {
            if (local_micro_batches == 0) {
                return;
            }
            const double average_class_loss = host_class_loss_sum / static_cast<double>(local_micro_batches);
            const double average_box_loss = host_box_loss_sum / static_cast<double>(local_micro_batches);
            const double average_loss = host_loss_sum / static_cast<double>(local_micro_batches);
            const double elapsed_seconds = std::chrono::duration<double>(
                                               std::chrono::steady_clock::now() - epoch_started)
                                               .count();
            const double images_per_second =
                elapsed_seconds > 0.0
                    ? static_cast<double>(local_micro_batches) * static_cast<double>(options.batch_size) / elapsed_seconds
                    : 0.0;
            const bool should_update_bar =
                static_cast<bool>(progress) &&
                (force || local_micro_batches % std::max(1, options.print_freq) == 0);
            if (should_update_bar) {
                progress->set_postfix(train_progress_postfix(
                    average_class_loss,
                    average_box_loss,
                    average_loss,
                    current_step_class_loss,
                    current_step_box_loss,
                    current_step_loss,
                    images_per_second,
                    optimizer_steps,
                    steps_per_epoch));
            }
            write_progress_snapshot("train", 0.0, json{}, std::filesystem::path{});
        };

        write_progress_snapshot("train", 0.0, json{}, std::filesystem::path{});

        const auto apply_optimizer_schedule = [&](int64_t current_step) {
            const double lr_scale = compute_lr_scale(lr_config, current_step, steps_per_epoch, total_training_steps);
            set_optimizer_lrs(optimizer, optimizer_build.base_lrs, lr_scale);
            if (options.optimizer == TrainOptimizerKind::Muon) {
                optimizer.set_muon_momentum(
                    compute_warmup_momentum(lr_config, current_step, steps_per_epoch, options.momentum));
            }
        };

        auto next_train_full_batch = [&](bool wait_now = true) -> std::optional<Batch> {
            Batch batch{};
            while (train_loader.next_batch(batch)) {
                MMLTK_PROFILE_SCOPE("rfdetr.train.batch");
                if (wait_now) {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.wait_batch");
                    train_loader.wait_batch(batch);
                }
                if (batch.num_images != options.batch_size) {
                    if (!wait_now) {
                        train_loader.wait_batch(batch);
                    }
                    train_loader.release_batch(batch);
                    continue;
                }
                if (local_full_batches >= usable_full_batches) {
                    if (!wait_now) {
                        train_loader.wait_batch(batch);
                    }
                    train_loader.release_batch(batch);
                    return std::nullopt;
                }
                ++local_full_batches;
                MMLTK_PROFILE_ADD("rfdetr.train.images", batch.num_images);
                MMLTK_PROFILE_ADD("rfdetr.train.full_batches", 1);
                return batch;
            }
            return std::nullopt;
        };

        std::shared_ptr<CudaEventHandle> params_ready;
        if (train_lane_count > 1) {
            params_ready = record_current_stream_event(
                options.device_id,
                "parallel train parameter readiness event");
        }

        if (train_lane_count <= 1) {
            while (true) {
                auto batch = next_train_full_batch();
                if (!batch.has_value()) {
                    break;
                }

                LoaderBatchGuard batch_guard(train_loader, *batch, options.device_id);
                torch::Tensor normalized;
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.normalize");
                    normalized =
                        normalize_batch(*batch,
                                        options.device_id,
                                        train_loader.image_height(),
                                        train_loader.image_width(),
                                        mean,
                                        std)
                            .contiguous();
                }

                torch::Tensor loss;
                torch::Tensor class_loss;
                torch::Tensor box_loss;
                TensorMap loss_dict;
                {
                    PreparedTargets prepared;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.targets");
                        prepared = build_targets(*batch,
                                                 static_cast<int>(train_loader.image_height()),
                                                 static_cast<int>(train_loader.image_width()),
                                                 detection_config.include_masks,
                                                 detection_config.include_masks,
                                                 options.device_id,
                                                 target_scratch);
                    }

                    AutocastGuard autocast_guard(amp_enabled, autocast_dtype);
                    ModelOutputs outputs;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.forward");
                        outputs = model.forward(NestedTensor{normalized, prepared.nested_mask});
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.targets_handoff");
                        target_scratch.handoff_pending_copy_to_current_stream(options.device_id);
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.loss_dict");
                        loss_dict = detection_loss_dict(
                            outputs,
                            prepared,
                            detection_config,
                            true,
                            distributed.enabled,
                            distributed.enabled
                                ? AllReduceTensorFn([&distributed](torch::Tensor& value) {
                                      distributed_all_reduce_tensor(distributed, value);
                                  })
                                : AllReduceTensorFn{});
                    }
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.loss_total");
                        loss = weighted_detection_loss(loss_dict, detection_config, normalized.device());
                        class_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_ce");
                        box_loss = loss_value_or_zero(loss_dict, normalized.device(), "loss_bbox");
                    }
                }
                if (!torch::isfinite(loss).item<bool>()) {
                    throw std::runtime_error(
                        "non-finite RF-DETR loss encountered during native training" +
                        format_nonfinite_loss_report(loss_dict, all_params, all_param_names));
                }

                const auto loss_value = loss.detach().item<double>();
                const auto class_loss_value = class_loss.detach().item<double>();
                const auto box_loss_value = box_loss.detach().item<double>();
                loss_sum_device.add_(loss.detach());
                class_loss_sum_device.add_(class_loss.detach());
                box_loss_sum_device.add_(box_loss.detach());
                host_loss_sum += loss_value;
                host_class_loss_sum += class_loss_value;
                host_box_loss_sum += box_loss_value;
                current_step_loss = loss_value;
                current_step_class_loss = class_loss_value;
                current_step_box_loss = box_loss_value;
                ++local_micro_batches;
                ++local_waves;
                const auto scaled_loss = grad_scaler.scale(loss / static_cast<double>(options.grad_accum_steps));
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.backward");
                    scaled_loss.backward();
                }

                if (local_waves % options.grad_accum_steps == 0) {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.optimizer");
                    const int64_t current_step = static_cast<int64_t>(epoch) * steps_per_epoch + optimizer_steps;
                    apply_optimizer_schedule(current_step);
                    average_gradients(distributed, all_params);
                    if (options.clip_max_norm > 0.0) {
                        grad_scaler.unscale_(optimizer);
                        torch::nn::utils::clip_grad_norm_(all_params, options.clip_max_norm);
                    }
                    grad_scaler.step(optimizer);
                    grad_scaler.update();
                    optimizer.zero_grad(true);
                    if (ema.has_value()) {
                        ema->update(all_params, optimizer_steps);
                    }
                    ++optimizer_steps;
                }

                if (progress) {
                    progress->add(static_cast<size_t>(options.batch_size));
                }
                flush_progress(false);
            }
        } else {
            while (local_full_batches < usable_full_batches) {
                const double current_scale = grad_scaler.enabled()
                    ? static_cast<double>(grad_scaler.current_scale())
                    : 1.0;
                const double scaled_loss_factor =
                    current_scale / static_cast<double>(micro_batches_per_optimizer_step(options, train_lane_count));
                std::vector<std::future<TrainLaneResult>> lane_futures;
                lane_futures.reserve(static_cast<size_t>(train_lane_count));
                double wave_loss_sum = 0.0;
                double wave_class_loss_sum = 0.0;
                double wave_box_loss_sum = 0.0;

                for (int lane_index = 0; lane_index < train_lane_count; ++lane_index) {
                    auto batch = next_train_full_batch(false);
                    if (!batch.has_value()) {
                        throw std::runtime_error(
                            "native RF-DETR training ended an epoch with an incomplete parallel train wave");
                    }

                    auto& lane = train_lanes[static_cast<size_t>(lane_index)];

                    const double num_boxes_value = resolve_num_boxes_value(
                        batch_target_instance_count(*batch),
                        detection_config,
                        true,
                        distributed,
                        cuda_device(options.device_id));
                    lane_futures.push_back(enqueue_train_lane(*train_lane_pool,
                                                              &train_runtime.cpu_pool(),
                                                              train_loader,
                                                              lane,
                                                              *batch,
                                                              params_ready,
                                                              num_boxes_value,
                                                              scaled_loss_factor,
                                                              parameter_version,
                                                              detection_config,
                                                              model,
                                                              all_param_names,
                                                              options.device_id,
                                                              static_cast<int>(train_loader.image_height()),
                                                              static_cast<int>(train_loader.image_width()),
                                                              mean,
                                                              std,
                                                              amp_enabled,
                                                              autocast_dtype));
                }

                for (auto& lane_future : lane_futures) {
                    TrainLaneResult lane_result = lane_future.get();
                    merge_lane_gradients(lane_result, all_params, options.device_id);
                    const auto lane_loss_value = lane_result.loss.item<double>();
                    const auto lane_class_loss_value = lane_result.class_loss.item<double>();
                    const auto lane_box_loss_value = lane_result.box_loss.item<double>();
                    loss_sum_device.add_(lane_result.loss);
                    class_loss_sum_device.add_(lane_result.class_loss);
                    box_loss_sum_device.add_(lane_result.box_loss);
                    host_loss_sum += lane_loss_value;
                    host_class_loss_sum += lane_class_loss_value;
                    host_box_loss_sum += lane_box_loss_value;
                    wave_loss_sum += lane_loss_value;
                    wave_class_loss_sum += lane_class_loss_value;
                    wave_box_loss_sum += lane_box_loss_value;
                    ++local_micro_batches;
                    if (progress) {
                        progress->add(static_cast<size_t>(options.batch_size));
                    }
                }

                current_step_loss = wave_loss_sum / static_cast<double>(lane_futures.size());
                current_step_class_loss = wave_class_loss_sum / static_cast<double>(lane_futures.size());
                current_step_box_loss = wave_box_loss_sum / static_cast<double>(lane_futures.size());
                ++local_waves;
                if (local_waves % options.grad_accum_steps == 0) {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.optimizer");
                    const int64_t current_step = static_cast<int64_t>(epoch) * steps_per_epoch + optimizer_steps;
                    apply_optimizer_schedule(current_step);
                    average_gradients(distributed, all_params);
                    if (options.clip_max_norm > 0.0) {
                        grad_scaler.unscale_(optimizer);
                        torch::nn::utils::clip_grad_norm_(all_params, options.clip_max_norm);
                    }
                    grad_scaler.step(optimizer);
                    grad_scaler.update();
                    optimizer.zero_grad(true);
                    if (ema.has_value()) {
                        ema->update(all_params, optimizer_steps);
                    }
                    ++optimizer_steps;
                    ++parameter_version;
                    params_ready = record_current_stream_event(
                        options.device_id,
                        "parallel train parameter readiness event");
                }
                flush_progress(false);
            }
        }

        {
            MMLTK_PROFILE_SCOPE("rfdetr.train.drain_loader");
            Batch drain_batch{};
            while (train_loader.next_batch(drain_batch)) {
                train_loader.release_batch(drain_batch);
            }
        }

        if (local_micro_batches == 0 || local_waves % options.grad_accum_steps != 0) {
            throw std::runtime_error("native RF-DETR training ended an epoch with incomplete gradient accumulation");
        }
        if (train_lane_count <= 1) {
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.wait_pending_copy");
                target_scratch.wait_for_pending_copy();
            }
        } else {
            MMLTK_PROFILE_SCOPE("rfdetr.train.parallel.wait_pending_copy");
            for (auto& lane : train_lanes) {
                lane.target_scratch.wait_for_pending_copy();
            }
        }
        flush_progress(true);
        if (progress) {
            progress->close();
        }

        auto reduced_loss_sum = loss_sum_device.detach().clone();
        auto reduced_micro_batches = torch::tensor(
            {static_cast<float>(local_micro_batches)},
            torch::TensorOptions().dtype(torch::kFloat32).device(cuda_device(options.device_id)));
        if (distributed.enabled) {
            distributed_all_reduce_tensor(distributed, reduced_loss_sum);
            distributed_all_reduce_tensor(distributed, reduced_micro_batches);
        }
        const double train_loss =
            reduced_loss_sum.item<double>() / std::max(1.0, reduced_micro_batches.item<double>());
        distributed_barrier(distributed);

        if (main_process) {
            write_progress_snapshot("validate", 0.0, json{}, std::filesystem::path{});
            auto val_result = evaluate_model(
                options, train_runtime, model, options.val_compiled_path, detection_config, epoch, phase_progress_label("val", epoch, options.epochs));

            mmltk::logging::logger("rfdetr.train")->info(
                "epoch {} stats: optimizer={} train_loss={:.6f} val_loss={:.6f} bbox_ap={:.4f} mask_ap={:.4f}",
                epoch + 1,
                optimizer.kind_name(),
                train_loss,
                val_result.loss,
                val_result.summary.bbox.ap,
                detection_config.include_masks ? val_result.summary.mask.ap : 0.0);
                         
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.epoch");
                NativeCheckpoint epoch_checkpoint;
                epoch_checkpoint.metadata = metadata;
                epoch_checkpoint.state_dict = collect_module_state(model);
                auto epoch_path = options.output_dir / ("checkpoint_epoch_" + std::to_string(epoch + 1) + ".pt");
                save_native_checkpoint(epoch_path, epoch_checkpoint);
            }

            TrainEpochSummary epoch_summary;
            epoch_summary.epoch = epoch;
            epoch_summary.train_loss = train_loss;
            epoch_summary.val_loss = val_result.loss;
            epoch_summary.val_summary = val_result.summary;

            const double regular_metric =
                detection_config.include_masks ? val_result.summary.mask.ap : val_result.summary.bbox.ap;
            if (regular_metric > best_regular) {
                best_regular = regular_metric;
                {
                    MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_regular");
                    NativeCheckpoint checkpoint;
                    checkpoint.metadata = metadata;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_regular.collect_state");
                        checkpoint.state_dict = collect_module_state(model);
                    }
                    save_native_checkpoint(best_regular_checkpoint_path, checkpoint);
                }
                result.best_is_ema = false;
                result.best_checkpoint_path = best_regular_checkpoint_path;
            }

            if (ema.has_value()) {
                std::vector<torch::Tensor> saved_params;
                saved_params.reserve(all_params.size());
                {
                    torch::NoGradGuard no_grad;
                    for (const auto& param : all_params) {
                        saved_params.push_back(param.detach().clone());
                    }
                    ema->copy_to(all_params);
                }
                auto ema_result = evaluate_model(
                    options,
                    train_runtime,
                    model,
                    options.val_compiled_path,
                    detection_config,
                    epoch,
                    phase_progress_label("ema", epoch, options.epochs));
                {
                    torch::NoGradGuard no_grad;
                    for (size_t index = 0; index < all_params.size(); ++index) {
                        all_params[index].copy_(saved_params[index]);
                    }
                }
                epoch_summary.ema_val_loss = ema_result.loss;
                epoch_summary.ema_val_summary = ema_result.summary;

                const double ema_metric =
                    detection_config.include_masks ? ema_result.summary.mask.ap : ema_result.summary.bbox.ap;
                if (ema_metric > best_ema) {
                    best_ema = ema_metric;
                    {
                        MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_ema");
                        NativeCheckpoint checkpoint;
                        checkpoint.metadata = metadata;
                        {
                            MMLTK_PROFILE_SCOPE("rfdetr.train.save.best_ema.collect_state");
                            const auto overrides = ema_override_map(all_param_names, *ema);
                            checkpoint.state_dict = collect_module_state(model, &overrides);
                        }
                        save_native_checkpoint(best_ema_checkpoint_path, checkpoint);
                    }
                    result.best_is_ema = true;
                    result.best_checkpoint_path = best_ema_checkpoint_path;
                }
            }

            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.resume");
                save_resume_checkpoint(checkpoint_path,
                                       model,
                                       metadata,
                                       optimizer,
                                       grad_scaler,
                                       options,
                                       epoch,
                                       best_regular,
                                       best_ema,
                                       all_param_names,
                                       ema);
            }

            result.history.push_back(epoch_summary);
            {
                MMLTK_PROFILE_SCOPE("rfdetr.train.save.log_json");
                append_json_line(
                    log_path,
                    json{
                        {"epoch", epoch},
                        {"train_lanes", train_lane_count},
                        {"eval_lanes", train_runtime.split().lane_threads},
                        {"effective_batch_per_rank", effective_batch_per_rank(options, train_lane_count)},
                        {"effective_batch_global", effective_batch_global(options, distributed, train_lane_count)},
                        {"train_loss", train_loss},
                        {"val_loss", val_result.loss},
                        {"val", eval_summary_json(val_result.summary)},
                        {"ema_val_loss", epoch_summary.ema_val_loss.value_or(0.0)},
                        {"ema_val", epoch_summary.ema_val_summary.has_value()
                                        ? eval_summary_json(*epoch_summary.ema_val_summary)
                                        : json()},
                    });
            }
            write_progress_snapshot(
                "epoch_complete",
                val_result.loss,
                eval_summary_json(val_result.summary),
                result.best_checkpoint_path.has_value() ? *result.best_checkpoint_path : checkpoint_path);
        }
        distributed_barrier(distributed);
    }

    if (main_process && !result.best_checkpoint_path.has_value()) {
        result.best_checkpoint_path = checkpoint_path;
    }

    if (main_process && test_loader) {
        NativeRfDetrModel best_model(artifacts.config);
        best_model.to(cuda_device(options.device_id));
        best_model.load_weights(*result.best_checkpoint_path, false);
        result.test_summary =
            evaluate_model(options, train_runtime, best_model, options.test_compiled_path, detection_config, std::nullopt, "test").summary;
    }

    if (main_process) {
        MMLTK_PROFILE_SCOPE("rfdetr.train.save.results_json");
        write_json_file(
            results_path,
            json{
                {"preset_name", artifacts.config.preset_name},
                {"output_dir", options.output_dir.string()},
                {"checkpoint", checkpoint_path.string()},
                {"best_checkpoint", result.best_checkpoint_path->string()},
                {"best_is_ema", result.best_is_ema},
                {"best_regular_metric", best_regular},
                {"best_ema_metric", best_ema},
                {"last_epoch", result.last_epoch},
                {"history_size", result.history.size()},
                {"train_lanes", train_lane_count},
                {"eval_lanes", train_runtime.split().lane_threads},
                {"effective_batch_per_rank", effective_batch_per_rank(options, train_lane_count)},
                {"effective_batch_global", effective_batch_global(options, distributed, train_lane_count)},
                {"test", result.test_summary.has_value() ? eval_summary_json(*result.test_summary) : json()},
            });
    }
    distributed_barrier(distributed);
    if (distributed.enabled) {
#if defined(USE_C10D_NCCL)
        distributed.process_group.reset();
        distributed.store.reset();
#endif
    }

    return result;
}

void print_training_summary(const TrainOptions& options, const TrainRunResult& result) {
    if (options.distributed_worker && options.distributed_rank != 0) {
        return;
    }
    const char* source_label = options.resume_path.empty() ? "weights" : "resume";
    const auto best_path = result.best_checkpoint_path.has_value() ? result.best_checkpoint_path->string() : "";
    const auto checkpoint_path = result.checkpoint_path.string();
    const double train_loss = result.history.empty() ? 0.0 : result.history.back().train_loss;
    const double val_loss = result.history.empty() ? 0.0 : result.history.back().val_loss;
    const double val_bbox_ap = result.history.empty() ? 0.0 : result.history.back().val_summary.bbox.ap;
    const double val_mask_ap = result.history.empty() ? 0.0 : result.history.back().val_summary.mask.ap;
    mmltk::logging::logger("rfdetr.train")->info(
        "rfdetr train[{}]: preset={} optimizer={} epochs={} train_loss={:.6f} val_loss={:.6f} val_bbox_ap={:.4f} val_mask_ap={:.4f} best={} checkpoint={}",
        source_label,
        result.artifacts.config.preset_name,
        train_optimizer_kind_name(options.optimizer),
        result.last_epoch + 1,
        train_loss,
        val_loss,
        val_bbox_ap,
        val_mask_ap,
        best_path,
        checkpoint_path);
}

} // namespace mmltk::rfdetr
