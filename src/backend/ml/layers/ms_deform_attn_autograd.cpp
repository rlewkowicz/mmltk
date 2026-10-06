#include <c10/cuda/CUDAGuard.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <torch/autograd.h>
#include <torch/library.h>
#include <torch/torch.h>
#include <vector>
#include "detail/ms_deform_attn_cuda.h"
#include "detail/ms_deform_attn_op_abi.h"
namespace mmltk::backend::ml::layers {
namespace {
using AttentionOp = torch::Tensor(const torch::Tensor&, const torch::Tensor&, const torch::Tensor&, const torch::Tensor&, const torch::Tensor&, int64_t);
const auto& attention_operator() {
 static const auto op = c10::Dispatcher::singleton().findSchemaOrThrow("mmltk::ms_deform_attn", "").typed<AttentionOp>();
 return op;
}
torch::Tensor cast_for_kernel(const torch::Tensor& tensor) {
 if (tensor.scalar_type() == torch::kFloat32) { return tensor.contiguous(); }
 if (tensor.scalar_type() == torch::kHalf || tensor.scalar_type() == torch::kBFloat16) { return tensor.to(torch::kFloat32).contiguous(); }
 TORCH_CHECK(false, "deformable attention CUDA supports float32 inputs or AMP upcasts from fp16/bf16");
}
torch::ScalarType restore_dtype(torch::autograd::AutogradContext* ctx, const char* key) { return static_cast<torch::ScalarType>(ctx->saved_data[key].toInt()); }  // namespace
torch::Tensor attention_cuda(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index, const torch::Tensor& sampling_locations,
 const torch::Tensor& attention_weights, int64_t im2col_step) {
 // This backend also serves InferenceMode, where the Autograd key is absent.
 return ms_deform_attn_cuda_forward(cast_for_kernel(value), spatial_shapes, level_start_index, cast_for_kernel(sampling_locations), cast_for_kernel(attention_weights), im2col_step)
  .to(value.scalar_type());
}
class MsDeformAttnAutograd : public torch::autograd::Function<MsDeformAttnAutograd> {
public:
 static torch::autograd::variable_list forward(torch::autograd::AutogradContext* ctx, const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
  const torch::Tensor& sampling_locations, const torch::Tensor& attention_weights, int64_t im2col_step) {
  TORCH_CHECK(value.is_cuda() && sampling_locations.is_cuda() && attention_weights.is_cuda(), "deformable attention values, locations, and weights must be CUDA tensors");
  TORCH_CHECK(value.device() == sampling_locations.device() && value.device() == attention_weights.device(), "deformable attention CUDA inputs must share one device");
  TORCH_CHECK(spatial_shapes.device().is_cpu() && level_start_index.device().is_cpu(), "deformable attention layout metadata must remain on CPU");
  c10::cuda::CUDAGuard device_guard(value.device());
  auto value_fp32 = cast_for_kernel(value);
  auto sampling_locations_fp32 = cast_for_kernel(sampling_locations);
  auto attention_weights_fp32 = cast_for_kernel(attention_weights);
  at::AutoDispatchBelowADInplaceOrView below_autograd;
  auto output = attention_operator().call(value_fp32, spatial_shapes, level_start_index, sampling_locations_fp32, attention_weights_fp32, im2col_step);
  ctx->save_for_backward({
   value_fp32,
   spatial_shapes,
   level_start_index,
   sampling_locations_fp32,
   attention_weights_fp32,
  });
  ctx->saved_data["im2col_step"] = im2col_step;
  ctx->saved_data["value_dtype"] = static_cast<int64_t>(value.scalar_type());
  ctx->saved_data["sampling_dtype"] = static_cast<int64_t>(sampling_locations.scalar_type());
  ctx->saved_data["attention_dtype"] = static_cast<int64_t>(attention_weights.scalar_type());
  return {output.to(value.scalar_type())};
 }
 static torch::autograd::variable_list backward(torch::autograd::AutogradContext* ctx, torch::autograd::variable_list grad_outputs) {
  const auto saved = ctx->get_saved_variables();
  auto grad_output = grad_outputs.front();
  TORCH_CHECK(grad_output.defined(), "deformable attention backward requires grad_output");
  c10::cuda::CUDAGuard device_guard(saved[0].device());
  grad_output = cast_for_kernel(grad_output);
  auto grads = ms_deform_attn_cuda_backward(saved[0], saved[1], saved[2], saved[3], saved[4], grad_output, ctx->saved_data["im2col_step"].toInt());
  auto grad_value = grads[0].to(restore_dtype(ctx, "value_dtype"));
  auto grad_sampling_locations = grads[1].to(restore_dtype(ctx, "sampling_dtype"));
  auto grad_attention_weights = grads[2].to(restore_dtype(ctx, "attention_dtype"));
  return {
   grad_value,
   torch::Tensor(),
   torch::Tensor(),
   grad_sampling_locations,
   grad_attention_weights,
   torch::Tensor(),
  };
 }
};
torch::Tensor attention_autograd(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index, const torch::Tensor& sampling_locations,
 const torch::Tensor& attention_weights, int64_t im2col_step) {
 return MsDeformAttnAutograd::apply(value, spatial_shapes, level_start_index, sampling_locations, attention_weights, im2col_step).front();
}
}  // namespace
// Keep schema, implementations and the referenced ABI entry in one ordinary
// translation unit: static-library linking retains every registration, while
// custom_function.h stays separate from the logging module importer.
TORCH_LIBRARY_FRAGMENT(mmltk, m) {
 m.def("ms_deform_attn(Tensor value, Tensor spatial_shapes, Tensor level_start_index, Tensor sampling_locations, Tensor attention_weights, int im2col_step) -> Tensor");
}
TORCH_LIBRARY_IMPL(mmltk, CUDA, m) { m.impl("ms_deform_attn", TORCH_FN(attention_cuda)); }
TORCH_LIBRARY_IMPL(mmltk, Autograd, m) { m.impl("ms_deform_attn", TORCH_FN(attention_autograd)); }
namespace detail {
void ms_deform_attn_cuda_autograd_abi(const void* const value, const void* const spatial_shapes, const void* const level_start_index, const void* const sampling_locations,
 const void* const attention_weights, const std::int64_t im2col_step, void* const output) {
 *static_cast<torch::Tensor*>(output) = attention_operator().call(*static_cast<const torch::Tensor*>(value), *static_cast<const torch::Tensor*>(spatial_shapes),
  *static_cast<const torch::Tensor*>(level_start_index), *static_cast<const torch::Tensor*>(sampling_locations), *static_cast<const torch::Tensor*>(attention_weights), im2col_step);
}
}  // namespace detail
}  // namespace mmltk::backend::ml::layers
