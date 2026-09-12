#pragma once

#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>

cudaError_t cuda_vulkan_fill(void* destination, std::size_t pitch,
                            std::uint32_t width, std::uint32_t height,
                            std::uint32_t value, cudaStream_t stream);
