#include <c10/cuda/CUDAGuard.h>
#include <torch/autograd.h>
#include <torch/torch.h>
#include <vector>
#include "detail/ms_deform_attn_cuda.h"
#include "detail/ms_deform_attn_op_abi.h"
namespace mmltk::backend::ml::layers {
namespace {
torch::Tensor cast_for_kernel(const torch::Tensor& tensor) {
 if (tensor.scalar_type() == torch::kFloat32) { return tensor.contiguous(); }
 if (tensor.scalar_type() == torch::kHalf || tensor.scalar_type() == torch::kBFloat16) { return tensor.to(torch::kFloat32).contiguous(); }
 TORCH_CHECK(false, "deformable attention CUDA supports float32 inputs or AMP upcasts from fp16/bf16");
}
torch::ScalarType restore_dtype(torch::autograd::AutogradContext* ctx, const char* key) { return static_cast<torch::ScalarType>(ctx->saved_data[key].toInt()); }  // namespace
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
  auto output = ms_deform_attn_cuda_forward(value_fp32, spatial_shapes, level_start_index, sampling_locations_fp32, attention_weights_fp32, im2col_step);
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
}  // namespace
namespace detail {
void ms_deform_attn_cuda_autograd_abi(const void* const value, const void* const spatial_shapes, const void* const level_start_index, const void* const sampling_locations,
 const void* const attention_weights, const std::int64_t im2col_step, void* const output) {
 auto outputs = MsDeformAttnAutograd::apply(*static_cast<const torch::Tensor*>(value), *static_cast<const torch::Tensor*>(spatial_shapes), *static_cast<const torch::Tensor*>(level_start_index),
  *static_cast<const torch::Tensor*>(sampling_locations), *static_cast<const torch::Tensor*>(attention_weights), im2col_step);
 *static_cast<torch::Tensor*>(output) = outputs.front();
}
}  // namespace detail
}  // namespace mmltk::backend::ml::layers
