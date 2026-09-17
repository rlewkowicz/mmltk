#pragma once
#include <cuda_runtime.h>
#include <cstdint>
namespace mmltk::backend::imaging::upscale {
__device__ __forceinline__ float channel_mean(const std::uint32_t channel) { return channel == 0U ? 0.485F : (channel == 1U ? 0.456F : 0.406F); }
__device__ __forceinline__ float channel_std(const std::uint32_t channel) { return channel == 0U ? 0.229F : (channel == 1U ? 0.224F : 0.225F); }
}  // namespace mmltk::backend::imaging::upscale
