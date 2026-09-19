#include <ATen/cuda/CUDAContext.h>
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/models/rfdetr/core/detail/detection_geometry.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include <ATen/ops/argsort.h>
#include <ATen/ops/gather.h>
#include <ATen/ops/index_select.h>
#include <ATen/ops/gt.h>
#include <ATen/ops/upsample_bilinear2d.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <c10/util/ArrayRef.h>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace {
struct PostprocessCore {
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    torch::Tensor query_indices;
    torch::Tensor counts;
};
struct FixedBoxScaleCacheEntry {
    torch::Device device = torch::kCPU;
    cudaStream_t stream = nullptr;
    int64_t height = 0;
    int64_t width = 0;
    torch::Tensor scale;
};
torch::Tensor fixed_box_scale(const torch::Tensor& boxes, const int64_t height, const int64_t width) {
    cudaStream_t stream = nullptr;
    if (boxes.is_cuda()) { stream = at::cuda::getCurrentCUDAStream(boxes.get_device()).stream(); }
    thread_local FixedBoxScaleCacheEntry cache;
    if (cache.scale.defined() && cache.device == boxes.device() && cache.stream == stream && cache.height == height && cache.width == width) {
        return cache.scale;
    }
    FixedBoxScaleCacheEntry candidate{
        boxes.device(), stream, height, width, torch::tensor({width, height, width, height}, boxes.options().dtype(torch::kFloat32)).view({1, 1, 4}),
    };
    // Never rewrite storage still read by queued work. Torch retires the old
    // allocation on its own stream; construction failure leaves the cache intact.
    cache = std::move(candidate);
    return cache.scale;
}
PostprocessCore postprocess_core(const OutputTensors& outputs, int64_t target_height, int64_t target_width,
                                 int64_t num_select, ClassPostprocessLane* classes) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_total{"rfdetr.native.postprocess.total"};
    const auto out_logits = (classes ? classes->ValidateLogits(outputs.pred_logits) : outputs.pred_logits).to(torch::kFloat32);
    const auto out_bbox = outputs.pred_boxes.to(torch::kFloat32);
    if (out_logits.dim() != 3 || out_bbox.dim() != 3 || out_bbox.size(2) != 4 ||
        out_bbox.size(0) != out_logits.size(0) || out_bbox.size(1) != out_logits.size(1) || num_select < 0)
        throw std::invalid_argument("invalid RF-DETR postprocessing shapes or selection limit");
    if (out_logits.numel() != 0) static_cast<void>(checked_prediction_extent(out_logits.numel(), sizeof(std::int64_t), kMaximumPredictionTensorBytes));
    PostprocessCore core;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_topk{"rfdetr.native.postprocess.topk"};
        const auto flat_logits = out_logits.flatten(1);
        const int64_t k = classes && classes->eligible_count() == 0 ? 0 : std::min<int64_t>(num_select, flat_logits.size(1));
        if (k == 0) {
            core.scores = torch::empty({out_logits.size(0), 0}, out_logits.options());
            core.query_indices = torch::empty({out_logits.size(0), 0}, out_logits.options().dtype(torch::kInt64));
            core.labels = core.query_indices;
        } else {
            const auto flat_prob = flat_logits.sigmoid();
            // Stable physical flattened order is the tie breaker, before slot filtering.
            const auto sorted_indices = at::argsort(flat_prob, true, 1, true);
            const auto topk_indexes = sorted_indices.narrow(1, 0, k);
            core.scores = flat_prob.gather(1, topk_indexes);
            core.query_indices = torch::floor_divide(topk_indexes, out_logits.size(2));
            core.labels = topk_indexes.remainder(out_logits.size(2));
            if (classes) core.labels = classes->References(core.labels);
            if (!classes || classes->eligible_count() == static_cast<std::size_t>(out_logits.size(2))) {
                core.counts = torch::full({out_logits.size(0)}, k, out_logits.options().dtype(torch::kInt64));
            } else {
                const auto valid = core.labels.ge(0);
                const auto prefix = valid.to(torch::kInt64).cumsum(1);
                core.counts = prefix.select(1, k - 1).clone();
                // Linear stable partition without a dynamic shape or a host wait.
                const auto ordinal = torch::arange(k, core.query_indices.options()).unsqueeze(0).expand_as(core.query_indices);
                const auto destinations = torch::where(valid, prefix - 1, core.counts.unsqueeze(1) + ordinal - prefix);
                const auto order = torch::empty_like(core.query_indices).scatter_(1, destinations, ordinal);
                core.scores = core.scores.gather(1, order);
                core.labels = core.labels.gather(1, order);
                core.query_indices = core.query_indices.gather(1, order);
            }
        }
    }
    if (!core.counts.defined()) core.counts = torch::zeros({out_logits.size(0)}, out_logits.options().dtype(torch::kInt64));
    core.boxes = box_cxcywh_to_xyxy(out_bbox, BoxExtentPolicy::Preserve);
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_gather_boxes{"rfdetr.native.postprocess.gather_boxes"};
        core.boxes = core.boxes.gather(1, core.query_indices.unsqueeze(-1).expand({core.query_indices.size(0), core.query_indices.size(1), 4}));
    }
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_scale_boxes{"rfdetr.native.postprocess.scale_boxes"};
        core.boxes = core.boxes.clamp(0, 1) * fixed_box_scale(core.boxes, target_height, target_width);
    }
    return core;
}
}  // namespace
void clip_prediction_boxes_(const torch::Tensor& boxes, double left, double top, double right, double bottom) {
    if (left > right || top > bottom) throw std::invalid_argument("invalid prediction clipping bounds");
    boxes.select(-1, 0).clamp_(left, right);
    boxes.select(-1, 2).clamp_(left, right);
    boxes.select(-1, 1).clamp_(top, bottom);
    boxes.select(-1, 3).clamp_(top, bottom);
}
void ClassPostprocessLane::Prepare(const torch::Device& device) {
    const auto stream = device.is_cuda() ? at::cuda::getCurrentCUDAStream(device.index()).stream() : nullptr;
    if (references_.defined() && references_.device() == device && prepared_stream_ == stream) return;
    const auto references = layout_->physical_references();
    references_ = torch::tensor(at::ArrayRef<std::int64_t>(references.data(), references.size()),
                                torch::TensorOptions().dtype(torch::kInt64).device(device));
    prepared_stream_ = stream;
}
torch::Tensor ClassPostprocessLane::ValidateLogits(const torch::Tensor& logits) const {
    if (logits.dim() != 3 || static_cast<std::size_t>(logits.size(2)) != layout_->output_width())
        throw std::invalid_argument("logit width disagrees with admitted class layout");
    return logits;
}
torch::Tensor ClassPostprocessLane::References(const torch::Tensor& indices) const {
    const auto stream = indices.is_cuda() ? at::cuda::getCurrentCUDAStream(indices.get_device()).stream() : nullptr;
    if (!references_.defined() || indices.device() != references_.device() || prepared_stream_ != stream)
        throw std::invalid_argument("class references used outside the prepared lane");
    return references_.index_select(0, indices.flatten()).view(indices.sizes());
}
PostprocessedSelection select_output_batch_fixed_size(const OutputTensors& outputs, int64_t height, int64_t width, int64_t count, bool require_masks,
                                                      ClassPostprocessLane* classes) {
    if (require_masks && !outputs.pred_masks) throw std::runtime_error("RF-DETR requested masks are absent");
    auto core = postprocess_core(outputs, height, width, count, classes);
    if (outputs.pred_masks &&
        (outputs.pred_masks->dim() != 4 || outputs.pred_masks->size(0) != outputs.pred_logits.size(0) ||
         outputs.pred_masks->size(1) != outputs.pred_logits.size(1) || outputs.pred_masks->size(2) <= 0 || outputs.pred_masks->size(3) <= 0 ||
         !outputs.pred_masks->is_floating_point() || outputs.pred_masks->device() != outputs.pred_logits.device()))
        throw std::invalid_argument("RF-DETR mask logits are incompatible with selected queries");
    if (outputs.pred_masks)
        static_cast<void>(checked_prediction_extent(outputs.pred_masks->numel(), outputs.pred_masks->element_size(), kMaximumPredictionTensorBytes));
    return {std::move(core.scores), std::move(core.labels), std::move(core.boxes), std::move(core.query_indices), outputs.pred_masks, std::move(core.counts)};
}
SelectedMaskCapacity SelectedMaskWorkspace::RetainedCapacity(std::size_t host_bytes) const {
    const auto capacity = [](const auto& tensor) -> std::size_t { return tensor.defined() ? tensor.storage().nbytes() : 0U; };
    return {{capacity(gathered_), capacity(expanded_), capacity(masks_), host_bytes}};
}
void SelectedMaskWorkspace::ResetSettled() {
    gathered_ = torch::Tensor{};
    expanded_ = torch::Tensor{};
    masks_ = torch::Tensor{};
}
torch::Tensor SelectedMaskWorkspace::Materialize(const torch::Tensor& logits, const torch::Tensor& query_indices, int64_t height, int64_t width) {
    const auto batch = logits.size(0);
    const auto count = query_indices.size(1);
    if (count == 0) return torch::empty({batch, 0, height, width}, logits.options().dtype(torch::kBool));
    auto gather = query_indices.unsqueeze(-1).unsqueeze(-1).expand({batch, count, logits.size(2), logits.size(3)});
    const auto selected = checked_prediction_extent(static_cast<std::size_t>(batch), static_cast<std::size_t>(count), kMaximumPredictionCandidates);
    static_cast<void>(SelectedMaskCapacity::Resolve(selected, logits.size(2), logits.size(3), height, width, logits.element_size()));
    if (!gathered_.defined()) gathered_ = torch::empty({0}, logits.options());
    if (!expanded_.defined()) expanded_ = torch::empty({0}, logits.options());
    if (!masks_.defined()) masks_ = torch::empty({0}, logits.options().dtype(torch::kBool));
    if (gathered_.scalar_type() != logits.scalar_type() || gathered_.device() != logits.device()) {
        gathered_ = torch::empty({0}, logits.options());
        expanded_ = torch::empty({0}, logits.options());
        masks_ = torch::empty({0}, logits.options().dtype(torch::kBool));
    }
    gathered_.resize_(gather.sizes());
    expanded_.resize_({static_cast<std::int64_t>(selected), 1, height, width});
    masks_.resize_(expanded_.sizes());
    at::gather_out(gathered_, logits, 1, gather);
    at::upsample_bilinear2d_out(expanded_, gathered_.flatten(0, 1).unsqueeze(1), {height, width}, false);
    at::gt_out(masks_, expanded_, 0.0);
    return masks_.view({batch, count, height, width});
}
torch::Tensor materialize_selected_masks(const torch::Tensor& logits, const torch::Tensor& query_indices, int64_t height, int64_t width) {
    SelectedMaskWorkspace workspace;
    return workspace.Materialize(logits, query_indices, height, width);
}
PostprocessedBatch postprocess_output_batch_fixed_size(const OutputTensors& outputs, int64_t target_height, int64_t target_width, int64_t num_select,
                                                       ClassPostprocessLane* classes) {
    auto selected = select_output_batch_fixed_size(outputs, target_height, target_width, num_select, false, classes);
    PostprocessedBatch result{selected.scores, selected.labels, selected.boxes, std::nullopt, selected.counts};
    if (selected.mask_logits) result.masks = materialize_selected_masks(*selected.mask_logits, selected.query_indices, target_height, target_width);
    return result;
}
}  // namespace mmltk::backend::models::rfdetr
