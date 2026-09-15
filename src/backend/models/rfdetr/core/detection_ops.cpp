#include <cctype>
#include "detail/detection_ops.h"

#include <ATen/TensorIndexing.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/nn/functional/activation.h>
#include <torch/nn/functional/loss.h>
#include <torch/nn/functional/upsampling.h>
#include <torch/nn/functional/vision.h>
#include <torch/script.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#include "detail/detection_geometry.h"
#include "detail/detr_matcher_cuda.h"
#include "detail/scipy_rectangular_lsap.h"
#include "detail/lsap_scratch.h"
#include "detail/matcher_workspace.h"
#include "detail/traced_loss_cache.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/numa_memory.h"
#include "src/frameworks/gpu/device_execution.h"
#include <span>
#include "detail/training_mask_ops_cuda.h"

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.backend.models.rfdetr.core.runtime;
#include "src/frameworks/gpu/cuda_error.h"

namespace F = torch::nn::functional;
using namespace torch::indexing;

namespace mmltk::backend::models::rfdetr {

using mmltk::frameworks::gpu::ensure_cuda_ok;

namespace {

constexpr double kSanitizedCostMargin = 1.0;

using MatchIndices = std::vector<std::pair<torch::Tensor, torch::Tensor>>;

class MatcherAccess final {
   public:
    explicit MatcherAccess(const torch::Device& device) {
        if (auto* workspace = active_matcher_workspace()) {
            workspace_ = workspace;
            return;
        }
        const auto topology = mmltk::common::system::NumaTopology::Capture();
        const auto placement =
            device.is_cuda()
                ? mmltk::frameworks::gpu::resolve_device_execution(
                      device.index(), topology, mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy()))
                      .placement
                : mmltk::common::system::resolve_placement(topology, topology.permitted_nodes.front());
        policy_.emplace(mmltk::common::system::ExecutionPolicyRequest{placement.cpus, {}, 0, placement.numa_node, -10, false});
        local_ = std::make_unique<MatcherWorkspace>(placement.numa_node);
        workspace_ = local_.get();
        scope_.emplace(nullptr, 0, workspace_);
    }
    MatcherWorkspace& get() { return *workspace_; }

   private:
    std::optional<mmltk::common::system::ScopedExecutionPolicy> policy_;
    std::unique_ptr<MatcherWorkspace> local_;
    MatcherWorkspace* workspace_{};
    std::optional<ScopedRuntimeContext> scope_;
};

TracedLossOpCache& traced_loss_op_cache() {
    auto* workspace = active_matcher_workspace();
    if (!workspace) throw std::logic_error("criterion trace requires an active matcher workspace");
    return workspace->loss_cache();
}

torch::Tensor make_cpu_int64_tensor(std::span<const int64_t> values) {
    auto tensor = torch::empty({static_cast<int64_t>(values.size())}, torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU));
    if (!values.empty()) { std::copy(values.begin(), values.end(), tensor.data_ptr<int64_t>()); }
    return tensor;
}

template <size_t Arity, typename TraceFn, typename... Tensors>
void ensure_loss_trace(TracedLossOp<Arity>& cache, const char* class_name, TraceFn&& fn, const Tensors&... tensors) {
    static_assert(sizeof...(Tensors) == Arity);
    if (cache.matches(tensors...)) { return; }

    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create(class_name, cu, true);
    cache.module = torch::jit::Module(cu, cls);

    auto trace_res = torch::jit::tracer::trace(
        {tensors.detach().contiguous()...},
        [&](torch::jit::Stack args) -> torch::jit::Stack {
            return [&]<size_t... I>(std::index_sequence<I...>) -> torch::jit::Stack {
                return {fn(args[I].toTensor()...)};
            }(std::make_index_sequence<Arity>{});
        },
        [](const torch::autograd::Variable&) { return ""; }, false, false, &cache.module);
    cache.module.type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));
    cache.record_signature(tensors...);
}

template <typename TraceFn>
void ensure_parametric_binary_loss_trace(TracedParametricBinaryLossOp& cache, const char* class_name, const torch::Tensor& input,
                                         const torch::Tensor& target, double alpha, double gamma, TraceFn&& fn) {
    ensure_loss_trace(cache, class_name, std::forward<TraceFn>(fn), input, target);
    cache.alpha = alpha;
    cache.gamma = gamma;
}

torch::Tensor point_sample(const torch::Tensor& input, const torch::Tensor& point_coords, F::GridSampleFuncOptions::mode_t mode);

torch::Tensor matcher_point_sample(const torch::Tensor& input, const torch::Tensor& point_coords, F::GridSampleFuncOptions::mode_t mode);

const PackedTargetMasks& require_target_masks(const PreparedTargets& targets, const char* context) {
    if (!targets.packed_masks.has_value() || !targets.packed_masks->bits.defined()) {
        throw std::runtime_error(std::string(context) + " requires target masks");
    }
    return *targets.packed_masks;
}

int64_t nearest_grid_sample_index(float coord, int64_t size) {
    const float source = coord * static_cast<float>(size) - 0.5f;
    const auto index = static_cast<int64_t>(std::nearbyint(source));
    return std::clamp<int64_t>(index, 0, size - 1);
}

torch::Tensor sample_packed_target_masks_cpu(const PackedTargetMasks& masks, const torch::Tensor& mask_indices,
                                             const torch::Tensor& point_coords, const char* context) {
    if (point_coords.dim() != 3 || point_coords.size(2) != 2) {
        throw std::runtime_error(std::string(context) + " expects point coordinates shaped [batch, points, 2]");
    }
    if (!masks.bits.defined() || masks.bits.dim() != 2) {
        throw std::runtime_error(std::string(context) + " expects packed target masks shaped [instances, words]");
    }
    if (masks.height <= 0 || masks.width <= 0) {
        throw std::runtime_error(std::string(context) + " requires positive packed mask dimensions");
    }
    if (masks.bits.size(0) == 0) {
        return torch::empty({0, point_coords.size(1)}, torch::TensorOptions().dtype(torch::kFloat32).device(point_coords.device()));
    }
    if (!point_coords.device().is_cpu()) {
        throw std::runtime_error(std::string(context) + " CPU packed mask sampling requires CPU point coordinates");
    }
    if (!mask_indices.device().is_cpu() || mask_indices.scalar_type() != torch::kInt64 || mask_indices.dim() != 1) {
        throw std::runtime_error(std::string(context) + " CPU packed mask sampling requires 1D CPU int64 mask indices");
    }

    const auto masks_contiguous = masks.bits.contiguous();
    const auto indices = mask_indices.contiguous();
    const auto coords =
        point_coords.scalar_type() == torch::kFloat32 ? point_coords.contiguous() : point_coords.to(torch::kFloat32).contiguous();
    if (coords.size(0) != 1 && coords.size(0) != indices.size(0)) {
        throw std::runtime_error(std::string(context) + " point coordinate batch must match sampled masks or be shared");
    }
    auto output = torch::empty({indices.size(0), coords.size(1)}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU));
    const auto* words = reinterpret_cast<const uint64_t*>(masks_contiguous.data_ptr<int64_t>());
    const auto* index_data = indices.data_ptr<int64_t>();
    const auto* coords_data = coords.data_ptr<float>();
    auto* output_data = output.data_ptr<float>();
    const int64_t words_per_mask = masks_contiguous.size(1);
    const int64_t num_points = coords.size(1);
    for (int64_t mask_slot = 0; mask_slot < indices.size(0); ++mask_slot) {
        const int64_t coord_batch = coords.size(0) == 1 ? 0 : mask_slot;
        const int64_t mask_index = index_data[mask_slot];
        const auto* mask_words = words + mask_index * words_per_mask;
        for (int64_t point_index = 0; point_index < num_points; ++point_index) {
            const auto* coord_ptr = coords_data + (coord_batch * num_points + point_index) * 2;
            const int64_t x = nearest_grid_sample_index(coord_ptr[0], masks.width);
            const int64_t y = nearest_grid_sample_index(coord_ptr[1], masks.height);
            const int64_t pixel_index = y * masks.width + x;
            const int64_t word_index = pixel_index >> 6;
            const int64_t bit_index = pixel_index & 63;
            output_data[mask_slot * num_points + point_index] = ((mask_words[word_index] >> bit_index) & uint64_t{1}) != 0 ? 1.0f : 0.0f;
        }
    }
    return output;
}

}  // namespace

