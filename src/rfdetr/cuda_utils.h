#pragma once

#include "cuda_priority.h"

#include <c10/core/Device.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>

#include <cuda_runtime.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace mmltk::rfdetr {

inline c10::DeviceIndex checked_device_index(int device_id) {
    if (device_id < std::numeric_limits<c10::DeviceIndex>::min() ||
        device_id > std::numeric_limits<c10::DeviceIndex>::max()) {
        throw std::runtime_error("device id exceeds c10::DeviceIndex range");
    }
    return static_cast<c10::DeviceIndex>(device_id);
}

inline void ensure_cuda_ok(cudaError_t status, const char* context) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

inline c10::cuda::CUDAStream get_high_priority_cuda_stream(c10::DeviceIndex device_index) {
    c10::cuda::CUDAGuard device_guard(device_index);
    return c10::cuda::getStreamFromPool(mmltk::current_cuda_highest_stream_priority(), device_index);
}

inline c10::cuda::CUDAStream get_high_priority_cuda_stream(int device_id) {
    return get_high_priority_cuda_stream(checked_device_index(device_id));
}

} // namespace mmltk::rfdetr
