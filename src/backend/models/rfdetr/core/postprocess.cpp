#include <ATen/cuda/CUDAContext.h>
#include <torch/nn/functional/vision.h>
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/models/rfdetr/core/detail/detection_geometry.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include <ATen/ops/gather.h>
#include <ATen/ops/index_select.h>
#include <ATen/ops/gt.h>
#include <ATen/ops/upsample_bilinear2d.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace {
struct PostprocessCore {
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    torch::Tensor query_indices;
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
PostprocessCore postprocess_core(const OutputTensors& outputs, const torch::Tensor* target_sizes, std::optional<std::pair<int64_t, int64_t>> fixed_size,
                                 int64_t num_select, ClassPostprocessLane* classes) {
    mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_total{"rfdetr.native.postprocess.total"};
    const auto out_logits = (classes ? classes->Gather(outputs.pred_logits) : outputs.pred_logits).to(torch::kFloat32);
    const auto out_bbox = outputs.pred_boxes.to(torch::kFloat32);
    if (target_sizes != nullptr && (out_logits.size(0) != target_sizes->size(0) || target_sizes->size(1) != 2)) {
        throw std::runtime_error("target_sizes must be [batch,2] and aligned with RF-DETR outputs");
    }
    if (!fixed_size.has_value() && target_sizes == nullptr) { throw std::runtime_error("postprocess_outputs requires target_sizes or a fixed target size"); }
    PostprocessCore core;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_topk{"rfdetr.native.postprocess.topk"};
        const auto prob = out_logits.sigmoid();
        const auto flat_prob = prob.flatten(1);
        const int64_t k = std::min<int64_t>(num_select, flat_prob.size(1));
        if (out_logits.size(2) == 0) {
            core.scores = flat_prob.narrow(1, 0, 0);
            core.query_indices = torch::empty({out_logits.size(0), 0}, out_logits.options().dtype(torch::kInt64));
            core.labels = core.query_indices;
        } else {
            const auto topk = flat_prob.topk(k, 1);
            core.scores = std::get<0>(topk);
            const auto topk_indexes = std::get<1>(topk).to(torch::kInt64);
            core.query_indices = torch::floor_divide(topk_indexes, out_logits.size(2));
            core.labels = topk_indexes.remainder(out_logits.size(2));
            if (classes) core.labels = classes->References(core.labels);
        }
    }
    core.boxes = box_cxcywh_to_xyxy(out_bbox, BoxExtentPolicy::ClampNonnegative);
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_gather_boxes{"rfdetr.native.postprocess.gather_boxes"};
        core.boxes = core.boxes.gather(1, core.query_indices.unsqueeze(-1).expand({core.query_indices.size(0), core.query_indices.size(1), 4}));
    }
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_scale_boxes{"rfdetr.native.postprocess.scale_boxes"};
        if (fixed_size.has_value()) {
            const auto [target_height, target_width] = *fixed_size;
            core.boxes = core.boxes * fixed_box_scale(core.boxes, target_height, target_width);
        } else {
            const auto img_h = target_sizes->select(1, 0);
            const auto img_w = target_sizes->select(1, 1);
            const auto scale = torch::stack({img_w, img_h, img_w, img_h}, 1);
            core.boxes = core.boxes * scale.unsqueeze(1);
        }
    }
    return core;
}
}  // namespace
void ClassPostprocessLane::Prepare(const torch::Device& device) {
    prefix_identity_ = layout_->prefix_identity();
    if (prefix_identity_ || eligible_count() == 0) return;
    const auto stream = device.is_cuda() ? at::cuda::getCurrentCUDAStream(device.index()).stream() : nullptr;
    if (slots_.defined() && slots_.device() == device && prepared_stream_ == stream) return;
    const auto options = torch::TensorOptions().dtype(torch::kInt64).device(device);
    auto slots = torch::tensor(std::vector<std::int64_t>(layout_->eligible_slots().begin(), layout_->eligible_slots().end()), options);
    auto references = torch::tensor(std::vector<std::int64_t>(layout_->class_references().begin(), layout_->class_references().end()), options);
    auto gather = gather_.defined() ? torch::empty({gather_.numel()}, gather_.options().device(device)) : torch::Tensor{};
    // Allocation and upload precede commit. Old tensors retain their original
    // allocator stream; queued consumers never borrow the replacement storage.
    slots_ = std::move(slots);
    references_ = std::move(references);
    gather_ = std::move(gather);
    prepared_stream_ = stream;
}
torch::Tensor ClassPostprocessLane::Gather(const torch::Tensor& logits) {
    if (logits.dim() != 3 || static_cast<std::size_t>(logits.size(2)) != layout_->output_width())
        throw std::invalid_argument("logit width disagrees with admitted class layout");
    if (prefix_identity_ || eligible_count() == 0) return logits.narrow(2, 0, eligible_count());
    const auto stream = logits.is_cuda() ? at::cuda::getCurrentCUDAStream(logits.get_device()).stream() : nullptr;
    if (!slots_.defined() || slots_.device() != logits.device() || prepared_stream_ != stream)
        throw std::invalid_argument("class postprocessor was not prepared on this device and stream");
    if (logits.numel() == 0) return logits.narrow(2, 0, eligible_count());
    const auto rows = checked_prediction_extent(logits.size(0), logits.size(1), kMaximumPredictionTensorBytes);
    const auto values = checked_prediction_extent(rows, eligible_count(), kMaximumPredictionTensorBytes);
    static_cast<void>(checked_prediction_extent(values, logits.element_size(), kMaximumPredictionTensorBytes));
    const bool replace = !gather_.defined() || gather_.scalar_type() != logits.scalar_type() || static_cast<std::size_t>(gather_.numel()) < values;
    auto candidate = replace ? torch::empty({static_cast<std::int64_t>(values)}, logits.options()) : gather_;
    auto output = candidate.narrow(0, 0, static_cast<std::int64_t>(values)).view({logits.size(0), logits.size(1), static_cast<std::int64_t>(eligible_count())});
    at::index_select_out(output, logits, 2, slots_);
    if (replace) gather_ = std::move(candidate);
    return output;
}
torch::Tensor ClassPostprocessLane::References(const torch::Tensor& indices) const {
    if (prefix_identity_ || eligible_count() == 0) return indices;
    const auto stream = indices.is_cuda() ? at::cuda::getCurrentCUDAStream(indices.get_device()).stream() : nullptr;
    if (indices.device() != references_.device() || prepared_stream_ != stream) throw std::invalid_argument("class references used outside the prepared lane");
    // This allocation is the independently owned final label result, not
    // temporary remap scratch. Earlier returned labels survive subsequent Runs.
    return references_.index_select(0, indices.flatten()).view(indices.sizes());
}
PostprocessedSelection select_output_batch_fixed_size(const OutputTensors& outputs, int64_t height, int64_t width, int64_t count, bool require_masks,
                                                      ClassPostprocessLane* classes) {
    if (require_masks && !outputs.pred_masks) throw std::runtime_error("RF-DETR requested masks are absent");
    auto core = postprocess_core(outputs, nullptr, std::make_pair(height, width), count, classes);
    if (outputs.pred_masks &&
        (outputs.pred_masks->dim() != 4 || outputs.pred_masks->size(0) != outputs.pred_logits.size(0) ||
         outputs.pred_masks->size(1) != outputs.pred_logits.size(1) || outputs.pred_masks->size(2) <= 0 || outputs.pred_masks->size(3) <= 0 ||
         !outputs.pred_masks->is_floating_point() || outputs.pred_masks->device() != outputs.pred_logits.device()))
        throw std::invalid_argument("RF-DETR mask logits are incompatible with selected queries");
    if (outputs.pred_masks)
        static_cast<void>(checked_prediction_extent(outputs.pred_masks->numel(), outputs.pred_masks->element_size(), kMaximumPredictionTensorBytes));
    return {std::move(core.scores), std::move(core.labels), std::move(core.boxes), std::move(core.query_indices), outputs.pred_masks};
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
    PostprocessedBatch result{selected.scores, selected.labels, selected.boxes, std::nullopt};
    if (selected.mask_logits) result.masks = materialize_selected_masks(*selected.mask_logits, selected.query_indices, target_height, target_width);
    return result;
}
std::vector<TensorMap> split_postprocessed_batch(const PostprocessedBatch& batch) {
    std::vector<TensorMap> results;
    results.reserve(static_cast<size_t>(batch.size()));
    for (int64_t image_index = 0; image_index < batch.size(); ++image_index) {
        TensorMap result;
        result["scores"] = batch.scores[image_index];
        result["labels"] = batch.labels[image_index];
        result["boxes"] = batch.boxes[image_index];
        if (batch.masks.has_value()) { result["masks"] = (*batch.masks)[image_index]; }
        results.push_back(std::move(result));
    }
    return results;
}
PostprocessedBatch postprocessed_batch_from_result(const TensorMap& result) {
    PostprocessedBatch batch;
    batch.scores = result.at("scores").unsqueeze(0);
    batch.labels = result.at("labels").unsqueeze(0);
    batch.boxes = result.at("boxes").unsqueeze(0);
    if (const auto masks = result.find("masks"); masks != result.end()) {
        torch::Tensor mask_values = masks->second;
        if (mask_values.dim() == 4 && mask_values.size(1) == 1) { mask_values = mask_values.squeeze(1); }
        if (mask_values.dim() != 3) { throw std::runtime_error("predicted masks must be [num_predictions,height,width]"); }
        batch.masks = mask_values.unsqueeze(0);
    }
    return batch;
}
std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs, const torch::Tensor& target_sizes, int64_t num_select, ClassPostprocessLane* classes) {
    PostprocessCore core = postprocess_core(outputs, &target_sizes, std::nullopt, num_select, classes);
    std::vector<TensorMap> results;
    results.reserve(static_cast<size_t>(outputs.pred_logits.size(0)));
    if (outputs.pred_masks.has_value()) {
        const auto& out_masks = *outputs.pred_masks;
        mmltk::common::logging::ScopedProfile profile_rfdetr_native_postprocess_masks{"rfdetr.native.postprocess.masks"};
        for (int64_t batch = 0; batch < out_masks.size(0); ++batch) {
            TensorMap result;
            result["scores"] = core.scores[batch];
            result["labels"] = core.labels[batch];
            result["boxes"] = core.boxes[batch];
            const auto gather_index =
                core.query_indices[batch].unsqueeze(-1).unsqueeze(-1).expand({core.query_indices.size(1), out_masks.size(-2), out_masks.size(-1)});
            auto masks = out_masks[batch].gather(0, gather_index);
            const auto size_cpu = target_sizes[batch].to(torch::kCPU);
            const int64_t height = size_cpu[0].item<int64_t>();
            const int64_t width = size_cpu[1].item<int64_t>();
            auto interpolate_options = torch::nn::functional::InterpolateFuncOptions();
            interpolate_options.size(std::vector<int64_t>{height, width});
            interpolate_options.mode(torch::kBilinear);
            interpolate_options.align_corners(false);
            masks = torch::nn::functional::interpolate(masks.unsqueeze(1), interpolate_options).gt(0.0);
            result["masks"] = masks;
            results.push_back(std::move(result));
        }
    } else {
        for (int64_t batch = 0; batch < outputs.pred_logits.size(0); ++batch) {
            TensorMap result;
            result["scores"] = core.scores[batch];
            result["labels"] = core.labels[batch];
            result["boxes"] = core.boxes[batch];
            results.push_back(std::move(result));
        }
    }
    return results;
}
std::vector<TensorMap> postprocess_outputs(const ModelOutputs& outputs, const torch::Tensor& target_sizes, const int64_t num_select,
                                           ClassPostprocessLane* classes) {
    return postprocess_outputs(OutputTensors{outputs.main.pred_logits, outputs.main.pred_boxes, outputs.main.pred_masks}, target_sizes, num_select, classes);
}
std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs, int64_t target_height, int64_t target_width, int64_t num_select,
                                                      ClassPostprocessLane* classes) {
    return split_postprocessed_batch(postprocess_output_batch_fixed_size(outputs, target_height, target_width, num_select, classes));
}
}  // namespace mmltk::backend::models::rfdetr
