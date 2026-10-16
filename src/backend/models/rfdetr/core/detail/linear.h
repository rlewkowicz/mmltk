#pragma once
#include <ATen/ops/linear.h>
#include <ATen/ops/baddbmm.h>
#include <ATen/ops/stack.h>
#include <torch/nn/modules/linear.h>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace mmltk::backend::models::rfdetr::detail {
// Keep the registered LibTorch module's state, but dispatch the same primitive
// as Python nn.Linear. The pinned C++ frontend splits batched matmul and bias,
// introducing an extra AMP rounding boundary and different parameter VJPs.
[[nodiscard]] inline torch::Tensor linear_forward(const torch::nn::LinearImpl& layer, const torch::Tensor& input) { return at::linear(input, layer.weight, layer.bias); }
[[nodiscard]] torch::Tensor linear_relu_forward(const torch::nn::LinearImpl& layer, const torch::Tensor& input);
// Read live group-local parameters so optimizer updates and gradients retain
// their registered ownership while sharing the upstream batched GEMM.
template <typename LayerAt>
[[nodiscard]] torch::Tensor grouped_linear_forward(const torch::Tensor& input, std::size_t groups, LayerAt&& layer_at) {
 TORCH_CHECK(input.dim() >= 3 && input.size(0) == static_cast<int64_t>(groups) && groups > 0, "grouped linear input does not match its parameter groups");
 std::vector<torch::Tensor> weights, biases;
 weights.reserve(groups);
 biases.reserve(groups);
 for (std::size_t group = 0; group < groups; ++group) {
  const auto& layer = layer_at(group);
  weights.push_back(layer.weight);
  biases.push_back(layer.bias);
 }
 const auto weight = at::stack(weights);
 auto shape = input.sizes().vec();
 shape.back() = weight.size(1);
 return at::baddbmm(at::stack(biases).unsqueeze(1), input.reshape({input.size(0), -1, input.size(-1)}), weight.transpose(1, 2)).reshape(shape);
}
}  // namespace mmltk::backend::models::rfdetr::detail
