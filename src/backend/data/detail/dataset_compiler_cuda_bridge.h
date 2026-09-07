#pragma once  // backend.data private C++/CUDA language boundary

#include <cuda_runtime_api.h>

namespace mmltk::backend::data::compiler_internal::cuda_bridge {

[[nodiscard]] cudaError_t create_compiler_stream(cudaStream_t* stream, unsigned int flags) noexcept;
void require_cuda_success(cudaError_t status, const char* context);

}  // namespace mmltk::backend::data::compiler_internal::cuda_bridge
