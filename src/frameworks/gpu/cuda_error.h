#pragma once
#include <cuda_runtime_api.h>
#include <cuda.h>
#include <stdexcept>
#include <string>
namespace mmltk::frameworks::gpu {
[[nodiscard]] constexpr bool cuda_shared_failure(cudaError_t status) noexcept {
 switch (status) {
  case cudaErrorInitializationError:
  case cudaErrorCudartUnloading:
  case cudaErrorStubLibrary:
  case cudaErrorInsufficientDriver:
  case cudaErrorCallRequiresNewerDriver:
  case cudaErrorIncompatibleDriverContext:
  case cudaErrorNoDevice:
  case cudaErrorInvalidDevice:
  case cudaErrorDeviceNotLicensed:
  case cudaErrorSoftwareValidityNotEstablished:
  case cudaErrorStartupFailure:
  case cudaErrorDeviceUninitialized:
  case cudaErrorInvalidGraphicsContext:
  case cudaErrorDevicesUnavailable:
  case cudaErrorContextIsDestroyed:
  case cudaErrorIllegalAddress:
  case cudaErrorAssert:
  case cudaErrorLaunchFailure:
  case cudaErrorLaunchTimeout:
  case cudaErrorHardwareStackError:
  case cudaErrorIllegalInstruction:
  case cudaErrorMisalignedAddress:
  case cudaErrorInvalidAddressSpace:
  case cudaErrorInvalidPc:
  case cudaErrorECCUncorrectable:
  case cudaErrorNvlinkUncorrectable:
  case cudaErrorContained:
  case cudaErrorTensorMemoryLeak:
  case cudaErrorSystemNotReady:
  case cudaErrorSystemDriverMismatch:
  case cudaErrorCompatNotSupportedOnDevice:
  case cudaErrorMpsConnectionFailed:
  case cudaErrorMpsRpcFailure:
  case cudaErrorMpsServerNotReady:
  case cudaErrorMpsClientTerminated:
  case cudaErrorExternalDevice:
  case cudaErrorStreamDetached:
  case cudaErrorUnknown: return true;
  default: return false;
 }
}
// Driver/runtime error identities share CUDA's status numbers, including the
// driver INVALID_CONTEXT spelling of runtime DeviceUninitialized.
[[nodiscard]] constexpr bool cuda_shared_failure(CUresult status) noexcept { return cuda_shared_failure(static_cast<cudaError_t>(status)); }
static_assert(static_cast<int>(CUDA_ERROR_INVALID_CONTEXT) == static_cast<int>(cudaErrorDeviceUninitialized));
static_assert(static_cast<int>(CUDA_ERROR_DEINITIALIZED) == static_cast<int>(cudaErrorCudartUnloading));
static_assert(static_cast<int>(CUDA_ERROR_OUT_OF_MEMORY) == static_cast<int>(cudaErrorMemoryAllocation));
class CudaError final : public std::runtime_error {
public:
 CudaError(cudaError_t status, const char* context) : std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status)), status_(status) {}
 [[nodiscard]] cudaError_t status() const noexcept { return status_; }
 [[nodiscard]] bool shared_failure() const noexcept { return cuda_shared_failure(status_); }

private:
 cudaError_t status_;
};
void ensure_cuda_ok(cudaError_t status, const char* context);
void ensure_cuda_driver_ok(CUresult status, const char* operation);
}  // namespace mmltk::frameworks::gpu
