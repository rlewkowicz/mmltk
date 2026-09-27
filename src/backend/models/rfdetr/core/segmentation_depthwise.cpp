#include "detail/segmentation_depthwise.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include <ATen/ops/convolution_backward.h>
#include <ATen/ops/conv2d.h>
#include <torch/csrc/autograd/custom_function.h>
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace {
// RF-DETR e9a138f, models/heads/segmentation.py::_DepthwiseConvWithoutCuDNN.
// Kernel selection remains operation-local; concurrent training lanes must not
// change process-global cuDNN state.
class SegmentationDepthwise final : public torch::autograd::Function<SegmentationDepthwise> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& input, const torch::Tensor& weight, const std::optional<torch::Tensor>& bias) {
  ctx->save_for_backward({input, weight});
  ctx->saved_data["has_bias"] = bias.has_value();
  return at::conv2d(input, weight, bias, {1, 1}, {1, 1}, {1, 1}, input.size(1));
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list gradients) {
  if (!gradients[0].defined()) return {{}, {}, {}};
  const auto saved = ctx->get_saved_variables();
  const auto& input = saved[0];
  const auto& weight = saved[1];
  mmltk::backend::ml::cuda::TorchAutocastScope precision(false, weight.scalar_type());
  const auto incoming = gradients[0].to(weight.scalar_type());
  const bool input_grad = ctx->needs_input_grad(0);
  const bool weight_grad = ctx->needs_input_grad(1);
  const bool bias_grad = ctx->saved_data["has_bias"].toBool() && ctx->needs_input_grad(2);
  torch::Tensor dx, dw, db;
  if (input_grad || weight_grad) {
   const auto derivatives =
    at::convolution_backward(incoming, input.to(weight.scalar_type()), weight, std::nullopt, {1, 1}, {1, 1}, {1, 1}, false, {0, 0}, input.size(1), {input_grad, weight_grad, false});
   dx = std::get<0>(derivatives);
   dw = std::get<1>(derivatives);
  }
  if (bias_grad) db = incoming.sum({0, 2, 3});
  return {dx, dw, db};
 }
};
}  // namespace
torch::Tensor segmentation_depthwise(const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias) {
 return SegmentationDepthwise::apply(input, weight, bias.defined() ? std::optional<torch::Tensor>{bias} : std::nullopt);
}
}  // namespace mmltk::backend::models::rfdetr
