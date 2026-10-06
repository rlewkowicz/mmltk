#pragma once
#include <ATen/ops/linear.h>
#include <torch/nn/modules/linear.h>
namespace mmltk::backend::models::rfdetr::detail {
// Keep the registered LibTorch module's state, but dispatch the same primitive
// as Python nn.Linear. The pinned C++ frontend splits batched matmul and bias,
// introducing an extra AMP rounding boundary and different parameter VJPs.
[[nodiscard]] inline torch::Tensor linear_forward(const torch::nn::LinearImpl& layer, const torch::Tensor& input) { return at::linear(input, layer.weight, layer.bias); }
}  // namespace mmltk::backend::models::rfdetr::detail
