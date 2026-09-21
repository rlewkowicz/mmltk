#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>
#include "detail/ms_deform_attn_cuda.h"
#include "detail/ms_deform_attn_cuda_launch.h"
namespace mmltk::backend::ml::layers {
namespace {
struct DeformAttnDims {
 int batch;
 int spatial_size;
 int num_heads;
 int channels;
 int num_levels;
 int num_query;
 int num_point;
 int im2col_step;
};
int checked_dimension(const int64_t value, const char* name) {
 TORCH_CHECK(value >= 0 && value <= std::numeric_limits<int>::max(), name, " exceeds the CUDA kernel integer range");
 return static_cast<int>(value);
}
bool product_fits(const std::initializer_list<std::int64_t> factors, const std::int64_t limit) {
 std::int64_t product = 1;
 for (const std::int64_t factor : factors) {
  if (factor < 0 || (factor != 0 && product > limit / factor)) { return false; }
  product *= factor;
 }
 return true;
}
struct ValidatedDeformAttn final {
 DeformAttnDims dims;
 ms_deform_attn_launch::DeviceLayout layout;
};
ValidatedDeformAttn validate_deform_attn_inputs(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
                                                const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight, int64_t im2col_step, const char* context) {
 TORCH_CHECK(value.dim() == 4, "value must have shape [batch, spatial, heads, channels]");
 TORCH_CHECK(spatial_shapes.dim() == 2 && spatial_shapes.size(1) == 2, "spatial_shapes must have shape [levels, 2]");
 TORCH_CHECK(level_start_index.dim() == 1, "level_start_index must have shape [levels]");
 TORCH_CHECK(sampling_loc.dim() == 6 && sampling_loc.size(5) == 2, "sampling_loc must have shape [batch, queries, heads, levels, points, 2]");
 TORCH_CHECK(attn_weight.dim() == 5, "attn_weight must have shape [batch, queries, heads, levels, points]");
 TORCH_CHECK(im2col_step > 0, "im2col_step must be positive");
 TORCH_CHECK(value.is_contiguous(), "value tensor has to be contiguous");
 TORCH_CHECK(spatial_shapes.is_contiguous(), "spatial_shapes tensor has to be contiguous");
 TORCH_CHECK(level_start_index.is_contiguous(), "level_start_index tensor has to be contiguous");
 TORCH_CHECK(sampling_loc.is_contiguous(), "sampling_loc tensor has to be contiguous");
 TORCH_CHECK(attn_weight.is_contiguous(), "attn_weight tensor has to be contiguous");
 TORCH_CHECK(value.is_cuda(), "value must be a CUDA tensor");
 TORCH_CHECK(spatial_shapes.device().is_cpu(), "spatial_shapes must be authoritative CPU metadata");
 TORCH_CHECK(level_start_index.device().is_cpu(), "level_start_index must be authoritative CPU metadata");
 TORCH_CHECK(sampling_loc.is_cuda(), "sampling_loc must be a CUDA tensor");
 TORCH_CHECK(attn_weight.is_cuda(), "attn_weight must be a CUDA tensor");
 TORCH_CHECK(value.device() == sampling_loc.device() && value.device() == attn_weight.device(), "deformable attention tensors must share one CUDA device");
 TORCH_CHECK(value.scalar_type() == torch::kFloat32, context, " expects float32 value");
 TORCH_CHECK(spatial_shapes.scalar_type() == torch::kInt64, "spatial_shapes must use int64");
 TORCH_CHECK(level_start_index.scalar_type() == torch::kInt64, "level_start_index must use int64");
 TORCH_CHECK(sampling_loc.scalar_type() == torch::kFloat32, context, " expects float32 sampling_loc");
 TORCH_CHECK(attn_weight.scalar_type() == torch::kFloat32, context, " expects float32 attn_weight");
 const auto value_sizes = value.sizes();
 const auto spatial_shape_sizes = spatial_shapes.sizes();
 const auto sampling_loc_sizes = sampling_loc.sizes();
 const int batch = checked_dimension(value_sizes[0], "batch");
 const int spatial_size = checked_dimension(value_sizes[1], "spatial size");
 const int num_heads = checked_dimension(value_sizes[2], "head count");
 const int channels = checked_dimension(value_sizes[3], "channel count");
 const int num_levels = checked_dimension(spatial_shape_sizes[0], "level count");
 TORCH_CHECK(num_levels <= ms_deform_attn_launch::kMaxLevels, "level count exceeds the bounded CUDA metadata capacity");
 const int num_query = checked_dimension(sampling_loc_sizes[1], "query count");
 const int num_point = checked_dimension(sampling_loc_sizes[4], "point count");
 const int checked_step = checked_dimension(im2col_step, "im2col_step");
 const int im2col_step_ = std::min(batch, checked_step);
 TORCH_CHECK(batch > 0, "deformable attention batch must be positive");
 TORCH_CHECK(spatial_size > 0 && num_heads > 0 && channels > 0 && num_levels > 0 && num_query > 0 && num_point > 0,
             "deformable attention dimensions must be positive");
 TORCH_CHECK(spatial_shapes.size(0) == level_start_index.size(0), "spatial shape and level-start counts must agree");
 TORCH_CHECK(sampling_loc.size(0) == batch && attn_weight.size(0) == batch && sampling_loc.size(1) == attn_weight.size(1) &&
              sampling_loc.size(2) == attn_weight.size(2) && sampling_loc.size(3) == attn_weight.size(3) && sampling_loc.size(4) == attn_weight.size(4),
             "sampling locations and attention weights must have compatible shapes");
 TORCH_CHECK(sampling_loc.size(2) == num_heads && sampling_loc.size(3) == num_levels, "sampling locations must match value heads and spatial levels");
 TORCH_CHECK(batch % im2col_step_ == 0, "im2col_step(%d) must divide batch(%d)", im2col_step_, batch);
 ms_deform_attn_launch::DeviceLayout layout{};
 const auto* shapes = spatial_shapes.data_ptr<int64_t>();
 const auto* starts = level_start_index.data_ptr<int64_t>();
 std::int64_t expected_start = 0;
 for (int level = 0; level < num_levels; ++level) {
  const int height = checked_dimension(shapes[level * 2], "level height");
  const int width = checked_dimension(shapes[level * 2 + 1], "level width");
  TORCH_CHECK(height > 0 && width > 0, "deformable attention level extents must be positive");
  TORCH_CHECK(starts[level] == expected_start, "level_start_index must be the canonical contiguous layout");
  TORCH_CHECK(static_cast<std::int64_t>(height) <= (std::numeric_limits<int>::max() - expected_start) / width,
              "deformable attention spatial layout exceeds CUDA integer range");
  layout.spatial_shapes[level][0] = height;
  layout.spatial_shapes[level][1] = width;
  layout.level_start_index[level] = static_cast<int>(expected_start);
  expected_start += static_cast<std::int64_t>(height) * width;
 }
 TORCH_CHECK(expected_start == spatial_size, "deformable attention spatial metadata does not cover value");
 TORCH_CHECK(product_fits({spatial_size, num_heads, channels}, std::numeric_limits<int>::max()),
             "deformable attention value stride exceeds CUDA integer range");
 TORCH_CHECK(product_fits({num_query, num_heads, num_levels, num_point, 2}, std::numeric_limits<int>::max()),
             "deformable attention sampling stride exceeds CUDA integer range");
 TORCH_CHECK(product_fits({num_query, num_heads, channels}, std::numeric_limits<int>::max()),
             "deformable attention head/channel stride exceeds CUDA integer range");
 TORCH_CHECK(product_fits({im2col_step_, num_query, num_heads, channels}, std::numeric_limits<int>::max()),
             "deformable attention launch extent exceeds CUDA integer range");
 TORCH_CHECK(product_fits({im2col_step_, spatial_size, num_heads, channels}, std::numeric_limits<int>::max()),
             "deformable attention value pointer range exceeds CUDA integer range");
 TORCH_CHECK(product_fits({im2col_step_, num_query, num_heads, num_levels, num_point, 2}, std::numeric_limits<int>::max()),
             "deformable attention sampling pointer range exceeds CUDA integer range");
 return {{batch, spatial_size, num_heads, channels, num_levels, num_query, num_point, im2col_step_}, layout};
}
[[nodiscard]] ms_deform_attn_launch::CommonLaunch make_tensor_launch_common(const torch::Tensor& value, const ms_deform_attn_launch::DeviceLayout& layout,
                                                                            const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight,
                                                                            const DeformAttnDims& dims, cudaStream_t stream) {
 return ms_deform_attn_launch::make_common_launch(value.data_ptr<float>(), layout, sampling_loc.data_ptr<float>(), attn_weight.data_ptr<float>(), dims.batch,
                                                  dims.spatial_size, dims.num_heads, dims.channels, dims.num_levels, dims.num_query, dims.num_point,
                                                  dims.im2col_step, stream);
}  // namespace
[[nodiscard]] ms_deform_attn_launch::CommonLaunch make_current_tensor_launch_common(const torch::Tensor& value,
                                                                                    const ms_deform_attn_launch::DeviceLayout& layout,
                                                                                    const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight,
                                                                                    const DeformAttnDims& dims) {
 return make_tensor_launch_common(value, layout, sampling_loc, attn_weight, dims, at::cuda::getCurrentCUDAStream());
}  // namespace mmltk::backend::ml::layers
}  // namespace
torch::Tensor ms_deform_attn_cuda_forward(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
                                          const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight, int64_t im2col_step) {
 const auto validated =
  validate_deform_attn_inputs(value, spatial_shapes, level_start_index, sampling_loc, attn_weight, im2col_step, "ms_deform_attn_cuda_forward");
 const auto& dims = validated.dims;
 c10::cuda::CUDAGuard device_guard(value.device());
 auto output = at::zeros({dims.batch, dims.num_query, dims.num_heads, dims.channels}, value.options());
 launch_ms_deform_attn_cuda_forward(ms_deform_attn_launch::ForwardLaunch{
  make_current_tensor_launch_common(value, validated.layout, sampling_loc, attn_weight, dims),
  output.data_ptr<float>(),
 });
 const int64_t flattened_channels = static_cast<int64_t>(dims.num_heads) * static_cast<int64_t>(dims.channels);
 return output.view({dims.batch, dims.num_query, flattened_channels});
}
std::vector<torch::Tensor> ms_deform_attn_cuda_backward(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
                                                        const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight, const torch::Tensor& grad_output,
                                                        int64_t im2col_step) {
 const auto validated =
  validate_deform_attn_inputs(value, spatial_shapes, level_start_index, sampling_loc, attn_weight, im2col_step, "ms_deform_attn_cuda_backward");
 const auto& dims = validated.dims;
 TORCH_CHECK(grad_output.is_contiguous(), "grad_output tensor has to be contiguous");
 TORCH_CHECK(grad_output.is_cuda(), "grad_output must be a CUDA tensor");
 TORCH_CHECK(grad_output.device() == value.device(), "grad_output must share the input CUDA device");
 TORCH_CHECK(grad_output.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_backward expects float32 grad_output");
 TORCH_CHECK(grad_output.dim() == 3 && grad_output.size(0) == dims.batch && grad_output.size(1) == dims.num_query &&
              grad_output.size(2) == static_cast<int64_t>(dims.num_heads) * dims.channels,
             "grad_output shape does not match deformable attention output");
 c10::cuda::CUDAGuard device_guard(value.device());
 auto grad_value = at::zeros_like(value);
 auto grad_sampling_loc = at::zeros_like(sampling_loc);
 auto grad_attn_weight = at::zeros_like(attn_weight);
 launch_ms_deform_attn_cuda_backward(ms_deform_attn_launch::BackwardLaunch{
  make_current_tensor_launch_common(value, validated.layout, sampling_loc, attn_weight, dims),
  grad_output.data_ptr<float>(),
  grad_value.data_ptr<float>(),
  grad_sampling_loc.data_ptr<float>(),
  grad_attn_weight.data_ptr<float>(),
 });
 return {grad_value, grad_sampling_loc, grad_attn_weight};
}
}  // namespace mmltk::backend::ml::layers
