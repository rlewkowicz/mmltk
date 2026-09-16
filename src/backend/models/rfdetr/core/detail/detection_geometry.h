#pragma once
#include <torch/torch.h>
#include <cstdint>
namespace mmltk::backend::models::rfdetr {
enum class BoxExtentPolicy : std::uint8_t {
    Preserve,
    ClampNonnegative,
};
[[nodiscard]] torch::Tensor box_cxcywh_to_xyxy(const torch::Tensor& boxes, BoxExtentPolicy extent_policy = BoxExtentPolicy::ClampNonnegative);
[[nodiscard]] torch::Tensor pairwise_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
[[nodiscard]] torch::Tensor pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
[[nodiscard]] torch::Tensor aligned_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
[[nodiscard]] torch::Tensor aligned_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
// [...,M,4] x [...,N,4] -> [...,M,N]. Prefix dimensions must be identical,
// which prevents accidental cross-image or cross-group geometry.
[[nodiscard]] torch::Tensor batched_pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
}  // namespace mmltk::backend::models::rfdetr