torch::Tensor sample_target_masks(const PackedTargetMasks& masks, const torch::Tensor& mask_indices, const torch::Tensor& point_coords,
                                  const char* context) {
    const auto indices =
        mask_indices.scalar_type() == torch::kInt64 ? mask_indices.contiguous() : mask_indices.to(torch::kInt64).contiguous();
    const auto coords =
        point_coords.scalar_type() == torch::kFloat32 ? point_coords.contiguous() : point_coords.to(torch::kFloat32).contiguous();
    if (masks.bits.device() != coords.device()) {
        throw std::runtime_error(std::string(context) + " requires packed masks and point coordinates on the same device");
    }
    if (masks.bits.device() != indices.device()) {
        throw std::runtime_error(std::string(context) + " requires packed masks and mask indices on the same device");
    }
    if (masks.bits.device().is_cuda()) {
        return sample_packed_masks_cuda(masks.bits, masks.height, masks.width, indices, coords, masks.inverse_transforms,
                                        masks.occluder_mask_indices, masks.occluder_inverse_transforms, masks.erasure);
    }
    if (masks.inverse_transforms.defined() || masks.erasure.defined()) {
        throw std::runtime_error(std::string(context) + " cannot transform packed masks on CPU");
    }
    return sample_packed_target_masks_cpu(masks, indices, coords, context);
}

namespace {

void copy_sanitized_cost_to_double(const torch::Tensor& cost_cpu, const torch::Tensor& cost_double) {
    if (!cost_cpu.device().is_cpu() || cost_cpu.scalar_type() != torch::kFloat32 || cost_cpu.dim() != 2) {
        throw std::runtime_error("native RF-DETR matcher expects a 2D CPU float cost matrix");
    }
    auto* output = cost_double.data_ptr<double>();
    const auto* input = cost_cpu.data_ptr<float>();
    const int64_t rows = cost_cpu.size(0);
    const int64_t cols = cost_cpu.size(1);
    const int64_t row_stride = cost_cpu.stride(0);
    const int64_t col_stride = cost_cpu.stride(1);
    bool found_nonfinite = false;
    bool found_finite = false;
    float finite_max = -std::numeric_limits<float>::infinity();
    float finite_abs_max = 0.0f;
    for (int64_t row = 0; row < rows; ++row) {
        for (int64_t col = 0; col < cols; ++col) {
            const float value = input[row * row_stride + col * col_stride];
            output[row * cols + col] = static_cast<double>(value);
            if (!std::isfinite(value)) {
                found_nonfinite = true;
                continue;
            }
            found_finite = true;
            finite_max = std::max(finite_max, value);
            finite_abs_max = std::max(finite_abs_max, std::abs(value));
        }
    }
    if (!found_nonfinite) { return; }

    float replacement = std::numeric_limits<float>::max();
    if (found_finite) {
        const double candidate = static_cast<double>(finite_max) + static_cast<double>(finite_abs_max) + kSanitizedCostMargin;
        if (std::isfinite(candidate)) { replacement = static_cast<float>(std::min(candidate, static_cast<double>(replacement))); }
    }
    for (int64_t index = 0; index < rows * cols; ++index) {
        if (!std::isfinite(output[index])) { output[index] = static_cast<double>(replacement); }
    }
}

using BinaryLossFn = std::function<torch::Tensor(const torch::Tensor&, const torch::Tensor&)>;
using TernaryLossFn = std::function<torch::Tensor(const torch::Tensor&, const torch::Tensor&, const torch::Tensor&)>;

torch::Tensor run_binary_traced_or_direct(TracedLossOp<2> TracedLossOpCache::* cache_member, const char* class_name,
                                          const bool use_jit_traced, const BinaryLossFn& direct_fn, const torch::Tensor& first,
                                          const torch::Tensor& second) {
    if (use_jit_traced) {
        auto& slot = traced_loss_op_cache().*cache_member;
        ensure_loss_trace(slot, class_name, direct_fn, first, second);
        return slot.module.forward({first, second}).toTensor();
    }
    return direct_fn(first, second);
}

torch::Tensor run_ternary_traced_or_direct(TracedLossOp<3> TracedLossOpCache::* cache_member, const char* class_name,
                                           const bool use_jit_traced, const TernaryLossFn& direct_fn, const torch::Tensor& first,
                                           const torch::Tensor& second, const torch::Tensor& third) {
    if (use_jit_traced) {
        auto& slot = traced_loss_op_cache().*cache_member;
        ensure_loss_trace(slot, class_name, direct_fn, first, second, third);
        return slot.module.forward({first, second, third}).toTensor();
    }
    return direct_fn(first, second, third);
}

torch::Tensor run_parametric_traced_or_direct(TracedParametricBinaryLossOp TracedLossOpCache::* cache_member, const char* class_name,
                                              const torch::Tensor& inputs, const torch::Tensor& targets, const double alpha,
                                              const double gamma, const bool use_jit_traced, const BinaryLossFn& direct_fn) {
    if (use_jit_traced) {
        auto& slot = traced_loss_op_cache().*cache_member;
        ensure_parametric_binary_loss_trace(slot, class_name, inputs, targets, alpha, gamma, direct_fn);
        return slot.module.forward({inputs, targets}).toTensor();
    }
    return direct_fn(inputs, targets);
}

torch::Tensor binary_cross_entropy_with_logits_none(const torch::Tensor& inputs, const torch::Tensor& targets) {
    return F::binary_cross_entropy_with_logits(inputs, targets, F::BinaryCrossEntropyWithLogitsFuncOptions().reduction(torch::kNone));
}

torch::Tensor sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, double num_masks, bool use_jit_traced_loss_ops) {
    return run_binary_traced_or_direct(
               &TracedLossOpCache::sigmoid_ce, "__torch__.NativeRfDetrSigmoidCeLoss", use_jit_traced_loss_ops,
               [](const torch::Tensor& a, const torch::Tensor& b) { return binary_cross_entropy_with_logits_none(a, b).mean(1).sum(); },
               inputs, targets) /
           num_masks;
}

torch::Tensor dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, double num_masks, bool use_jit_traced_loss_ops) {
    return run_binary_traced_or_direct(
               &TracedLossOpCache::dice, "__torch__.NativeRfDetrDiceLoss", use_jit_traced_loss_ops,
               [](const torch::Tensor& a, const torch::Tensor& b) {
                   const auto probs = a.sigmoid().flatten(1);
                   const auto flat_targets = b.flatten(1);
                   const auto numerator = 2 * (probs * flat_targets).sum(-1);
                   const auto denominator = probs.sum(-1) + flat_targets.sum(-1);
                   return (1 - (numerator + 1) / (denominator + 1)).sum();
               },
               inputs, targets) /
           num_masks;
}

