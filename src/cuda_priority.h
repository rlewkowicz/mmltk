#pragma once

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>

namespace mmltk {

inline int current_cuda_highest_stream_priority() {
    int least_priority = 0;
    int greatest_priority = 0;
    const cudaError_t status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string("cudaDeviceGetStreamPriorityRange: ") + cudaGetErrorString(status));
    }
    return greatest_priority;
}

inline cudaError_t cuda_stream_create_with_highest_priority(cudaStream_t* stream, unsigned int flags) {
    int least_priority = 0;
    int greatest_priority = 0;
    cudaError_t status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
    if (status != cudaSuccess) {
        return status;
    }
    return cudaStreamCreateWithPriority(stream, flags, greatest_priority);
}

}  // namespace mmltk
