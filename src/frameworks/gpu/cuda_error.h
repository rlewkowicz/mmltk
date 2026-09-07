#pragma once

#include <cuda_runtime_api.h>

namespace mmltk::frameworks::gpu {

void ensure_cuda_ok(cudaError_t status, const char* context);

}
