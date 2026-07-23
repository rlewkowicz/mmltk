#include "rfdetr/mask_pack_cuda.h"

#include "rfdetr/mask_pack_cuda_launch.h"

#include <ATen/cuda/CUDAContext.h>

namespace mmltk::rfdetr {

void pack_bool_masks_cuda_into(const torch::Tensor& masks, torch::Tensor& packed_masks) {
    TORCH_CHECK(masks.is_cuda(), "pack_bool_masks_cuda_into requires CUDA masks");
    TORCH_CHECK(packed_masks.is_cuda(), "pack_bool_masks_cuda_into requires CUDA packed_masks");
    TORCH_CHECK(masks.scalar_type() == torch::kBool, "pack_bool_masks_cuda_into expects bool masks");
    TORCH_CHECK(packed_masks.scalar_type() == torch::kUInt8, "pack_bool_masks_cuda_into expects uint8 packed_masks");
    TORCH_CHECK(masks.dim() == 4, "pack_bool_masks_cuda_into expects masks shaped [B,K,H,W]");
    TORCH_CHECK(packed_masks.dim() == 3, "pack_bool_masks_cuda_into expects packed_masks shaped [B,K,bytes]");
    TORCH_CHECK(masks.size(0) == packed_masks.size(0) && masks.size(1) == packed_masks.size(1),
                "pack_bool_masks_cuda_into batch and prediction dimensions must match");
    TORCH_CHECK(masks.is_contiguous(), "pack_bool_masks_cuda_into expects contiguous masks");
    TORCH_CHECK(packed_masks.is_contiguous(), "pack_bool_masks_cuda_into expects contiguous packed_masks");

    const std::int64_t pixels_per_mask = masks.size(2) * masks.size(3);
    const std::int64_t bytes_per_mask = (pixels_per_mask + 7) / 8;
    TORCH_CHECK(packed_masks.size(2) == bytes_per_mask, "pack_bool_masks_cuda_into packed byte dimension is incorrect");
    if (masks.numel() == 0) {
        return;
    }
    launch_pack_bool_masks_cuda(PackBoolMasksLaunch{
        masks.data_ptr<bool>(),
        packed_masks.data_ptr<std::uint8_t>(),
        masks.size(0) * masks.size(1),
        pixels_per_mask,
        bytes_per_mask,
        at::cuda::getCurrentCUDAStream(),
    });
}

}  
