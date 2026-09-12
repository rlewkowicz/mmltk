#pragma once

#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>

cudaError_t cuda_vulkan_fill(void* destination, std::size_t pitch,
                            std::uint32_t width, std::uint32_t height,
                            std::uint32_t value, cudaStream_t stream);

// The same arithmetic defines the independent host expectation and GPU fill.
#ifdef __CUDACC__
__host__ __device__
#endif
inline std::uint32_t cuda_vulkan_pixel(std::uint32_t seed, std::uint32_t x, std::uint32_t y) {
    return 0xff000000U | ((seed ^ (x * 0x45d9f3bU) ^ (y * 0x119de1f3U)) & 0x00ffffffU);
}