torch::Tensor batch_dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool use_jit_traced_loss_ops) {
    return run_binary_traced_or_direct(
        &TracedLossOpCache::batch_dice, "__torch__.NativeRfDetrBatchDiceLoss", use_jit_traced_loss_ops,
        [](const torch::Tensor& a, const torch::Tensor& b) {
            const auto probs = a.sigmoid().flatten(1);
            const auto flat_targets = b.flatten(1);
            const auto numerator = 2 * torch::einsum("nc,mc->nm", {probs, flat_targets});
            const auto denominator = probs.sum(-1).unsqueeze(1) + flat_targets.sum(-1).unsqueeze(0);
            return 1 - (numerator + 1) / (denominator + 1);
        },
        inputs, targets);
}

torch::Tensor batch_sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool use_jit_traced_loss_ops) {
    return run_binary_traced_or_direct(
        &TracedLossOpCache::batch_sigmoid_ce, "__torch__.NativeRfDetrBatchSigmoidCeLoss", use_jit_traced_loss_ops,
        [](const torch::Tensor& a, const torch::Tensor& b) {
            const auto flat_targets = b.flatten(1);
            const auto positives = binary_cross_entropy_with_logits_none(a, torch::ones_like(a));
            const auto negatives = binary_cross_entropy_with_logits_none(a, torch::zeros_like(a));
            return (torch::einsum("nc,mc->nm", {positives, flat_targets}) + torch::einsum("nc,mc->nm", {negatives, 1 - flat_targets})) /
                   static_cast<double>(flat_targets.size(1));
        },
        inputs, targets);
}

torch::Tensor point_sample(const torch::Tensor& input, const torch::Tensor& point_coords,
                           F::GridSampleFuncOptions::mode_t mode = torch::kBilinear) {
    torch::Tensor grid = point_coords;
    bool add_dim = false;
    if (grid.dim() == 3) {
        add_dim = true;
        grid = grid.unsqueeze(2);
    }
    auto output =
        F::grid_sample(input, 2.0 * grid - 1.0, F::GridSampleFuncOptions().mode(mode).padding_mode(torch::kBorder).align_corners(false));
    if (add_dim) { output = output.squeeze(3); }
    return output;
}

torch::Tensor matcher_point_sample(const torch::Tensor& input, const torch::Tensor& point_coords, F::GridSampleFuncOptions::mode_t mode) {
    if (input.is_cuda() && input.scalar_type() == torch::kFloat32 && point_coords.scalar_type() == torch::kFloat32) {
        return matcher_point_sample_cuda_forward(input.contiguous(), point_coords.contiguous(),
                                                 std::holds_alternative<torch::enumtype::kNearest>(mode));
    }
    return point_sample(input, point_coords, mode);
}

torch::Tensor calculate_uncertainty(const torch::Tensor& logits) { return -torch::abs(logits); }

