#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::upscale::tests {
void normalize_reference(const std::uint8_t* source, std::size_t pitch, std::uint32_t width, std::uint32_t height, float* target, cudaStream_t stream);
}
