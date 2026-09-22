#include <ATen/cuda/CUDAContext.h>
#include <limits>
#include "detail/mask_pack_cuda.h"
import mmltk.backend.imaging.raster;
namespace mmltk::backend::models::rfdetr {
void pack_bool_masks_cuda_into(const torch::Tensor& masks, torch::Tensor& packed_masks) {
 TORCH_CHECK(masks.is_cuda(), "pack_bool_masks_cuda_into requires CUDA masks");
 TORCH_CHECK(packed_masks.is_cuda(), "pack_bool_masks_cuda_into requires CUDA packed_masks");
 TORCH_CHECK(masks.scalar_type() == torch::kBool, "pack_bool_masks_cuda_into expects bool masks");
 TORCH_CHECK(packed_masks.scalar_type() == torch::kUInt8, "pack_bool_masks_cuda_into expects uint8 packed_masks");
 TORCH_CHECK(masks.dim() == 4, "pack_bool_masks_cuda_into expects masks shaped [B,K,H,W]");
 TORCH_CHECK(packed_masks.dim() == 3, "pack_bool_masks_cuda_into expects packed_masks shaped [B,K,bytes]");
 TORCH_CHECK(masks.size(0) == packed_masks.size(0) && masks.size(1) == packed_masks.size(1), "pack_bool_masks_cuda_into batch and prediction dimensions must match");
 TORCH_CHECK(masks.is_contiguous(), "pack_bool_masks_cuda_into expects contiguous masks");
 TORCH_CHECK(packed_masks.is_contiguous(), "pack_bool_masks_cuda_into expects contiguous packed_masks");
 constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
 const std::int64_t height = masks.size(2);
 const std::int64_t width = masks.size(3);
 TORCH_CHECK(height == 0 || width <= (kMax - 7) / height, "pack_bool_masks_cuda_into mask dimensions overflow");
 TORCH_CHECK(masks.size(0) == 0 || masks.size(1) <= kMax / masks.size(0), "pack_bool_masks_cuda_into mask count overflows");
 const std::int64_t mask_count = masks.size(0) * masks.size(1);
 const std::int64_t pixels_per_mask = height * width;
 const std::int64_t bytes_per_mask = (pixels_per_mask + 7) / 8;
 TORCH_CHECK(packed_masks.size(2) == bytes_per_mask, "pack_bool_masks_cuda_into packed byte dimension is incorrect");
 if (masks.numel() == 0) { return; }
 mmltk::backend::imaging::raster::BoolMaskPackWork work{};
 work.masks = masks.data_ptr<bool>();
 work.packed_masks = packed_masks.data_ptr<std::uint8_t>();
 work.mask_count = mask_count;
 work.pixels_per_mask = pixels_per_mask;
 work.bytes_per_mask = bytes_per_mask;
 work.stream = at::cuda::getCurrentCUDAStream().stream();
 const cudaError_t status = static_cast<cudaError_t>(mmltk::backend::imaging::raster::pack_bool_masks(work));
 TORCH_CHECK(status == cudaSuccess, "pack_bool_masks_cuda_into raster launch failed: ", cudaGetErrorString(status));
}
}  // namespace mmltk::backend::models::rfdetr
