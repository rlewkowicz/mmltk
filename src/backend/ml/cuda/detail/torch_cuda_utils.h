#pragma once
#include <c10/core/Device.h>
#include <c10/core/ScalarType.h>
#include <c10/core/TensorOptions.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>

#include <cstdint>

namespace mmltk::backend::ml::cuda {

using TorchDeviceIndex = ::c10::DeviceIndex;
using TorchScalarType = ::c10::ScalarType;
using TorchTensorOptions = ::c10::TensorOptions;
using TorchCudaDeviceGuard = ::c10::cuda::CUDAGuard;
using TorchCudaStream = ::c10::cuda::CUDAStream;
using TorchCudaStreamGuard = ::c10::cuda::CUDAStreamGuard;
using TorchCudaStreamHandle = ::cudaStream_t;
using ::c10::kCPU;
using ::c10::cuda::getCurrentCUDAStream;
using ::c10::cuda::getStreamFromExternal;
using ::c10::cuda::getStreamFromPool;

[[nodiscard]] TorchDeviceIndex checked_device_index(int device_id);
[[nodiscard]] TorchCudaStream get_priority_cuda_stream(TorchDeviceIndex device_index, int priority);
[[nodiscard]] TorchCudaStream get_priority_cuda_stream(int device_id, int priority);
[[nodiscard]] TorchCudaStream current_torch_cuda_stream_object(TorchDeviceIndex device_index);
[[nodiscard]] TorchCudaStream external_torch_cuda_stream(std::uintptr_t stream, TorchDeviceIndex device_index);

}  // namespace mmltk::backend::ml::cuda
