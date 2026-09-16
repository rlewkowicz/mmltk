#pragma once
#include <torch/types.h>
namespace mmltk::backend::ml::ops {
[[nodiscard]] torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
[[nodiscard]] torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);
}  // namespace mmltk::backend::ml::ops
