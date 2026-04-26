#pragma once

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

namespace mmltk::testsupport {

inline void cuda_require(cudaError_t err, const char* expr) {
    if (err == cudaSuccess) {
        return;
    }
    std::fprintf(stderr, "CUDA call failed: %s: %s\n", expr, cudaGetErrorString(err));
    std::abort();
}

}  // namespace mmltk::testsupport

#define CUDA_ASSERT_OK(call) ::mmltk::testsupport::cuda_require((call), #call)
