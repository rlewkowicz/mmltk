#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace mmltk::backend::ml::cuda {
TorchDeviceIndex checked_device_index(const int device_id) {
    if (device_id < std::numeric_limits<TorchDeviceIndex>::min() || device_id > std::numeric_limits<TorchDeviceIndex>::max()) {
        throw std::runtime_error("device id exceeds c10::DeviceIndex range");
    }
    return static_cast<TorchDeviceIndex>(device_id);
}
TorchCudaStream get_priority_cuda_stream(const TorchDeviceIndex device_index, const int priority) {
    TorchCudaDeviceGuard device_guard(device_index);
    return getStreamFromPool(priority, device_index);
}
TorchCudaStream get_priority_cuda_stream(const int device_id, const int priority) {
    return get_priority_cuda_stream(checked_device_index(device_id), priority);
}
TorchCudaStream current_torch_cuda_stream_object(const TorchDeviceIndex device_index) { return getCurrentCUDAStream(device_index); }
TorchCudaStream external_torch_cuda_stream(const std::uintptr_t stream, const TorchDeviceIndex device_index) {
    return getStreamFromExternal(reinterpret_cast<TorchCudaStreamHandle>(stream), device_index);
}
}  // namespace mmltk::backend::ml::cuda
