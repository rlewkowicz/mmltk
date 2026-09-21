#include <cuda_runtime_api.h>
#include <stdexcept>
#include <string>
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/cuda_priority.h"
namespace mmltk::frameworks::gpu {
namespace {
cudaError_t runtime_get_device(void*, int* device) noexcept { return cudaGetDevice(device); }
cudaError_t runtime_set_device(void*, const int device) noexcept { return cudaSetDevice(device); }
}  // namespace
void ensure_cuda_ok(const cudaError_t status, const char* context) {
 if (status != cudaSuccess) { throw CudaError(status, context); }
}
void ensure_cuda_driver_ok(const CUresult status, const char* operation) {
 if (status == CUDA_SUCCESS) return;
 const char* detail = nullptr;
 (void)cuGetErrorString(status, &detail);
 throw std::runtime_error(std::string(operation) + ": " + (detail ? detail : "CUDA driver failure"));
}
int current_cuda_highest_stream_priority() {
 int least_priority = 0;
 int greatest_priority = 0;
 const cudaError_t status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
 ensure_cuda_ok(status, "cudaDeviceGetStreamPriorityRange");
 return greatest_priority;
}
cudaError_t cuda_stream_create_with_highest_priority(cudaStream_t* stream, const unsigned int flags) noexcept {
 int least_priority = 0;
 int greatest_priority = 0;
 const cudaError_t status = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
 if (status != cudaSuccess) return status;
 return cudaStreamCreateWithPriority(stream, flags, greatest_priority);
}
CudaDeviceApi cuda_runtime_device_api() noexcept { return {.get = runtime_get_device, .set = runtime_set_device}; }
}  // namespace mmltk::frameworks::gpu