torch::Tensor get_uncertain_point_coords_with_randomness(const torch::Tensor& coarse_logits, int64_t num_points,
                                                         int64_t oversample_ratio = 3, double importance_sample_ratio = 0.75) {
    if (oversample_ratio < 1) { throw std::runtime_error("mask oversample_ratio must be at least 1"); }
    if (importance_sample_ratio < 0.0 || importance_sample_ratio > 1.0) {
        throw std::runtime_error("mask importance_sample_ratio must be in [0, 1]");
    }

    const int64_t num_boxes = coarse_logits.size(0);
    const int64_t num_sampled = num_points * oversample_ratio;
    auto point_coords =
        torch::rand({num_boxes, num_sampled, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(coarse_logits.device()));
    const auto point_logits = point_sample(coarse_logits, point_coords, torch::kBilinear);
    const auto point_uncertainties = calculate_uncertainty(point_logits);
    const auto num_uncertain_points = static_cast<int64_t>(importance_sample_ratio * static_cast<double>(num_points));
    const int64_t num_random_points = num_points - num_uncertain_points;

    torch::Tensor sampled_coords;
    if (num_uncertain_points > 0) {
        auto idx = std::get<1>(point_uncertainties.index({Slice(), 0, Slice()}).topk(num_uncertain_points, 1));
        const auto shift =
            num_sampled * torch::arange(num_boxes, torch::TensorOptions().dtype(torch::kInt64).device(coarse_logits.device()));
        idx = idx + shift.unsqueeze(1);
        sampled_coords = point_coords.view({-1, 2}).index({idx.reshape({-1})}).view({num_boxes, num_uncertain_points, 2});
    } else {
        sampled_coords = torch::empty({num_boxes, 0, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(coarse_logits.device()));
    }

    if (num_random_points > 0) {
        sampled_coords = torch::cat(
            {
                sampled_coords,
                torch::rand({num_boxes, num_random_points, 2},
                            torch::TensorOptions().dtype(torch::kFloat32).device(coarse_logits.device())),
            },
            1);
    }
    return sampled_coords;
}

torch::Tensor sigmoid_focal_loss(const torch::Tensor& inputs, const torch::Tensor& targets, double num_boxes, double alpha, double gamma,
                                 bool use_jit_traced_loss_ops) {
    return run_parametric_traced_or_direct(&TracedLossOpCache::sigmoid_focal, "__torch__.NativeRfDetrSigmoidFocalLoss", inputs, targets,
                                           alpha, gamma, use_jit_traced_loss_ops,
                                           [alpha, gamma](const torch::Tensor& a, const torch::Tensor& b) {
                                               const auto prob = a.sigmoid();
                                               const auto ce_loss = binary_cross_entropy_with_logits_none(a, b);
                                               const auto p_t = prob * b + (1 - prob) * (1 - b);
                                               auto loss = ce_loss * torch::pow(1 - p_t, gamma);
                                               if (alpha >= 0.0) {
                                                   const auto alpha_t = alpha * b + (1 - alpha) * (1 - b);
                                                   loss = alpha_t * loss;
                                               }
                                               return loss.mean(1).sum();
                                           }) /
           num_boxes;
}

torch::Tensor sigmoid_varifocal_loss(const torch::Tensor& inputs, const torch::Tensor& targets, double num_boxes, double alpha,
                                     double gamma, bool use_jit_traced_loss_ops) {
    return run_parametric_traced_or_direct(&TracedLossOpCache::sigmoid_varifocal, "__torch__.NativeRfDetrSigmoidVarifocalLoss", inputs,
                                           targets, alpha, gamma, use_jit_traced_loss_ops,
                                           [alpha, gamma](const torch::Tensor& a, const torch::Tensor& b) {
                                               const auto prob = a.sigmoid();
                                               const auto focal_weight =
                                                   b * b.gt(0.0).to(a.dtype()) +
                                                   (1 - alpha) * torch::pow((prob - b).abs(), gamma) * b.le(0.0).to(a.dtype());
                                               const auto ce_loss = binary_cross_entropy_with_logits_none(a, b);
                                               return (ce_loss * focal_weight).mean(1).sum();
                                           }) /
           num_boxes;
}

torch::Tensor position_supervised_loss(const torch::Tensor& inputs, const torch::Tensor& targets, double num_boxes, double alpha,
                                       double gamma, bool use_jit_traced_loss_ops) {
    return run_parametric_traced_or_direct(
               &TracedLossOpCache::position_supervised, "__torch__.NativeRfDetrPositionSupervisedLoss", inputs, targets, alpha, gamma,
               use_jit_traced_loss_ops,
               [alpha, gamma](const torch::Tensor& a, const torch::Tensor& b) {
                   const auto prob = a.sigmoid();
                   auto loss = binary_cross_entropy_with_logits_none(a, b) * torch::pow((b - prob).abs(), gamma);
                   if (alpha >= 0.0) {
                       const auto alpha_t = alpha * b.gt(0.0).to(a.dtype()) + (1 - alpha) * b.le(0.0).to(a.dtype());
                       loss = alpha_t * loss;
                   }
                   return loss.mean(1).sum();
               }) /
           num_boxes;
}

torch::Tensor ia_bce_loss(const torch::Tensor& inputs, const torch::Tensor& pos_weights, const torch::Tensor& neg_weights, double num_boxes,
                          bool use_jit_traced_loss_ops) {
    return run_ternary_traced_or_direct(
               &TracedLossOpCache::ia_bce, "__torch__.NativeRfDetrIaBceLoss", use_jit_traced_loss_ops,
               [](const torch::Tensor& a, const torch::Tensor& b, const torch::Tensor& c) {
                   return (c * a - F::logsigmoid(a) * (b + c)).sum();
               },
               inputs, pos_weights, neg_weights) /
           num_boxes;
}

torch::Tensor accuracy_top1(const torch::Tensor& output, const torch::Tensor& target) {
    if (target.numel() == 0) { return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(output.device())); }
    const auto topk = output.topk(1, 1, true, true);
    const auto pred = std::get<1>(topk).squeeze(1);
    const auto correct = pred.eq(target).to(torch::kFloat32).sum();
    return correct * (100.0 / static_cast<double>(target.size(0)));
}

torch::Tensor concat_target_labels(const PreparedTargets& targets, const MatcherLayerIndices& indices, const torch::Device& device) {
    return targets.all_labels.to(device, torch::kInt64).index({indices.global_targets});
}

torch::Tensor concat_target_boxes(const PreparedTargets& targets, const MatcherLayerIndices& indices, const torch::Device& device) {
    return targets.all_boxes.to(device, torch::kFloat32).index({indices.global_targets});
}

bool has_target_masks(const PreparedTargets& targets) { return targets.packed_masks.has_value() && targets.packed_masks->bits.defined(); }

// The zero-query result of a sparse mask projection: no masks, but the spatial extent, dtype, and
// device the projection would have produced. Both projection entry points return this one shape.
torch::Tensor empty_sparse_pred_masks(const OutputLayer::SparsePredMasks& sparse) {
    return torch::empty({0, sparse.spatial_features.size(-2), sparse.spatial_features.size(-1)},
                        torch::TensorOptions().dtype(sparse.spatial_features.dtype()).device(sparse.spatial_features.device()));
}

torch::Tensor matched_sparse_pred_masks_for_batch(const OutputLayer::SparsePredMasks& sparse, int64_t batch_index,
                                                  const torch::Tensor& query_indices) {
    if (query_indices.dim() != 1) { throw std::runtime_error("native RF-DETR sparse mask projection expects 1D query indices"); }
    if (query_indices.numel() == 0) { return empty_sparse_pred_masks(sparse); }

    auto device_query_indices = query_indices.to(sparse.query_features.device(), torch::kInt64, false, false);
    const auto batch_spatial_features = sparse.spatial_features.index({batch_index});
    auto batch_query_features = sparse.query_features.index({batch_index, device_query_indices});
    if (batch_query_features.scalar_type() != batch_spatial_features.scalar_type()) {
        batch_query_features = batch_query_features.to(batch_spatial_features.scalar_type());
    }
    const auto bias = sparse.bias.to(batch_spatial_features.dtype());
    const auto flattened_spatial = batch_spatial_features.flatten(1).contiguous();
    return torch::matmul(batch_query_features, flattened_spatial)
               .view({batch_query_features.size(0), batch_spatial_features.size(-2), batch_spatial_features.size(-1)}) +
           bias;
}

torch::Tensor matched_pred_masks(const OutputLayer& layer, const MatcherLayerIndices& indices) {
    const auto& idx = indices.source;
    if (layer.pred_masks.has_value()) {
        const auto device = layer.pred_masks->device();
        return layer.pred_masks->index({
            idx.first.to(device, torch::kInt64, false, false),
            idx.second.to(device, torch::kInt64, false, false),
        });
    }
    if (!layer.sparse_pred_masks.has_value()) {
        throw std::runtime_error("native RF-DETR mask projection requires pred_masks in the model outputs");
    }

    const auto& sparse = *layer.sparse_pred_masks;
    if (idx.first.numel() == 0) { return empty_sparse_pred_masks(sparse); }

    const auto& batch_indices_cpu = indices.cpu_source.first;
    const auto* batch_indices = batch_indices_cpu.data_ptr<int64_t>();
    std::vector<torch::Tensor> selected_masks;
    selected_masks.reserve(static_cast<size_t>(sparse.spatial_features.size(0)));
    int64_t begin = 0;
    while (begin < batch_indices_cpu.numel()) {
        const int64_t batch_index = batch_indices[begin];
        int64_t end = begin + 1;
        while (end < batch_indices_cpu.numel() && batch_indices[end] == batch_index) {
            ++end;
        }
        selected_masks.push_back(matched_sparse_pred_masks_for_batch(sparse, batch_index, idx.second.narrow(0, begin, end - begin)));
        begin = end;
    }
    if (selected_masks.size() == 1) { return selected_masks.front(); }
    return torch::cat(selected_masks, 0);
}

void solve_linear_assignment(const torch::Tensor& cost_matrix_cpu, LsapScratch& scratch) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_lsap{"rfdetr.matcher.lsap"};
    const int64_t rows = cost_matrix_cpu.size(0);
    const int64_t cols = cost_matrix_cpu.size(1);
    if (rows == 0 || cols == 0) {
        scratch.row_indices.clear();
        scratch.col_indices.clear();
        return;
    }
    if (!cost_matrix_cpu.device().is_cpu()) { throw std::runtime_error("native RF-DETR matcher solver expects a CPU cost matrix"); }

    scratch.costs.resize(static_cast<std::size_t>(rows * cols));
    const auto matrix =
        torch::from_blob(scratch.costs.data(), {rows, cols}, torch::TensorOptions().dtype(torch::kDouble).device(torch::kCPU));
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_prepare_exact_cost{"rfdetr.matcher.prepare_exact_cost"};
        copy_sanitized_cost_to_double(cost_matrix_cpu, matrix);
    }

    const int64_t assignment_size = std::min(rows, cols);
    scratch.row_indices.resize(static_cast<size_t>(assignment_size));
    scratch.col_indices.resize(static_cast<size_t>(assignment_size));
    const auto status = mmltk::backend::models::rfdetr::solve_rectangular_linear_sum_assignment(
        rows, cols, matrix.data_ptr<double>(), false, scratch.row_indices.data(), scratch.col_indices.data(), scratch.solver);
    if (status == mmltk::backend::models::rfdetr::RectangularLsApStatus::kInvalid) {
        throw std::runtime_error("native RF-DETR matcher received invalid numeric entries in the cost matrix");
    }
    if (status == mmltk::backend::models::rfdetr::RectangularLsApStatus::kInfeasible) {
        throw std::runtime_error("native RF-DETR matcher received an infeasible cost matrix");
    }
}

MatchIndices empty_matcher_indices(const PreparedTargets& targets) {
    MatchIndices empty;
    empty.reserve(targets.targets.size());
    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(torch::kCPU);
    for (size_t index = 0; index < targets.targets.size(); ++index) {
        empty.emplace_back(torch::empty({0}, options), torch::empty({0}, options));
    }
    return empty;
}

struct MatcherMaskLogits {
    torch::Tensor point_coords;
    torch::Tensor pred_logits;
};

