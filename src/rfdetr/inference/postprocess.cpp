#include "rfdetr/postprocess.h"

#include "profile_utils.h"

#include <ATen/TensorIndexing.h>
#include <torch/nn/functional/upsampling.h>

#include <algorithm>
#include <stdexcept>

namespace F = torch::nn::functional;
using namespace torch::indexing;

namespace fastloader::rfdetr {

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
        -1
    );
}

static std::vector<TensorMap> postprocess_outputs_impl(
    const OutputTensors& outputs,
    const torch::Tensor* target_sizes,
    std::optional<std::pair<int64_t, int64_t>> fixed_size,
    int64_t num_select) {
    FASTLOADER_PROFILE_SCOPE("rfdetr.native.postprocess.total");
    const auto& out_logits = outputs.pred_logits;
    const auto& out_bbox = outputs.pred_boxes;
    if (target_sizes != nullptr &&
        (out_logits.size(0) != target_sizes->size(0) || target_sizes->size(1) != 2)) {
        throw std::runtime_error("target_sizes must be [batch,2] and aligned with RF-DETR outputs");
    }
    if (!fixed_size.has_value() && target_sizes == nullptr) {
        throw std::runtime_error("postprocess_outputs requires target_sizes or a fixed target size");
    }

    torch::Tensor scores;
    torch::Tensor topk_boxes;
    torch::Tensor labels;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.postprocess.topk");
        const auto prob = out_logits.sigmoid();
        const auto flat_prob = prob.flatten(1);
        const int64_t k = std::min<int64_t>(num_select, flat_prob.size(1));
        const auto topk = flat_prob.topk(k, 1);
        scores = std::get<0>(topk);
        const auto topk_indexes = std::get<1>(topk).to(torch::kInt64);
        topk_boxes = torch::floor_divide(topk_indexes, out_logits.size(2));
        labels = topk_indexes.remainder(out_logits.size(2));
    }

    auto boxes = box_cxcywh_to_xyxy(out_bbox);
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.postprocess.gather_boxes");
        boxes = boxes.gather(1, topk_boxes.unsqueeze(-1).expand({topk_boxes.size(0), topk_boxes.size(1), 4}));
    }

    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.postprocess.scale_boxes");
        if (fixed_size.has_value()) {
            const auto [target_height, target_width] = *fixed_size;
            const auto scale =
                torch::tensor({target_width, target_height, target_width, target_height}, boxes.options())
                    .view({1, 1, 4});
            boxes = boxes * scale;
        } else {
            const auto img_h = target_sizes->select(1, 0);
            const auto img_w = target_sizes->select(1, 1);
            const auto scale = torch::stack({img_w, img_h, img_w, img_h}, 1);
            boxes = boxes * scale.unsqueeze(1);
        }
    }

    std::vector<TensorMap> results;
    results.reserve(static_cast<size_t>(out_logits.size(0)));
    if (outputs.pred_masks.has_value()) {
        const auto& out_masks = *outputs.pred_masks;
        FASTLOADER_PROFILE_SCOPE("rfdetr.native.postprocess.masks");
        for (int64_t batch = 0; batch < out_masks.size(0); ++batch) {
            TensorMap result;
            result["scores"] = scores[batch];
            result["labels"] = labels[batch];
            result["boxes"] = boxes[batch];
            const auto gather_index = topk_boxes[batch].unsqueeze(-1).unsqueeze(-1).expand(
                {topk_boxes.size(1), out_masks.size(-2), out_masks.size(-1)}
            );
            auto masks = out_masks[batch].gather(0, gather_index);
            int64_t height = 0;
            int64_t width = 0;
            if (fixed_size.has_value()) {
                height = fixed_size->first;
                width = fixed_size->second;
            } else {
                const auto size_cpu = (*target_sizes)[batch].to(torch::kCPU);
                height = size_cpu[0].item<int64_t>();
                width = size_cpu[1].item<int64_t>();
            }
            auto interpolate_options = F::InterpolateFuncOptions();
            interpolate_options.size(std::vector<int64_t>{height, width});
            interpolate_options.mode(torch::kBilinear);
            interpolate_options.align_corners(false);
            masks = F::interpolate(masks.unsqueeze(1), interpolate_options).gt(0.0);
            result["masks"] = masks;
            results.push_back(std::move(result));
        }
    } else {
        for (int64_t batch = 0; batch < out_logits.size(0); ++batch) {
            TensorMap result;
            result["scores"] = scores[batch];
            result["labels"] = labels[batch];
            result["boxes"] = boxes[batch];
            results.push_back(std::move(result));
        }
    }
    return results;
}

std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs,
                                           const torch::Tensor& target_sizes,
                                           int64_t num_select) {
    return postprocess_outputs_impl(outputs, &target_sizes, std::nullopt, num_select);
}

std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs,
                                                      int64_t target_height,
                                                      int64_t target_width,
                                                      int64_t num_select) {
    return postprocess_outputs_impl(outputs,
                                    nullptr,
                                    std::make_optional(std::make_pair(target_height, target_width)),
                                    num_select);
}

} // namespace fastloader::rfdetr
