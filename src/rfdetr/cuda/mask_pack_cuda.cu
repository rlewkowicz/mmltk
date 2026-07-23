#include "rfdetr/mask_pack_cuda_launch.h"

#include "rfdetr/torch_cuda_utils.h"

namespace mmltk::rfdetr {

namespace {

constexpr int kCudaThreads = 256;

__global__ void pack_bool_masks_kernel(const bool* masks, std::uint8_t* packed_masks, std::int64_t mask_count,
                                       std::int64_t pixels_per_mask, std::int64_t bytes_per_mask) {
    const std::int64_t output_index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total_bytes = mask_count * bytes_per_mask;
    if (output_index >= total_bytes) {
        return;
    }
    const std::int64_t mask_index = output_index / bytes_per_mask;
    const std::int64_t byte_index = output_index - mask_index * bytes_per_mask;
    const std::int64_t first_pixel = byte_index * 8;
    const bool* source = masks + mask_index * pixels_per_mask;
    std::uint8_t packed = 0;
#pragma unroll
    for (int bit = 0; bit < 8; ++bit) {
        const std::int64_t pixel_index = first_pixel + bit;
        if (pixel_index < pixels_per_mask && source[pixel_index]) {
            packed |= static_cast<std::uint8_t>(1U << bit);
        }
    }
    packed_masks[output_index] = packed;
}

}  // namespace

void launch_pack_bool_masks_cuda(const PackBoolMasksLaunch& launch) {
    const std::int64_t total_bytes = launch.mask_count * launch.bytes_per_mask;
    const int blocks = static_cast<int>((total_bytes + kCudaThreads - 1) / kCudaThreads);
    pack_bool_masks_kernel<<<blocks, kCudaThreads, 0, launch.stream>>>(
        launch.masks, launch.packed_masks, launch.mask_count, launch.pixels_per_mask, launch.bytes_per_mask);
    ensure_cuda_ok(cudaGetLastError(), "pack_bool_masks_cuda launch");
}

}  // namespace mmltk::rfdetr