// Samples random point coordinates and the matching per-query mask logits for matcher mask costs, from either
// dense pred_masks or the sparse mask head. pred_logits is shaped {batch, queries, points}.
MatcherMaskLogits sample_matcher_mask_logits(const OutputLayer& layer, const DetectionConfig& config) {
    if (layer.pred_masks.has_value()) {
        const int64_t batch_size = layer.pred_logits.size(0);
        const int64_t query_count = layer.pred_logits.size(1);
        const auto out_masks = layer.pred_masks->flatten(0, 1);
        const int64_t num_points = out_masks.size(-2) * out_masks.size(-1) / config.mask_point_sample_ratio;
        auto point_coords = torch::rand({1, num_points, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(out_masks.device()));
        auto pred_masks_logits =
            matcher_point_sample(out_masks.unsqueeze(1), point_coords.expand({out_masks.size(0), num_points, 2}), torch::kBilinear)
                .squeeze(1)
                .view({batch_size, query_count, num_points});
        mmltk::common::logging::profile_set_value("rfdetr.matcher.mask_points", static_cast<size_t>(num_points));
        return {std::move(point_coords), std::move(pred_masks_logits)};
    }
    if (layer.sparse_pred_masks.has_value()) {
        const auto& sparse = *layer.sparse_pred_masks;
        const int64_t num_points = sparse.spatial_features.size(-2) * sparse.spatial_features.size(-1) / config.mask_point_sample_ratio;
        auto point_coords =
            torch::rand({1, num_points, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(sparse.spatial_features.device()));
        auto sampled_features = matcher_point_sample(
            sparse.spatial_features, point_coords.expand({sparse.spatial_features.size(0), num_points, 2}), torch::kBilinear);
        auto query_features = sparse.query_features;
        if (query_features.scalar_type() != sampled_features.scalar_type()) {
            query_features = query_features.to(sampled_features.scalar_type());
        }
        const auto bias = sparse.bias.to(sampled_features.dtype());
        auto pred_masks_logits = torch::bmm(query_features, sampled_features) + bias;
        mmltk::common::logging::profile_set_value("rfdetr.matcher.mask_points", static_cast<size_t>(num_points));
        return {std::move(point_coords), std::move(pred_masks_logits)};
    }
    throw std::runtime_error("native RF-DETR mask matcher requires pred_masks in the model outputs");
}

torch::Tensor build_dense_matcher_cost(const OutputLayer& layer, const PreparedTargets& targets, const DetectionConfig& config) {
    const auto bs = layer.pred_logits.size(0);
    const auto num_queries = layer.pred_logits.size(1);
    const auto device = layer.pred_logits.device();

    int64_t total_targets = 0;
    for (const auto count : targets.counts) {
        total_targets += count;
    }
    if (total_targets == 0) { return torch::empty({bs, num_queries, 0}, torch::TensorOptions().dtype(torch::kFloat32).device(device)); }

    const double alpha = 0.25;
    const double gamma = 2.0;
    const auto flat_pred_logits = layer.pred_logits.flatten(0, 1);
    const auto out_prob = flat_pred_logits.sigmoid();
    const auto out_bbox = layer.pred_boxes.flatten(0, 1);
    const auto tgt_ids = targets.all_labels.to(device, torch::kInt64, false, false);
    const auto tgt_bbox = targets.all_boxes.to(device, torch::kFloat32, false, false);

    torch::Tensor cost_giou;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_giou{"rfdetr.matcher.cost_giou"};
        cost_giou = -pairwise_generalized_box_iou(box_cxcywh_to_xyxy(out_bbox, BoxExtentPolicy::Preserve),
                                                  box_cxcywh_to_xyxy(tgt_bbox, BoxExtentPolicy::Preserve));
    }

    torch::Tensor cost_class;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_class{"rfdetr.matcher.cost_class"};
        const auto neg_cost_class = (1 - alpha) * torch::pow(out_prob, gamma) * (-F::logsigmoid(-flat_pred_logits));
        const auto pos_cost_class = alpha * torch::pow(1 - out_prob, gamma) * (-F::logsigmoid(flat_pred_logits));
        cost_class = pos_cost_class.index({Slice(), tgt_ids}) - neg_cost_class.index({Slice(), tgt_ids});
    }

    torch::Tensor cost_bbox;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_bbox{"rfdetr.matcher.cost_bbox"};
        cost_bbox = torch::cdist(out_bbox, tgt_bbox, 1.0);
    }

    auto cost = config.set_cost_bbox * cost_bbox + config.set_cost_class * cost_class + config.set_cost_giou * cost_giou;

    const bool masks_present = config.include_masks && has_target_masks(targets);
    if (masks_present) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_masks{"rfdetr.matcher.cost_masks"};
        const auto sampled = sample_matcher_mask_logits(layer, config);
        const auto& point_coords = sampled.point_coords;
        const auto pred_masks_logits = sampled.pred_logits.flatten(0, 1);

        const auto& all_masks = require_target_masks(targets, "native RF-DETR mask matcher");
        const auto mask_indices =
            torch::arange(all_masks.bits.size(0), torch::TensorOptions().dtype(torch::kInt64).device(all_masks.bits.device()));
        const auto tgt_masks_flat =
            sample_target_masks(all_masks, mask_indices, point_coords, "native RF-DETR mask matcher").to(pred_masks_logits.dtype());
        cost = cost + config.mask_ce_loss_coef * batch_sigmoid_ce_loss(pred_masks_logits, tgt_masks_flat, config.use_jit_traced_loss_ops) +
               config.mask_dice_loss_coef * batch_dice_loss(pred_masks_logits, tgt_masks_flat, config.use_jit_traced_loss_ops);
    }

    return cost.reshape({bs, num_queries, total_targets}).to(torch::kFloat32);
}

std::pair<torch::Tensor, torch::Tensor> target_metadata_on_device(const PreparedTargets& targets, const torch::Device& device) {
    if (targets.target_offsets.defined() && targets.target_counts.defined() && targets.target_offsets.device() == device &&
        targets.target_counts.device() == device) {
        return {targets.target_offsets, targets.target_counts};
    }
    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(device);
    return {torch::tensor(targets.offsets, options), torch::tensor(targets.counts, options)};
}

void build_cuda_matcher_cost_into(const OutputLayer& layer, const PreparedTargets& targets, const DetectionConfig& config,
                                  const torch::Tensor& target_indices, const torch::Tensor& compact_cost) {
    const auto device = layer.pred_logits.device();
    auto [target_offsets, target_counts] = target_metadata_on_device(targets, device);
    const auto pred_logits = layer.pred_logits.contiguous();
    const auto pred_boxes = layer.pred_boxes.contiguous();
    const auto target_labels = targets.all_labels.to(device, torch::kInt64, false, false).contiguous();
    const auto target_boxes = targets.all_boxes.to(device, torch::kFloat32, false, false).contiguous();
    pairwise_detection_cost_cuda_out(compact_cost, pred_logits, pred_boxes, target_labels, target_boxes, target_offsets.contiguous(),
                                     target_counts.contiguous(), config.set_cost_class, config.set_cost_bbox, config.set_cost_giou);

    if (!config.include_masks || !has_target_masks(targets)) { return; }

    mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_masks{"rfdetr.matcher.cost_masks"};
    const auto [point_coords, pred_masks_logits] = sample_matcher_mask_logits(layer, config);

    const auto& all_masks = require_target_masks(targets, "native RF-DETR mask matcher");
    const auto target_masks =
        sample_target_masks(all_masks, target_indices, point_coords, "native RF-DETR mask matcher").to(torch::kFloat32).contiguous();
    pairwise_mask_cost_cuda_add_(compact_cost, pred_masks_logits.contiguous(), target_masks, target_offsets.contiguous(),
                                 target_counts.contiguous(), config.mask_ce_loss_coef, config.mask_dice_loss_coef);
}

