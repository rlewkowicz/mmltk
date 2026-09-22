#pragma once
#include <cuda_runtime_api.h>
#include <cstdint>
namespace mmltk::backend::imaging::upscale::shiftlut {
// Fixed session-owned storage; only submits work to the supplied ORT stream.
cudaError_t enqueue(
 const float* input, const float* tables, std::int8_t* first, std::int8_t* second, float* output, std::uint32_t height, std::uint32_t width, cudaStream_t stream, std::int8_t* decisions = nullptr);
}  // namespace mmltk::backend::imaging::upscale::shiftlut
