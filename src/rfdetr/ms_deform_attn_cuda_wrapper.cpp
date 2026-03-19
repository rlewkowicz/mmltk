#include "rfdetr/ms_deform_attn_cuda.h"

#include "rfdetr/cuda_utils.h"
#include "rfdetr/ms_deform_attn_cuda_launch.h"

#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>

#include <algorithm>

namespace fastloader::rfdetr {

torch::Tensor ms_deform_attn_cuda_forward(const torch::Tensor& value,
                                          const torch::Tensor& spatial_shapes,
                                          const torch::Tensor& level_start_index,
                                          const torch::Tensor& sampling_loc,
                                          const torch::Tensor& attn_weight,
                                          int64_t im2col_step) {
    TORCH_CHECK(value.is_contiguous(), "value tensor has to be contiguous");
    TORCH_CHECK(spatial_shapes.is_contiguous(), "spatial_shapes tensor has to be contiguous");
    TORCH_CHECK(level_start_index.is_contiguous(), "level_start_index tensor has to be contiguous");
    TORCH_CHECK(sampling_loc.is_contiguous(), "sampling_loc tensor has to be contiguous");
    TORCH_CHECK(attn_weight.is_contiguous(), "attn_weight tensor has to be contiguous");

    TORCH_CHECK(value.is_cuda(), "value must be a CUDA tensor");
    TORCH_CHECK(spatial_shapes.is_cuda(), "spatial_shapes must be a CUDA tensor");
    TORCH_CHECK(level_start_index.is_cuda(), "level_start_index must be a CUDA tensor");
    TORCH_CHECK(sampling_loc.is_cuda(), "sampling_loc must be a CUDA tensor");
    TORCH_CHECK(attn_weight.is_cuda(), "attn_weight must be a CUDA tensor");
    TORCH_CHECK(value.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_forward expects float32 value");
    TORCH_CHECK(sampling_loc.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_forward expects float32 sampling_loc");
    TORCH_CHECK(attn_weight.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_forward expects float32 attn_weight");

    const auto value_sizes = value.sizes();
    const auto spatial_shape_sizes = spatial_shapes.sizes();
    const auto sampling_loc_sizes = sampling_loc.sizes();
    const int batch = static_cast<int>(value_sizes[0]);
    const int spatial_size = static_cast<int>(value_sizes[1]);
    const int num_heads = static_cast<int>(value_sizes[2]);
    const int channels = static_cast<int>(value_sizes[3]);
    const int num_levels = static_cast<int>(spatial_shape_sizes[0]);
    const int num_query = static_cast<int>(sampling_loc_sizes[1]);
    const int num_point = static_cast<int>(sampling_loc_sizes[4]);
    const int im2col_step_ = std::min(batch, static_cast<int>(im2col_step));

    TORCH_CHECK(batch % im2col_step_ == 0, "batch(%d) must divide im2col_step(%d)", batch, im2col_step_);

    auto output = at::zeros({batch, num_query, num_heads, channels}, value.options());
    launch_ms_deform_attn_cuda_forward(
        value.data_ptr<float>(),
        spatial_shapes.data_ptr<int64_t>(),
        level_start_index.data_ptr<int64_t>(),
        sampling_loc.data_ptr<float>(),
        attn_weight.data_ptr<float>(),
        batch,
        spatial_size,
        num_heads,
        channels,
        num_levels,
        num_query,
        num_point,
        im2col_step_,
        output.data_ptr<float>(),
        at::cuda::getCurrentCUDAStream());
    const int64_t flattened_channels = static_cast<int64_t>(num_heads) * static_cast<int64_t>(channels);
    return output.view({batch, num_query, flattened_channels});
}

std::vector<torch::Tensor> ms_deform_attn_cuda_backward(const torch::Tensor& value,
                                                        const torch::Tensor& spatial_shapes,
                                                        const torch::Tensor& level_start_index,
                                                        const torch::Tensor& sampling_loc,
                                                        const torch::Tensor& attn_weight,
                                                        const torch::Tensor& grad_output,
                                                        int64_t im2col_step) {
    TORCH_CHECK(value.is_contiguous(), "value tensor has to be contiguous");
    TORCH_CHECK(spatial_shapes.is_contiguous(), "spatial_shapes tensor has to be contiguous");
    TORCH_CHECK(level_start_index.is_contiguous(), "level_start_index tensor has to be contiguous");
    TORCH_CHECK(sampling_loc.is_contiguous(), "sampling_loc tensor has to be contiguous");
    TORCH_CHECK(attn_weight.is_contiguous(), "attn_weight tensor has to be contiguous");
    TORCH_CHECK(grad_output.is_contiguous(), "grad_output tensor has to be contiguous");

    TORCH_CHECK(value.is_cuda(), "value must be a CUDA tensor");
    TORCH_CHECK(spatial_shapes.is_cuda(), "spatial_shapes must be a CUDA tensor");
    TORCH_CHECK(level_start_index.is_cuda(), "level_start_index must be a CUDA tensor");
    TORCH_CHECK(sampling_loc.is_cuda(), "sampling_loc must be a CUDA tensor");
    TORCH_CHECK(attn_weight.is_cuda(), "attn_weight must be a CUDA tensor");
    TORCH_CHECK(grad_output.is_cuda(), "grad_output must be a CUDA tensor");
    TORCH_CHECK(value.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_backward expects float32 value");
    TORCH_CHECK(sampling_loc.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_backward expects float32 sampling_loc");
    TORCH_CHECK(attn_weight.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_backward expects float32 attn_weight");
    TORCH_CHECK(grad_output.scalar_type() == torch::kFloat32, "ms_deform_attn_cuda_backward expects float32 grad_output");

    const auto value_sizes = value.sizes();
    const auto spatial_shape_sizes = spatial_shapes.sizes();
    const auto sampling_loc_sizes = sampling_loc.sizes();
    const int batch = static_cast<int>(value_sizes[0]);
    const int spatial_size = static_cast<int>(value_sizes[1]);
    const int num_heads = static_cast<int>(value_sizes[2]);
    const int channels = static_cast<int>(value_sizes[3]);
    const int num_levels = static_cast<int>(spatial_shape_sizes[0]);
    const int num_query = static_cast<int>(sampling_loc_sizes[1]);
    const int num_point = static_cast<int>(sampling_loc_sizes[4]);
    const int im2col_step_ = std::min(batch, static_cast<int>(im2col_step));

    TORCH_CHECK(batch % im2col_step_ == 0, "batch(%d) must divide im2col_step(%d)", batch, im2col_step_);

    auto grad_value = at::zeros_like(value);
    auto grad_sampling_loc = at::zeros_like(sampling_loc);
    auto grad_attn_weight = at::zeros_like(attn_weight);

    launch_ms_deform_attn_cuda_backward(
        value.data_ptr<float>(),
        spatial_shapes.data_ptr<int64_t>(),
        level_start_index.data_ptr<int64_t>(),
        sampling_loc.data_ptr<float>(),
        attn_weight.data_ptr<float>(),
        grad_output.data_ptr<float>(),
        batch,
        spatial_size,
        num_heads,
        channels,
        num_levels,
        num_query,
        num_point,
        im2col_step_,
        grad_value.data_ptr<float>(),
        grad_sampling_loc.data_ptr<float>(),
        grad_attn_weight.data_ptr<float>(),
        at::cuda::getCurrentCUDAStream());

    return {grad_value, grad_sampling_loc, grad_attn_weight};
}

} // namespace fastloader::rfdetr