void solve_matcher_indices_for_batch_cpu(const torch::Tensor& batch_cost_cpu, int64_t group_detr, LsapScratch& scratch,
                                         MatchIndices::value_type& result) {
    const int64_t num_queries = batch_cost_cpu.size(0);
    const int64_t group_queries = num_queries / group_detr;
    const int64_t target_count = batch_cost_cpu.size(1);
    if (target_count == 0) { return; }
    const int64_t group_assignment_size = std::min(group_queries, target_count);
    scratch.ensure_assignment_capacity(group_assignment_size, group_assignment_size * group_detr);
    scratch.batch_rows.clear();
    scratch.batch_cols.clear();
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_solve_groups{"rfdetr.matcher.solve_groups"};
        for (int64_t group_index = 0; group_index < group_detr; ++group_index) {
            auto group_cost = batch_cost_cpu.narrow(0, group_index * group_queries, group_queries);
            solve_linear_assignment(group_cost, scratch);
            for (const int64_t row : scratch.row_indices) {
                scratch.batch_rows.push_back(row + group_index * group_queries);
            }
            scratch.batch_cols.insert(scratch.batch_cols.end(), scratch.col_indices.begin(), scratch.col_indices.end());
        }
    }
    std::ranges::copy(scratch.batch_rows, result.first.data_ptr<int64_t>());
    std::ranges::copy(scratch.batch_cols, result.second.data_ptr<int64_t>());
}

std::vector<MatchIndices> compute_matcher_indices_for_layers(const std::vector<const OutputLayer*>& layers, const PreparedTargets& targets,
                                                             const DetectionConfig& config, int64_t group_detr,
                                                             MatcherWorkspace& workspace) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_total{"rfdetr.matcher.total"};
    torch::NoGradGuard no_grad;
    if (layers.empty()) { return {}; }

    if (group_detr <= 0) throw std::invalid_argument("matcher group count must be positive");
    const auto bs = layers.front()->pred_logits.size(0);
    const auto device = layers.front()->pred_logits.device();
    if (bs != static_cast<int64_t>(targets.targets.size())) {
        throw std::runtime_error("target batch size does not match RF-DETR output batch size");
    }

    int64_t total_targets = 0;
    for (const auto count : targets.counts) {
        total_targets += count;
    }
    int64_t max_queries = 0;
    int64_t max_targets_per_image = 0;
    int64_t assignment_extent = 0;
    std::vector<int64_t> layer_query_counts;
    layer_query_counts.reserve(layers.size());
    for (const auto& layer : layers) {
        const int64_t layer_queries = layer->pred_logits.size(1);
        if (layer->pred_logits.size(0) != bs) {
            throw std::runtime_error("native RF-DETR batched matcher requires identical layer batch sizes");
        }
        if (layer->pred_logits.device() != device) {
            throw std::runtime_error("native RF-DETR batched matcher requires all layers on the same device");
        }
        if (layer_queries % group_detr != 0) {
            throw std::runtime_error("num_queries must be divisible by group_detr for every matcher layer");
        }
        const int64_t group_queries = layer_queries / group_detr;
        for (const int64_t target_count : targets.counts) {
            // Each group solves a rectangular matrix; only its smaller side
            // has written assignments. Multiplication is bounded by layer_queries.
            const int64_t active_extent = std::min(group_queries, target_count) * group_detr;
            if (active_extent > std::numeric_limits<int64_t>::max() - assignment_extent)
                throw std::overflow_error("matcher assignment extent overflows");
            assignment_extent += active_extent;
        }
        layer_query_counts.push_back(layer_queries);
        max_queries = std::max(max_queries, layer_queries);
    }
    for (const auto count : targets.counts) {
        max_targets_per_image = std::max(max_targets_per_image, count);
    }
    mmltk::common::logging::profile_add_value("rfdetr.matcher.batch_size", bs);
    mmltk::common::logging::profile_add_value("rfdetr.matcher.num_queries", max_queries);
    mmltk::common::logging::profile_add_value("rfdetr.matcher.total_targets", total_targets);
    mmltk::common::logging::profile_set_value("rfdetr.matcher.layer_count", static_cast<size_t>(layers.size()));
    if (total_targets == 0) {
        std::vector<MatchIndices> empty(layers.size(), empty_matcher_indices(targets));
        return empty;
    }

    torch::Tensor compact_cost_cpu;
    std::vector<torch::Tensor> dense_cpu_costs;
    if (device.is_cuda()) {
        auto& scratch = workspace;
        scratch.prepare_cost({static_cast<int64_t>(layers.size()), bs, max_queries, max_targets_per_image}, device);
        const auto target_indices = targets.target_indices.defined() && targets.target_indices.device() == device
                                        ? targets.target_indices
                                        : torch::arange(total_targets, torch::TensorOptions().dtype(torch::kInt64).device(device));
        for (size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
            build_cuda_matcher_cost_into(*layers[layer_index], targets, config, target_indices,
                                         scratch.device_layer(static_cast<int64_t>(layer_index)));
        }
        {
            mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_cost_to_cpu{"rfdetr.matcher.cost_to_cpu"};
            compact_cost_cpu = scratch.read_cost();
        }
    } else {
        dense_cpu_costs.reserve(layers.size());
        for (const auto* layer : layers) {
            dense_cpu_costs.push_back(build_dense_matcher_cost(*layer, targets, config).to(torch::kCPU));
        }
    }

    const auto cpu_indices = workspace.cpu_indices(assignment_extent);
    std::vector<MatchIndices> all_indices(layers.size());
    int64_t offset = 0;
    for (size_t layer_index = 0; layer_index < all_indices.size(); ++layer_index) {
        auto& layer_indices = all_indices[layer_index];
        const auto group_queries = layer_query_counts[layer_index] / group_detr;
        layer_indices.reserve(targets.targets.size());
        for (const auto count : targets.counts) {
            const auto size = std::min(group_queries, count) * group_detr;
            layer_indices.emplace_back(cpu_indices.select(0, 0).narrow(0, offset, size), cpu_indices.select(0, 1).narrow(0, offset, size));
            offset += size;
        }
    }
    const int64_t task_count = static_cast<int64_t>(layers.size()) * bs;
    auto* const runtime = active_runtime_context();
    auto solve_tasks = [&](int64_t begin, int64_t end) {
        auto& scratch = runtime ? runtime->solver_workspace() : workspace.solver();
        for (int64_t task = begin; task < end; ++task) {
            const size_t layer_index = static_cast<size_t>(task / bs);
            const size_t batch_index = static_cast<size_t>(task % bs);
            const int64_t target_count = targets.counts[batch_index];
            if (target_count == 0) { continue; }

            torch::Tensor batch_cost_cpu;
            if (device.is_cuda()) {
                batch_cost_cpu = compact_cost_cpu.select(0, static_cast<int64_t>(layer_index))
                                     .select(0, static_cast<int64_t>(batch_index))
                                     .narrow(0, 0, layer_query_counts[layer_index])
                                     .narrow(1, 0, target_count);
            } else {
                batch_cost_cpu = dense_cpu_costs[layer_index]
                                     .select(0, static_cast<int64_t>(batch_index))
                                     .narrow(0, 0, layer_query_counts[layer_index])
                                     .narrow(1, targets.offsets[batch_index], target_count);
            }
            solve_matcher_indices_for_batch_cpu(batch_cost_cpu, group_detr, scratch, all_indices[layer_index][batch_index]);
        }
    };
    auto* const worker_pool = runtime ? &runtime->cpu_pool() : nullptr;
    if (worker_pool != nullptr) {
        worker_pool->parallel_for<int64_t>(0, task_count, static_cast<int>(worker_pool->size()), solve_tasks);
    } else {
        solve_tasks(0, task_count);
    }
    return all_indices;
}

MatchIndices compute_matcher_indices(const OutputLayer& layer, const PreparedTargets& targets, const DetectionConfig& config,
                                     int64_t group_detr) {
    std::vector<const OutputLayer*> layers = {&layer};
    MatcherAccess access(layer.pred_logits.device());
    return compute_matcher_indices_for_layers(layers, targets, config, group_detr, access.get()).front();
}

