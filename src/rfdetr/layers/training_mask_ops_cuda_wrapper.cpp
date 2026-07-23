#include "rfdetr/training_mask_ops_cuda.h"

#include "rfdetr/training_mask_ops_cuda_launch.h"

#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>

namespace mmltk::rfdetr {

torch::Tensor matcher_point_sample_cuda_forward(const torch::Tensor& input, const torch::Tensor& point_coords,
                                                bool nearest) {
    const auto input_sizes = input.sizes();
    const auto coord_sizes = point_coords.sizes();
    TORCH_CHECK(input.is_cuda(), "matcher_point_sample_cuda_forward requires CUDA input");
    TORCH_CHECK(point_coords.is_cuda(), "matcher_point_sample_cuda_forward requires CUDA point_coords");
    TORCH_CHECK(input.scalar_type() == torch::kFloat32, "matcher_point_sample_cuda_forward expects float32 input");
    TORCH_CHECK(point_coords.scalar_type() == torch::kFloat32,
                "matcher_point_sample_cuda_forward expects float32 point_coords");
    TORCH_CHECK(input.dim() == 4, "matcher_point_sample_cuda_forward expects input shaped [N,C,H,W]");
    TORCH_CHECK(point_coords.dim() == 3 && coord_sizes[2] == 2,
                "matcher_point_sample_cuda_forward expects point_coords shaped [N,P,2] or [1,P,2]");
    TORCH_CHECK(coord_sizes[0] == 1 || coord_sizes[0] == input_sizes[0],
                "matcher_point_sample_cuda_forward point_coords batch must match input or be shared");
    TORCH_CHECK(input.is_contiguous(), "matcher_point_sample_cuda_forward expects contiguous input");
    TORCH_CHECK(point_coords.is_contiguous(), "matcher_point_sample_cuda_forward expects contiguous point_coords");

    auto output = at::empty({input_sizes[0], input_sizes[1], coord_sizes[1]}, input.options().dtype(torch::kFloat32));
    if (output.numel() == 0) {
        return output;
    }
    launch_matcher_point_sample_cuda(input.data_ptr<float>(), point_coords.data_ptr<float>(), output.data_ptr<float>(),
                                     input_sizes[0], coord_sizes[0], input_sizes[1], input_sizes[2], input_sizes[3],
                                     coord_sizes[1], nearest, at::cuda::getCurrentCUDAStream());
    return output;
}

torch::Tensor sample_packed_masks_cuda(const torch::Tensor& packed_mask_bits, const int64_t height, const int64_t width,
                                       const torch::Tensor& mask_indices, const torch::Tensor& point_coords) {
    return sample_packed_masks_cuda(packed_mask_bits, height, width, mask_indices, point_coords, {}, {}, {});
}

torch::Tensor sample_packed_masks_cuda(const torch::Tensor& packed_mask_bits, int64_t height, int64_t width,
                                       const torch::Tensor& mask_indices, const torch::Tensor& point_coords,
                                       const torch::Tensor& inverse_transforms,
                                       const torch::Tensor& occluder_mask_indices,
                                       const torch::Tensor& occluder_inverse_transforms) {
    const auto packed_sizes = packed_mask_bits.sizes();
    const auto index_sizes = mask_indices.sizes();
    const auto coord_sizes = point_coords.sizes();
    TORCH_CHECK(packed_mask_bits.is_cuda(), "sample_packed_masks_cuda requires CUDA packed_mask_bits");
    TORCH_CHECK(mask_indices.is_cuda(), "sample_packed_masks_cuda requires CUDA mask_indices");
    TORCH_CHECK(point_coords.is_cuda(), "sample_packed_masks_cuda requires CUDA point_coords");
    TORCH_CHECK(packed_mask_bits.scalar_type() == torch::kInt64,
                "sample_packed_masks_cuda expects int64 packed_mask_bits");
    TORCH_CHECK(mask_indices.scalar_type() == torch::kInt64, "sample_packed_masks_cuda expects int64 mask_indices");
    TORCH_CHECK(point_coords.scalar_type() == torch::kFloat32, "sample_packed_masks_cuda expects float32 point_coords");
    TORCH_CHECK(packed_mask_bits.dim() == 2, "sample_packed_masks_cuda expects packed_mask_bits shaped [N,words]");
    TORCH_CHECK(mask_indices.dim() == 1, "sample_packed_masks_cuda expects 1D mask_indices");
    TORCH_CHECK(point_coords.dim() == 3 && coord_sizes[2] == 2,
                "sample_packed_masks_cuda expects point_coords shaped [N,P,2] or [1,P,2]");
    TORCH_CHECK(coord_sizes[0] == 1 || coord_sizes[0] == index_sizes[0],
                "sample_packed_masks_cuda point_coords batch must match mask_indices or be shared");
    TORCH_CHECK(packed_mask_bits.is_contiguous(), "sample_packed_masks_cuda expects contiguous packed_mask_bits");
    TORCH_CHECK(mask_indices.is_contiguous(), "sample_packed_masks_cuda expects contiguous mask_indices");
    TORCH_CHECK(point_coords.is_contiguous(), "sample_packed_masks_cuda expects contiguous point_coords");
    const bool transformed = inverse_transforms.defined();
    const bool occluded = occluder_mask_indices.defined() || occluder_inverse_transforms.defined();
    TORCH_CHECK(occluder_mask_indices.defined() == occluder_inverse_transforms.defined(),
                "sample_packed_masks_cuda requires both occluder tensors or neither");
    TORCH_CHECK(!occluded || transformed,
                "sample_packed_masks_cuda requires base transforms when occluders are present");
    if (transformed) {
        TORCH_CHECK(inverse_transforms.is_cuda(), "sample_packed_masks_cuda requires CUDA transforms");
        TORCH_CHECK(inverse_transforms.scalar_type() == torch::kFloat32 && inverse_transforms.dim() == 2 &&
                        inverse_transforms.size(1) == 6 && inverse_transforms.stride(1) == 1,
                    "sample_packed_masks_cuda expects inverse transforms shaped [N,6]");
        TORCH_CHECK(inverse_transforms.size(0) == packed_sizes[0],
                    "sample_packed_masks_cuda transform count must match packed masks");
    }
    if (occluded) {
        TORCH_CHECK(occluder_mask_indices.is_cuda() && occluder_inverse_transforms.is_cuda(),
                    "sample_packed_masks_cuda requires CUDA occluder tensors");
        TORCH_CHECK(occluder_mask_indices.scalar_type() == torch::kInt64 && occluder_mask_indices.dim() == 1 &&
                        occluder_mask_indices.size(0) == packed_sizes[0],
                    "sample_packed_masks_cuda expects one int64 occluder index per mask");
        TORCH_CHECK(occluder_inverse_transforms.scalar_type() == torch::kFloat32 &&
                        occluder_inverse_transforms.dim() == 2 &&
                        occluder_inverse_transforms.size(0) == packed_sizes[0] &&
                        occluder_inverse_transforms.size(1) == 6 && occluder_inverse_transforms.stride(1) == 1,
                    "sample_packed_masks_cuda expects occluder transforms shaped [N,6]");
    }

    auto output = at::empty({index_sizes[0], coord_sizes[1]}, point_coords.options().dtype(torch::kFloat32));
    if (output.numel() == 0) {
        return output;
    }
    launch_sample_packed_masks_cuda(
        packed_mask_bits.data_ptr<int64_t>(), mask_indices.data_ptr<int64_t>(), point_coords.data_ptr<float>(),
        transformed ? inverse_transforms.data_ptr<float>() : nullptr,
        occluded ? occluder_mask_indices.data_ptr<int64_t>() : nullptr,
        occluded ? occluder_inverse_transforms.data_ptr<float>() : nullptr, output.data_ptr<float>(), index_sizes[0],
        coord_sizes[0], packed_sizes[1], height, width, coord_sizes[1], transformed ? inverse_transforms.stride(0) : 0,
        at::cuda::getCurrentCUDAStream());
    return output;
}

}  
