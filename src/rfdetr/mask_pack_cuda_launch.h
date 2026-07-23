#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace mmltk::rfdetr {

struct PackBoolMasksLaunch {
    const bool* masks = nullptr;
    std::uint8_t* packed_masks = nullptr;
    std::int64_t mask_count = 0;
    std::int64_t pixels_per_mask = 0;
    std::int64_t bytes_per_mask = 0;
    cudaStream_t stream = nullptr;
};

void launch_pack_bool_masks_cuda(const PackBoolMasksLaunch& launch);

}  
