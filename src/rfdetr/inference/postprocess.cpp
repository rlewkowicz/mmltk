#include "rfdetr/postprocess.h"

#include "profile_utils.h"

#include <ATen/TensorIndexing.h>
#include <ATen/cuda/CUDAContext.h>
#include <torch/nn/functional/upsampling.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace F = torch::nn::functional;
using namespace torch::indexing;

namespace mmltk::rfdetr {

torch::Tensor box_cxcywh_to_xyxy(const torch::Tensor& value) {
    const auto x_c = value.select(-1, 0);
    const auto y_c = value.select(-1, 1);
    const auto w = value.select(-1, 2).clamp_min(0.0);
    const auto h = value.select(-1, 3).clamp_min(0.0);
    return torch::stack(
        {
            x_c - 0.5 * w,
            y_c - 0.5 * h,
            x_c + 0.5 * w,
            y_c + 0.5 * h,
        },
        -1);
}

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
    if (boxes.is_cuda()) {
        stream = at::cuda::getCurrentCUDAStream(boxes.get_device()).stream();
    }
    thread_local std::vector<FixedBoxScaleCacheEntry> cache;
    for (const auto& entry : cache) {
        if (entry.device == boxes.device() && entry.stream == stream && entry.height == height &&
            entry.width == width) {
            return entry.scale;
        }
    }

    cache.push_back(FixedBoxScaleCacheEntry{
        boxes.device(),
        stream,
        height,
        width,
        torch::tensor({width, height, width, height}, boxes.options().dtype(torch::kFloat32)).view({1, 1, 4}),
    });
    return cache.back().scale;
}

PostprocessCore postprocess_core(const OutputTensors& outputs, const torch::Tensor* target_sizes,
                                 std::optional<std::pair<int64_t, int64_t>> fixed_size, int64_t num_select) {
    MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.total");
    const auto out_logits = outputs.pred_logits.to(torch::kFloat32);
    const auto out_bbox = outputs.pred_boxes.to(torch::kFloat32);
    if (target_sizes != nullptr && (out_logits.size(0) != target_sizes->size(0) || target_sizes->size(1) != 2)) {
        throw std::runtime_error("target_sizes must be [batch,2] and aligned with RF-DETR outputs");
    }
    if (!fixed_size.has_value() && target_sizes == nullptr) {
        throw std::runtime_error("postprocess_outputs requires target_sizes or a fixed target size");
    }

    PostprocessCore core;
    {
        MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.topk");
        const auto prob = out_logits.sigmoid();
        const auto flat_prob = prob.flatten(1);
        const int64_t k = std::min<int64_t>(num_select, flat_prob.size(1));
        const auto topk = flat_prob.topk(k, 1);
        core.scores = std::get<0>(topk);
        const auto topk_indexes = std::get<1>(topk).to(torch::kInt64);
        core.query_indices = torch::floor_divide(topk_indexes, out_logits.size(2));
        core.labels = topk_indexes.remainder(out_logits.size(2));
    }

    core.boxes = box_cxcywh_to_xyxy(out_bbox);
    {
        MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.gather_boxes");
        core.boxes = core.boxes.gather(
            1, core.query_indices.unsqueeze(-1).expand({core.query_indices.size(0), core.query_indices.size(1), 4}));
    }

    {
        MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.scale_boxes");
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

}  

PostprocessedBatch postprocess_output_batch_fixed_size(const OutputTensors& outputs, int64_t target_height,
                                                       int64_t target_width, int64_t num_select) {
    PostprocessCore core = postprocess_core(outputs, nullptr, std::make_pair(target_height, target_width), num_select);
    PostprocessedBatch result{
        std::move(core.scores),
        std::move(core.labels),
        std::move(core.boxes),
        std::nullopt,
    };
    if (!outputs.pred_masks.has_value()) {
        return result;
    }

    MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.masks");
    const auto& out_masks = *outputs.pred_masks;
    const int64_t batch_size = out_masks.size(0);
    const int64_t selected_count = core.query_indices.size(1);
    if (selected_count == 0) {
        result.masks = torch::empty({batch_size, 0, target_height, target_width},
                                    torch::TensorOptions().dtype(torch::kBool).device(out_masks.device()));
        return result;
    }
    const auto gather_index = core.query_indices.unsqueeze(-1).unsqueeze(-1).expand(
        {batch_size, selected_count, out_masks.size(-2), out_masks.size(-1)});
    auto masks = out_masks.gather(1, gather_index);
    auto interpolate_options = F::InterpolateFuncOptions();
    interpolate_options.size(std::vector<int64_t>{target_height, target_width});
    interpolate_options.mode(torch::kBilinear);
    interpolate_options.align_corners(false);
    result.masks = F::interpolate(masks.flatten(0, 1).unsqueeze(1), interpolate_options)
                       .gt(0.0)
                       .view({batch_size, selected_count, target_height, target_width});
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
        if (batch.masks.has_value()) {
            result["masks"] = (*batch.masks)[image_index];
        }
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
        if (mask_values.dim() == 4 && mask_values.size(1) == 1) {
            mask_values = mask_values.squeeze(1);
        }
        if (mask_values.dim() != 3) {
            throw std::runtime_error("predicted masks must be [num_predictions,height,width]");
        }
        batch.masks = mask_values.unsqueeze(0);
    }
    return batch;
}

std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs, const torch::Tensor& target_sizes,
                                           int64_t num_select) {
    PostprocessCore core = postprocess_core(outputs, &target_sizes, std::nullopt, num_select);

    std::vector<TensorMap> results;
    results.reserve(static_cast<size_t>(outputs.pred_logits.size(0)));
    if (outputs.pred_masks.has_value()) {
        const auto& out_masks = *outputs.pred_masks;
        MMLTK_PROFILE_SCOPE("rfdetr.native.postprocess.masks");
        for (int64_t batch = 0; batch < out_masks.size(0); ++batch) {
            TensorMap result;
            result["scores"] = core.scores[batch];
            result["labels"] = core.labels[batch];
            result["boxes"] = core.boxes[batch];
            const auto gather_index = core.query_indices[batch].unsqueeze(-1).unsqueeze(-1).expand(
                {core.query_indices.size(1), out_masks.size(-2), out_masks.size(-1)});
            auto masks = out_masks[batch].gather(0, gather_index);
            const auto size_cpu = target_sizes[batch].to(torch::kCPU);
            const int64_t height = size_cpu[0].item<int64_t>();
            const int64_t width = size_cpu[1].item<int64_t>();
            auto interpolate_options = F::InterpolateFuncOptions();
            interpolate_options.size(std::vector<int64_t>{height, width});
            interpolate_options.mode(torch::kBilinear);
            interpolate_options.align_corners(false);
            masks = F::interpolate(masks.unsqueeze(1), interpolate_options).gt(0.0);
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

std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs, int64_t target_height,
                                                      int64_t target_width, int64_t num_select) {
    return split_postprocessed_batch(
        postprocess_output_batch_fixed_size(outputs, target_height, target_width, num_select));
}

}  
