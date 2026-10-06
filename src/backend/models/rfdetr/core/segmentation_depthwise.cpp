#include "detail/segmentation_depthwise.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include <ATen/Context.h>
#include <ATen/ops/_convolution.h>
#include <ATen/ops/_convolution_double_backward.h>
#include <ATen/ops/convolution_backward.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/autocast_mode.h>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/library.h>
#include <c10/cuda/CUDAGuard.h>
#include "src/pch_std.h"
namespace at::native {
// Exported by the pinned Torch CUDA payload (DepthwiseConv2d.cu). Its public
// convolution_backward dispatcher reads process-global cuDNN state; selecting
// this exact backend locally preserves upstream's kernel without changing a
// concurrent lane. Keep this declaration local to the operation's owner.
std::tuple<Tensor, Tensor> conv_depthwise2d_backward_cuda(
 const Tensor&, const Tensor&, const Tensor&, IntArrayRef, IntArrayRef, IntArrayRef, IntArrayRef, std::array<bool, 2>);
}
namespace mmltk::backend::models::rfdetr {
namespace {
using DepthwiseOp = torch::Tensor(const torch::Tensor&, const torch::Tensor&, const std::optional<torch::Tensor>&, std::optional<at::ScalarType>);
const auto& depthwise_operator() {
 static const auto op = c10::Dispatcher::singleton().findSchemaOrThrow("mmltk::segmentation_depthwise", "").typed<DepthwiseOp>();
 return op;
}
torch::Tensor depthwise_forward(const torch::Tensor& input, const torch::Tensor& weight, const std::optional<torch::Tensor>& bias, std::optional<at::ScalarType> autocast_dtype) {
 const auto device = input.device().type();
 // The opaque graph node must retain its recording precision even when replay
 // excludes ambient autocast. Preserve the original operands for its VJP.
 const auto x = autocast_dtype ? at::autocast::cached_cast(*autocast_dtype, input, device) : input;
 const auto w = autocast_dtype ? at::autocast::cached_cast(*autocast_dtype, weight, device) : weight;
 const auto b = autocast_dtype ? at::autocast::cached_cast(*autocast_dtype, bias, device) : bias;
 c10::impl::ExcludeDispatchKeyGuard precision(at::autocast::get_autocast_dispatch_key_from_device_type(device));
 const auto& context = at::globalContext();
 return at::_convolution(x, w, b, {1, 1}, {1, 1}, {1, 1}, false, {0, 0}, input.size(1), context.benchmarkCuDNN(),
  context.deterministicCuDNN() || context.deterministicAlgorithms(), false, context.allowTF32CuDNN());
}
std::tuple<torch::Tensor, torch::Tensor> depthwise_cuda_derivatives(
 const torch::Tensor& incoming, const torch::Tensor& input, const torch::Tensor& weight, bool input_grad, bool weight_grad) {
 c10::cuda::CUDAGuard guard(input.device());
 return at::native::conv_depthwise2d_backward_cuda(incoming, input, weight, weight.sizes().slice(2), {1, 1}, {1, 1}, {1, 1}, {input_grad, weight_grad});
}
// The native kernel has no dispatcher autograd node. When a caller requests a
// differentiable VJP, retain the same convolution double-backward primitive as
// ATen's convolution_backward (pinned derivatives.yaml). Ordinary training
// calls the identical CUDA kernel directly without allocating this tape.
class DepthwiseBackward final : public torch::autograd::Function<DepthwiseBackward> {
public:
 static torch::autograd::variable_list forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& incoming, const torch::Tensor& input,
  const torch::Tensor& weight, bool input_grad, bool weight_grad) {
  ctx->save_for_backward({incoming, input, weight});
  ctx->set_materialize_grads(false);
  ctx->saved_data["input_grad"] = input_grad;
  ctx->saved_data["weight_grad"] = weight_grad;
  auto [dx, dw] = depthwise_cuda_derivatives(incoming, input, weight, input_grad, weight_grad);
  // C++ Function forward outputs must be defined; unused derivative slots
  // stay absent instead of allocating placeholder tensors.
  torch::autograd::variable_list result;
  if (input_grad) result.push_back(dx);
  if (weight_grad) result.push_back(dw);
  return result;
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list gradients) {
  const auto saved = ctx->get_saved_variables();
  mmltk::backend::ml::cuda::TorchAutocastScope precision(false, saved[2].scalar_type());
  const bool input_grad = ctx->saved_data["input_grad"].toBool();
  const auto gg_input = input_grad ? gradients[0] : torch::Tensor{};
  const auto gg_weight = ctx->saved_data["weight_grad"].toBool() ? gradients[input_grad ? 1 : 0] : torch::Tensor{};
  auto [dg, dx, dw] = at::_convolution_double_backward(gg_input, gg_weight, std::nullopt, saved[0], saved[2], saved[1],
   {1, 1}, {1, 1}, {1, 1}, false, {0, 0}, saved[1].size(1), {ctx->needs_input_grad(0), ctx->needs_input_grad(1), ctx->needs_input_grad(2)});
  return {dg, dx, dw, {}, {}};
 }
};
// RF-DETR e9a138f, models/heads/segmentation.py::_DepthwiseConvWithoutCuDNN.
// Kernel selection remains operation-local; concurrent training lanes must not
// change process-global cuDNN state.
class SegmentationDepthwise final : public torch::autograd::Function<SegmentationDepthwise> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& input, const torch::Tensor& weight, const std::optional<torch::Tensor>& bias,
  std::optional<at::ScalarType> autocast_dtype) {
  ctx->save_for_backward({input, weight});
  ctx->saved_data["has_bias"] = bias.has_value();
  at::AutoDispatchBelowADInplaceOrView below_autograd;
  return depthwise_operator().call(input, weight, bias, autocast_dtype);
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list gradients) {
  if (!gradients[0].defined()) return {{}, {}, {}, {}};
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
   const auto original_input = input.to(weight.scalar_type());
   if (input.is_cuda()) {
    if (torch::GradMode::is_enabled()) {
     const auto derivatives = DepthwiseBackward::apply(incoming, original_input, weight, input_grad, weight_grad);
     if (input_grad) dx = derivatives[0];
     if (weight_grad) dw = derivatives[input_grad ? 1 : 0];
    } else {
     std::tie(dx, dw) = depthwise_cuda_derivatives(incoming, original_input, weight, input_grad, weight_grad);
    }
   } else {
    const auto derivatives =
     at::convolution_backward(incoming, original_input, weight, std::nullopt, {1, 1}, {1, 1}, {1, 1}, false, {0, 0}, input.size(1), {input_grad, weight_grad, false});
    dx = std::get<0>(derivatives);
    dw = std::get<1>(derivatives);
   }
  }
  if (bias_grad) db = incoming.sum({0, 2, 3});
  return {dx, dw, db, {}};
 }
};
torch::Tensor depthwise_autograd(const torch::Tensor& input, const torch::Tensor& weight, const std::optional<torch::Tensor>& bias, std::optional<at::ScalarType> autocast_dtype) {
 return SegmentationDepthwise::apply(input, weight, bias, autocast_dtype);
}
}  // namespace
// The public function and registrations share one object in the static library.
TORCH_LIBRARY_FRAGMENT(mmltk, m) { m.def("segmentation_depthwise(Tensor input, Tensor weight, Tensor? bias, ScalarType? autocast_dtype) -> Tensor"); }
TORCH_LIBRARY_IMPL(mmltk, CPU, m) { m.impl("segmentation_depthwise", TORCH_FN(depthwise_forward)); }
TORCH_LIBRARY_IMPL(mmltk, CUDA, m) { m.impl("segmentation_depthwise", TORCH_FN(depthwise_forward)); }
TORCH_LIBRARY_IMPL(mmltk, Autograd, m) { m.impl("segmentation_depthwise", TORCH_FN(depthwise_autograd)); }
torch::Tensor segmentation_depthwise(const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias) {
 const auto device = input.device().type();
 const auto dtype = at::autocast::is_autocast_enabled(device) ? std::optional{at::autocast::get_autocast_dtype(device)} : std::nullopt;
 return depthwise_operator().call(input, weight, bias.defined() ? std::optional<torch::Tensor>{bias} : std::nullopt, dtype);
}
}  // namespace mmltk::backend::models::rfdetr
