module;
#include <ATen/autocast_mode.h>
#include <c10/core/InferenceMode.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <stdexcept>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/backend/ml/cuda/torch_autocast_scope.h"
module mmltk.backend.ml.cuda.torch_scope;
namespace mmltk::backend::ml::cuda {
namespace {
[[nodiscard]] c10::DeviceIndex execution_device_index(const std::int32_t device) {
 if (device < 0) { throw std::invalid_argument("LibTorch CUDA stream scope requires a valid device"); }
 return checked_device_index(device);
}
[[nodiscard]] at::ScalarType scalar_type(const TorchCudaPrecision precision) {
 switch (precision) {
  case TorchCudaPrecision::Float32: return at::kFloat;
  case TorchCudaPrecision::Float16: return at::kHalf;
  case TorchCudaPrecision::BFloat16: return at::kBFloat16;
 }
 throw std::invalid_argument("invalid LibTorch CUDA precision");
}
[[nodiscard]] c10::cuda::CUDAStream torch_stream(const std::uintptr_t stream, const c10::DeviceIndex device_index) {
 // A same-device CUDAGuard may leave a fresh worker without a driver
 // context, especially once LibTorch has initialized its stream tables.
 // Pinned host work needs that context before the first tensor allocation.
 CUcontext current{};
 const auto context_status = cuCtxGetCurrent(&current);
 if (context_status != CUDA_SUCCESS && context_status != CUDA_ERROR_NOT_INITIALIZED) throw std::runtime_error("failed to inspect LibTorch CUDA execution context");
 if (current == nullptr) {
  const auto status = cudaSetDevice(device_index);
  if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
 }
 if (stream == 0U) { return c10::cuda::getDefaultCUDAStream(device_index); }
 return c10::cuda::getStreamFromExternal(reinterpret_cast<cudaStream_t>(stream), device_index);
}
}  // namespace
void run_on_torch_cuda_stream(const std::int32_t device, const std::uintptr_t stream, void* const context, const TorchCudaWork work) {
 if (work == nullptr) { throw std::invalid_argument("LibTorch CUDA stream scope requires work"); }
 const auto device_index = execution_device_index(device);
 c10::cuda::CUDAGuard device_guard{device_index};
 c10::cuda::CUDAStreamGuard stream_guard{torch_stream(stream, device_index)};
 work(context);
}
void run_with_torch_cuda_scope(const TorchCudaExecutionOptions& options, void* const context, const TorchCudaWork work) {
 if (work == nullptr) { throw std::invalid_argument("LibTorch CUDA execution scope requires work"); }
 const auto device_index = execution_device_index(options.device);
 c10::cuda::CUDAGuard device_guard{device_index};
 c10::cuda::CUDAStreamGuard stream_guard{torch_stream(options.stream, device_index)};
 c10::InferenceMode inference_mode{options.inference_mode};
 TorchAutocastScope autocast_scope{options.autocast, scalar_type(options.precision)};
 work(context);
}
std::uintptr_t current_torch_cuda_stream(const std::int32_t device) {
 const auto device_index = execution_device_index(device);
 c10::cuda::CUDAGuard device_guard{device_index};
 return reinterpret_cast<std::uintptr_t>(c10::cuda::getCurrentCUDAStream(device_index).stream());
}
TorchCudaPrecision preferred_torch_cuda_precision(const std::int32_t device) {
 if (device < 0) { throw std::invalid_argument("LibTorch CUDA precision requires a valid device"); }
 cudaDeviceProp properties{};
 const cudaError_t status = cudaGetDeviceProperties(&properties, device);
 if (status != cudaSuccess) { throw std::runtime_error("failed to inspect CUDA device precision"); }
 if (properties.major < 7) { throw std::runtime_error("LibTorch CUDA autocast requires compute capability 7.0"); }
 return properties.major >= 8 ? TorchCudaPrecision::BFloat16 : TorchCudaPrecision::Float16;
}
}  // namespace mmltk::backend::ml::cuda
