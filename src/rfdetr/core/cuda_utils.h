#pragma once

#include <c10/core/Device.h>

#include <cuda_runtime.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace fastloader::rfdetr {

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

} // namespace fastloader::rfdetr