torch::Tensor matched_pos_ious(const OutputLayer& layer, const PreparedTargets& targets, const MatcherLayerIndices& indices,
                               const std::pair<torch::Tensor, torch::Tensor>& idx, const torch::Device& device) {
    const auto src_boxes = layer.pred_boxes.index({idx.first, idx.second});
    const auto target_boxes = concat_target_boxes(targets, indices, device);
    return aligned_box_iou(box_cxcywh_to_xyxy(src_boxes.detach(), BoxExtentPolicy::Preserve),
                           box_cxcywh_to_xyxy(target_boxes, BoxExtentPolicy::Preserve))
        .detach();
}

torch::Tensor iou_weighted_class_targets(const torch::Tensor& src_logits, const torch::Tensor& pos_ious,
                                         const std::pair<torch::Tensor, torch::Tensor>& idx, const torch::Tensor& target_classes_o,
                                         int64_t num_classes) {
    auto cls_targets = torch::zeros({src_logits.size(0), src_logits.size(1), num_classes}, src_logits.options());
    cls_targets.index_put_({idx.first, idx.second, target_classes_o}, pos_ious.to(cls_targets.dtype()));
    return cls_targets;
}

TensorMap loss_labels(const OutputLayer& layer, const PreparedTargets& targets, const MatcherLayerIndices& indices,
                      const DetectionConfig& config, double num_boxes, bool log) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_loss_labels{"rfdetr.criterion.loss_labels"};
    TensorMap losses;
    const auto src_logits = layer.pred_logits;
    const auto device = src_logits.device();
    const auto idx = indices.source;
    const auto target_classes_o = concat_target_labels(targets, indices, device);

    torch::Tensor loss_ce;
    if (config.ia_bce_loss) {
        const double alpha = config.focal_alpha;
        const double gamma = 2.0;
        const auto pos_ious = matched_pos_ious(layer, targets, indices, idx, device);
        const auto prob = src_logits.sigmoid();
        auto pos_weights = torch::zeros_like(src_logits);
        auto neg_weights = torch::pow(prob, gamma);
        auto t = torch::pow(prob.index({idx.first, idx.second, target_classes_o}), alpha) * torch::pow(pos_ious, 1 - alpha);
        t = torch::clamp(t, 0.01).detach();
        const auto t_cast = t.to(pos_weights.dtype());
        pos_weights.index_put_({idx.first, idx.second, target_classes_o}, t_cast);
        const auto neg_update = (torch::ones_like(t_cast) - t_cast).to(neg_weights.dtype());
        neg_weights.index_put_({idx.first, idx.second, target_classes_o}, neg_update);
        loss_ce = ia_bce_loss(src_logits, pos_weights, neg_weights, num_boxes, config.use_jit_traced_loss_ops);
    } else if (config.use_position_supervised_loss) {
        const auto pos_ious = matched_pos_ious(layer, targets, indices, idx, device);
        auto cls_targets = iou_weighted_class_targets(src_logits, pos_ious, idx, target_classes_o, config.num_classes);
        cls_targets = cls_targets / (cls_targets.view({cls_targets.size(0), -1, 1}).amax(1, true) + 1e-8f);
        loss_ce = position_supervised_loss(src_logits, cls_targets, num_boxes, config.focal_alpha, 2.0, config.use_jit_traced_loss_ops) *
                  src_logits.size(1);
    } else if (config.use_varifocal_loss) {
        const auto pos_ious = matched_pos_ious(layer, targets, indices, idx, device);
        const auto cls_targets = iou_weighted_class_targets(src_logits, pos_ious, idx, target_classes_o, config.num_classes);
        loss_ce = sigmoid_varifocal_loss(src_logits, cls_targets, num_boxes, config.focal_alpha, 2.0, config.use_jit_traced_loss_ops) *
                  src_logits.size(1);
    } else {
        auto target_classes = torch::full({src_logits.size(0), src_logits.size(1)}, config.num_classes,
                                          torch::TensorOptions().dtype(torch::kInt64).device(device));
        target_classes.index_put_({idx.first, idx.second}, target_classes_o);
        auto target_classes_onehot = torch::zeros({src_logits.size(0), src_logits.size(1), src_logits.size(2) + 1}, src_logits.options());
        target_classes_onehot.scatter_(2, target_classes.unsqueeze(-1),
                                       torch::ones(target_classes.unsqueeze(-1).sizes(), target_classes_onehot.options()));
        target_classes_onehot = target_classes_onehot.index({Slice(), Slice(), Slice(None, -1)});
        loss_ce =
            sigmoid_focal_loss(src_logits, target_classes_onehot, num_boxes, config.focal_alpha, 2.0, config.use_jit_traced_loss_ops) *
            src_logits.size(1);
    }

    losses["loss_ce"] = loss_ce;
    if (log) {
        const auto matched_logits = src_logits.index({idx.first, idx.second});
        losses["class_error"] = 100.0 - accuracy_top1(matched_logits, target_classes_o);
    }
    return losses;
}

TensorMap loss_cardinality(const OutputLayer& layer, const PreparedTargets& targets) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_loss_cardinality{"rfdetr.criterion.loss_cardinality"};
    TensorMap losses;
    const auto pred_logits = layer.pred_logits;
    const auto target_lengths = targets.target_counts.defined()
                                    ? targets.target_counts.to(pred_logits.device())
                                    : make_cpu_int64_tensor(targets.counts).to(pred_logits.device(), torch::kInt64, false, false);
    losses["cardinality_error"] = cardinality_error(pred_logits, target_lengths);
    return losses;
}

TensorMap loss_boxes(const OutputLayer& layer, const PreparedTargets& targets, const MatcherLayerIndices& indices, double num_boxes) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_loss_boxes{"rfdetr.criterion.loss_boxes"};
    TensorMap losses;
    const auto device = layer.pred_boxes.device();
    const auto idx = indices.source;
    const auto src_boxes = layer.pred_boxes.index({idx.first, idx.second});
    const auto target_boxes = concat_target_boxes(targets, indices, device);
    const auto loss_bbox = F::l1_loss(src_boxes, target_boxes, torch::nn::functional::L1LossFuncOptions().reduction(torch::kNone));
    losses["loss_bbox"] = loss_bbox.sum() / num_boxes;
    const auto loss_giou = 1 - aligned_generalized_box_iou(box_cxcywh_to_xyxy(src_boxes, BoxExtentPolicy::Preserve),
                                                           box_cxcywh_to_xyxy(target_boxes, BoxExtentPolicy::Preserve));
    losses["loss_giou"] = loss_giou.sum() / num_boxes;
    return losses;
}

