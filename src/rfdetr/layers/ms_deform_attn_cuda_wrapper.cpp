#include "rfdetr/ms_deform_attn_cuda.h"

#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/ms_deform_attn_cuda_launch.h"

#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>

#include <algorithm>

namespace mmltk::rfdetr {

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

DeformAttnDims validate_deform_attn_inputs(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                           const torch::Tensor& level_start_index, const torch::Tensor& sampling_loc,
                                           const torch::Tensor& attn_weight, int64_t im2col_step, const char* context) {
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
    TORCH_CHECK(value.scalar_type() == torch::kFloat32, context, " expects float32 value");
    TORCH_CHECK(sampling_loc.scalar_type() == torch::kFloat32, context, " expects float32 sampling_loc");
    TORCH_CHECK(attn_weight.scalar_type() == torch::kFloat32, context, " expects float32 attn_weight");

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

    return {batch, spatial_size, num_heads, channels, num_levels, num_query, num_point, im2col_step_};
}

[[nodiscard]] ms_deform_attn_launch::CommonLaunch make_tensor_launch_common(
    const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
    const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight, const DeformAttnDims& dims,
    cudaStream_t stream) {
    return ms_deform_attn_launch::make_common_launch(
        value.data_ptr<float>(), spatial_shapes.data_ptr<int64_t>(), level_start_index.data_ptr<int64_t>(),
        sampling_loc.data_ptr<float>(), attn_weight.data_ptr<float>(), dims.batch, dims.spatial_size, dims.num_heads,
        dims.channels, dims.num_levels, dims.num_query, dims.num_point, dims.im2col_step, stream);
}

[[nodiscard]] ms_deform_attn_launch::CommonLaunch make_current_tensor_launch_common(
    const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index,
    const torch::Tensor& sampling_loc, const torch::Tensor& attn_weight, const DeformAttnDims& dims) {
    return make_tensor_launch_common(value, spatial_shapes, level_start_index, sampling_loc, attn_weight, dims,
                                     at::cuda::getCurrentCUDAStream());
}

}  

torch::Tensor ms_deform_attn_cuda_forward(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                          const torch::Tensor& level_start_index, const torch::Tensor& sampling_loc,
                                          const torch::Tensor& attn_weight, int64_t im2col_step) {
    const auto dims = validate_deform_attn_inputs(value, spatial_shapes, level_start_index, sampling_loc, attn_weight,
                                                  im2col_step, "ms_deform_attn_cuda_forward");

    auto output = at::zeros({dims.batch, dims.num_query, dims.num_heads, dims.channels}, value.options());
    launch_ms_deform_attn_cuda_forward(ms_deform_attn_launch::ForwardLaunch{
        make_current_tensor_launch_common(value, spatial_shapes, level_start_index, sampling_loc, attn_weight, dims),
        output.data_ptr<float>(),
    });
    const int64_t flattened_channels = static_cast<int64_t>(dims.num_heads) * static_cast<int64_t>(dims.channels);
    return output.view({dims.batch, dims.num_query, flattened_channels});
}

std::vector<torch::Tensor> ms_deform_attn_cuda_backward(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                                        const torch::Tensor& level_start_index,
                                                        const torch::Tensor& sampling_loc,
                                                        const torch::Tensor& attn_weight,
                                                        const torch::Tensor& grad_output, int64_t im2col_step) {
    const auto dims = validate_deform_attn_inputs(value, spatial_shapes, level_start_index, sampling_loc, attn_weight,
                                                  im2col_step, "ms_deform_attn_cuda_backward");
    TORCH_CHECK(grad_output.is_contiguous(), "grad_output tensor has to be contiguous");
    TORCH_CHECK(grad_output.is_cuda(), "grad_output must be a CUDA tensor");
    TORCH_CHECK(grad_output.scalar_type() == torch::kFloat32,
                "ms_deform_attn_cuda_backward expects float32 grad_output");

    auto grad_value = at::zeros_like(value);
    auto grad_sampling_loc = at::zeros_like(sampling_loc);
    auto grad_attn_weight = at::zeros_like(attn_weight);

    launch_ms_deform_attn_cuda_backward(ms_deform_attn_launch::BackwardLaunch{
        make_current_tensor_launch_common(value, spatial_shapes, level_start_index, sampling_loc, attn_weight, dims),
        grad_output.data_ptr<float>(),
        grad_value.data_ptr<float>(),
        grad_sampling_loc.data_ptr<float>(),
        grad_attn_weight.data_ptr<float>(),
    });

    return {grad_value, grad_sampling_loc, grad_attn_weight};
}

}  
