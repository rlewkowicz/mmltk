#include "detail/dataset_compiler_cuda_bridge.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_priority.h"
namespace mmltk::backend::data::compiler_internal::cuda_bridge {
cudaError_t create_compiler_stream(cudaStream_t* stream, const unsigned int flags) noexcept {
    return mmltk::frameworks::gpu::cuda_stream_create_with_highest_priority(stream, flags);
}
void require_cuda_success(const cudaError_t status, const char* context) { mmltk::frameworks::gpu::ensure_cuda_ok(status, context); }
}  // namespace mmltk::backend::data::compiler_internal::cuda_bridge