TensorMap loss_masks(const OutputLayer& layer, const PreparedTargets& targets, const MatcherLayerIndices& indices,
                     const DetectionConfig& config, double num_boxes) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_loss_masks{"rfdetr.criterion.loss_masks"};
    TensorMap losses;
    const auto idx = indices.source;
    if (idx.first.numel() == 0) {
        const auto zero = layer.pred_logits.sum() * 0.0f;
        losses["loss_mask_ce"] = zero;
        losses["loss_mask_dice"] = zero;
        return losses;
    }

    auto src_masks = matched_pred_masks(layer, indices);
    if (src_masks.numel() == 0) {
        losses["loss_mask_ce"] = src_masks.sum();
        losses["loss_mask_dice"] = src_masks.sum();
        return losses;
    }

    src_masks = src_masks.unsqueeze(1);

    const int64_t num_points =
        std::max<int64_t>(src_masks.size(-2), src_masks.size(-2) * src_masks.size(-1) / config.mask_point_sample_ratio);
    mmltk::common::logging::profile_add_value("rfdetr.criterion.matched_pairs", idx.first.size(0));
    mmltk::common::logging::profile_set_value("rfdetr.criterion.mask_points", static_cast<size_t>(num_points));

    torch::Tensor point_coords;
    {
        torch::NoGradGuard no_grad;
        point_coords = get_uncertain_point_coords_with_randomness(src_masks, num_points, 3, 0.75);
    }
    const auto point_logits = point_sample(src_masks, point_coords, torch::kBilinear).squeeze(1);
    torch::Tensor point_labels;
    {
        torch::NoGradGuard no_grad;
        const auto& all_masks = require_target_masks(targets, "native RF-DETR mask loss");
        const auto global_indices = indices.global_targets;
        point_labels = sample_target_masks(all_masks, global_indices, point_coords, "native RF-DETR mask loss").to(point_logits.dtype());
    }

    losses["loss_mask_ce"] = sigmoid_ce_loss(point_logits, point_labels, num_boxes, config.use_jit_traced_loss_ops);
    losses["loss_mask_dice"] = dice_loss(point_logits, point_labels, num_boxes, config.use_jit_traced_loss_ops);
    return losses;
}

void update_losses(TensorMap& destination, TensorMap source, const std::string& suffix = std::string()) {
    for (auto& item : source) {
        destination[item.first + suffix] = std::move(item.second);
    }
}

}  // namespace

torch::Tensor cardinality_error(const torch::Tensor& logits, const torch::Tensor& target_counts) {
    torch::NoGradGuard no_grad;
    const auto confident_queries = std::get<0>(logits.sigmoid().max(-1)).gt(0.5).sum(1);
    return F::l1_loss(confident_queries.to(torch::kFloat32), target_counts.to(torch::kFloat32));
}

std::vector<std::pair<torch::Tensor, torch::Tensor>> matcher_indices(const ModelOutputs& outputs, const PreparedTargets& targets,
                                                                     const DetectionConfig& config, bool training_mode) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_matcher_public{"rfdetr.matcher.public"};
    return compute_matcher_indices(outputs.main, targets, config, training_mode ? config.group_detr : 1);
}

TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config,
                              bool training_mode, bool distributed_enabled, const AllReduceTensorFn& distributed_all_reduce) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_total{"rfdetr.criterion.total"};
    const int64_t group_detr = training_mode ? config.group_detr : 1;
    int64_t num_boxes_int = 0;
    for (const auto count : targets.counts) {
        num_boxes_int += count;
    }
    if (!config.sum_group_losses) { num_boxes_int *= group_detr; }

    mmltk::common::logging::profile_add_value("rfdetr.criterion.num_boxes", num_boxes_int);
    double num_boxes_value =
        std::max(static_cast<double>(num_boxes_int) / static_cast<double>(std::max<int64_t>(1, config.world_size)), 1.0);
    if (distributed_enabled) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_distributed_num_boxes{"rfdetr.criterion.distributed_num_boxes"};
        auto num_boxes = torch::empty({1}, torch::TensorOptions().dtype(torch::kFloat32).device(outputs.main.pred_logits.device()));
        num_boxes.fill_(static_cast<float>(num_boxes_int));
        if (distributed_all_reduce) { distributed_all_reduce(num_boxes); }
        num_boxes_value = torch::clamp_min(num_boxes / std::max<int64_t>(1, config.world_size), 1.0).item<double>();
    }

    return detection_loss_dict(outputs, targets, config, training_mode, num_boxes_value);
}

TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config,
                              bool training_mode, double num_boxes_value) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_total_resolved_num_boxes{"rfdetr.criterion.total_resolved_num_boxes"};
    const int64_t group_detr = training_mode ? config.group_detr : 1;
    std::vector<const OutputLayer*> matcher_layers;
    matcher_layers.reserve(1 + outputs.aux_outputs.size() + (outputs.enc_outputs.has_value() ? 1 : 0));
    matcher_layers.push_back(&outputs.main);
    for (const auto& aux_output : outputs.aux_outputs) {
        matcher_layers.push_back(&aux_output);
    }
    if (outputs.enc_outputs.has_value()) { matcher_layers.push_back(&*outputs.enc_outputs); }
    MatcherAccess access(outputs.main.pred_logits.device());
    const auto cpu_indices = compute_matcher_indices_for_layers(matcher_layers, targets, config, group_detr, access.get());
    auto matcher_indices_all = access.get().pack(cpu_indices, targets.offsets, outputs.main.pred_logits.device());
    size_t matcher_layer_index = 0;
    const auto& indices = matcher_indices_all[matcher_layer_index++];

    TensorMap losses;
    update_losses(losses, loss_labels(outputs.main, targets, indices, config, num_boxes_value, true));
    update_losses(losses, loss_cardinality(outputs.main, targets));
    update_losses(losses, loss_boxes(outputs.main, targets, indices, num_boxes_value));
    if (config.include_masks) { update_losses(losses, loss_masks(outputs.main, targets, indices, config, num_boxes_value)); }

    if (!outputs.aux_outputs.empty()) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_aux{"rfdetr.criterion.aux"};
        for (size_t aux_index = 0; aux_index < outputs.aux_outputs.size(); ++aux_index) {
            const auto& aux_indices = matcher_indices_all[matcher_layer_index++];
            update_losses(losses, loss_labels(outputs.aux_outputs[aux_index], targets, aux_indices, config, num_boxes_value, false),
                          "_" + std::to_string(aux_index));
            update_losses(losses, loss_cardinality(outputs.aux_outputs[aux_index], targets), "_" + std::to_string(aux_index));
            update_losses(losses, loss_boxes(outputs.aux_outputs[aux_index], targets, aux_indices, num_boxes_value),
                          "_" + std::to_string(aux_index));
            if (config.include_masks) {
                update_losses(losses, loss_masks(outputs.aux_outputs[aux_index], targets, aux_indices, config, num_boxes_value),
                              "_" + std::to_string(aux_index));
            }
        }
    }

    if (outputs.enc_outputs.has_value()) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_encoder{"rfdetr.criterion.encoder"};
        const auto& enc_indices = matcher_indices_all[matcher_layer_index++];
        update_losses(losses, loss_labels(*outputs.enc_outputs, targets, enc_indices, config, num_boxes_value, false), "_enc");
        update_losses(losses, loss_cardinality(*outputs.enc_outputs, targets), "_enc");
        update_losses(losses, loss_boxes(*outputs.enc_outputs, targets, enc_indices, num_boxes_value), "_enc");
        if (config.include_masks) {
            update_losses(losses, loss_masks(*outputs.enc_outputs, targets, enc_indices, config, num_boxes_value), "_enc");
        }
    }

    return losses;
}

torch::Tensor weighted_detection_loss(const TensorMap& loss_dict, const DetectionConfig& config, const torch::Device& device, torch::Tensor* auxiliary_weighted) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_criterion_weighted_total{"rfdetr.criterion.weighted_total"};
    auto total = torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
    bool has_loss = false;
    for (const auto& item : config.weight_dict) {
        auto found = loss_dict.find(item.first);
        if (found == loss_dict.end()) { continue; }
        auto weighted = found->second * item.second;
        if (auxiliary_weighted && (item.first.ends_with("_enc") ||
            (!item.first.empty() && std::isdigit(static_cast<unsigned char>(item.first.back()))))) {
            *auxiliary_weighted = auxiliary_weighted->defined() ? *auxiliary_weighted + weighted : weighted;
        }
        if (!has_loss) {
            total = weighted;
            has_loss = true;
        } else {
            total = total + weighted;
        }
    }
    if (!has_loss) { throw std::runtime_error("native RF-DETR criterion returned no weighted losses"); }
    return total;
}

}  // namespace mmltk::backend::models::rfdetr
