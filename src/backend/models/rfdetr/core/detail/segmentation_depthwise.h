#pragma once
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
// The mask block's fixed 3x3, stride-one depthwise operator. Its custom VJP
// retains the original weight precision independently of forward autocast.
[[nodiscard]] torch::Tensor segmentation_depthwise(const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias);
}  // namespace mmltk::backend::models::rfdetr
