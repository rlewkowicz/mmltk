#include "detail/linear.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
#include <ATen/autocast_mode.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/ops/_addmm_activation.h>
#include <ATen/ops/threshold_backward.h>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/library.h>
namespace mmltk::backend::models::rfdetr::detail {
namespace {
using LinearReluOp = torch::Tensor(const torch::Tensor&, const torch::Tensor&, const torch::Tensor&);
const auto& linear_relu_operator() {
 static const auto op = c10::Dispatcher::singleton().findSchemaOrThrow("mmltk::linear_relu", "").typed<LinearReluOp>();
 return op;
}
torch::Tensor linear_relu_cuda(const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias) {
 auto shape = input.sizes().vec();
 shape.back() = weight.size(0);
 return at::_addmm_activation(bias, input.reshape({-1, input.size(-1)}), weight.t()).view(shape);
}
// Pinned upstream Transformer._LinearReLU uses the fused GEMM epilogue and
// supplies its own differentiable backward; ATen has no derivative for this op.
class LinearRelu final : public torch::autograd::Function<LinearRelu> {
public:
 static torch::Tensor forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias) {
  at::AutoDispatchBelowADInplaceOrView below_autograd;
  auto output = linear_relu_operator().call(input, weight, bias);
  ctx->save_for_backward({input, weight, output});
  return output;
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list gradients) {
  if (!gradients[0].defined()) return {{}, {}, {}};
  // CLEANUP-OFF: Saved-tensor aliases and the existing autocast owner serve different autograd-node contracts and derivatives.
  const auto saved = ctx->get_saved_variables();
  const auto& input = saved[0];
  const auto& weight = saved[1];
  mmltk::backend::ml::cuda::TorchAutocastScope precision(false, weight.scalar_type());
  // CLEANUP-ON
  const auto incoming = gradients[0].reshape({-1, weight.size(0)});
  const auto preactivation = at::threshold_backward(incoming, saved[2].reshape_as(incoming), 0);
  return {
   ctx->needs_input_grad(0) ? preactivation.mm(weight).view(input.sizes()) : torch::Tensor{},
   ctx->needs_input_grad(1) ? preactivation.t().mm(input.reshape({-1, input.size(-1)})) : torch::Tensor{},
   ctx->needs_input_grad(2) ? preactivation.sum(0) : torch::Tensor{},
  };
 }
};
torch::Tensor linear_relu_autograd(const torch::Tensor& input, const torch::Tensor& weight, const torch::Tensor& bias) { return LinearRelu::apply(input, weight, bias); }
}  // namespace
// The API and registrations share one object so static linking retains them.
TORCH_LIBRARY_FRAGMENT(mmltk, m) { m.def("linear_relu(Tensor input, Tensor weight, Tensor bias) -> Tensor"); }
TORCH_LIBRARY_IMPL(mmltk, CUDA, m) { m.impl("linear_relu", TORCH_FN(linear_relu_cuda)); }
TORCH_LIBRARY_IMPL(mmltk, Autograd, m) { m.impl("linear_relu", TORCH_FN(linear_relu_autograd)); }
torch::Tensor linear_relu_forward(const torch::nn::LinearImpl& layer, const torch::Tensor& input) {
 const auto device = input.device().type();
 const bool autocast = at::autocast::is_autocast_enabled(device);
 if (!input.is_cuda() || !layer.bias.defined() ||
     (autocast ? input.scalar_type() != torch::kFloat32 || layer.weight.scalar_type() != torch::kFloat32 || layer.bias.scalar_type() != torch::kFloat32
               : input.scalar_type() != layer.weight.scalar_type()))
  return at::relu(linear_forward(layer, input));
 const auto dtype = autocast ? at::autocast::get_autocast_dtype(device) : input.scalar_type();
 const auto x = input.to(dtype), weight = layer.weight.to(dtype), bias = layer.bias.to(dtype);
 c10::impl::ExcludeDispatchKeyGuard precision(at::autocast::get_autocast_dispatch_key_from_device_type(device));
 return linear_relu_operator().call(x, weight, bias);
}
}  // namespace mmltk::backend::models::rfdetr::detail
