#pragma once
#include <cuda_runtime.h>
namespace mmltk::frameworks::gpu {
[[nodiscard]] int current_cuda_highest_stream_priority();
[[nodiscard]] cudaError_t cuda_stream_create_with_highest_priority(cudaStream_t* stream, unsigned int flags) noexcept;
}  // namespace mmltk::frameworks::gpu
