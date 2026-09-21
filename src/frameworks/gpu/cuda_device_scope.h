#pragma once
#include <cuda_runtime_api.h>
#include <atomic>
#include <exception>
#include <utility>
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
namespace mmltk::frameworks::gpu {
struct CudaDeviceApi final {
 void* context = nullptr;
 cudaError_t (*get)(void*, int*) noexcept = nullptr;
 cudaError_t (*set)(void*, int) noexcept = nullptr;
};
inline void record_first_cuda_failure(std::atomic<cudaError_t>& target, const cudaError_t failure) noexcept {
 if (failure == cudaSuccess) return;
 cudaError_t expected = cudaSuccess;
 static_cast<void>(target.compare_exchange_strong(expected, failure, std::memory_order_acq_rel, std::memory_order_acquire));
}
// The production runtime adapter is implemented behind the owner's private
// vendor boundary. Higher-level resource owners may provide an equivalent
// narrow API for tests without exposing their backend to this framework.
[[nodiscard]] CudaDeviceApi cuda_runtime_device_api() noexcept;
struct CudaDeviceOwner final {
 int device = -1;
 void* context = nullptr;
 void (*record_failure)(void*, cudaError_t) noexcept = nullptr;
};
template <class Owner, void (Owner::*RecordFailure)(cudaError_t) noexcept>
[[nodiscard]] CudaDeviceOwner make_cuda_device_owner(Owner* owner, const int device) noexcept {
 return {.device = device, .context = owner, .record_failure = [](void* context, const cudaError_t failure) noexcept {
          (static_cast<Owner*>(context)->*RecordFailure)(failure);
         }};
}
// One allocation-free application command-issue scope. The first successful
// query captures an immutable caller device; switching owners never replaces
// it, including when the first owner already matches the caller.
class CudaDeviceScope final {
public:
 explicit CudaDeviceScope(const int owner_device) noexcept : CudaDeviceScope(CudaDeviceOwner{.device = owner_device}, cuda_runtime_device_api()) {}
 explicit CudaDeviceScope(const CudaDeviceOwner owner) noexcept : CudaDeviceScope(owner, cuda_runtime_device_api()) {}
 CudaDeviceScope(const int owner_device, const CudaDeviceApi api) noexcept : CudaDeviceScope(CudaDeviceOwner{.device = owner_device}, api) {}
 CudaDeviceScope(const CudaDeviceOwner owner, const CudaDeviceApi api) noexcept : api_(api), active_owner_(owner) {
  if (owner.device < 0 || api_.get == nullptr || api_.set == nullptr) return;
  status_ = api_.get(api_.context, &previous_device_);
  if (status_ != cudaSuccess) {
   record_unproved(owner, status_);
   return;
  }
  captured_ = true;
  restoration_required_ = true;
  if (previous_device_ != owner.device) {
   status_ = api_.set(api_.context, owner.device);
   if (status_ != cudaSuccess) {
    record_unproved(owner, status_);
    return;
   }
  }
  status_ = cudaSuccess;
  valid_ = true;
 }
 ~CudaDeviceScope() noexcept {
  if (restoration_required_ && !failed_closed_) std::terminate();
 }
 CudaDeviceScope(const CudaDeviceScope&) = delete;
 CudaDeviceScope& operator=(const CudaDeviceScope&) = delete;
 CudaDeviceScope(CudaDeviceScope&& other) noexcept
     : api_(other.api_),
       active_owner_(other.active_owner_),
       previous_device_(other.previous_device_),
       status_(other.status_),
       captured_(std::exchange(other.captured_, false)),
       restoration_required_(std::exchange(other.restoration_required_, false)),
       failed_closed_(std::exchange(other.failed_closed_, false)),
       valid_(std::exchange(other.valid_, false)) {}
 CudaDeviceScope& operator=(CudaDeviceScope&& other) noexcept {
  if (this == &other) return *this;
  if (restoration_required_ && !failed_closed_) std::terminate();
  api_ = other.api_;
  active_owner_ = other.active_owner_;
  previous_device_ = other.previous_device_;
  status_ = other.status_;
  captured_ = std::exchange(other.captured_, false);
  restoration_required_ = std::exchange(other.restoration_required_, false);
  failed_closed_ = std::exchange(other.failed_closed_, false);
  valid_ = std::exchange(other.valid_, false);
  return *this;
 }
 [[nodiscard]] explicit operator bool() const noexcept { return valid_; }
 [[nodiscard]] cudaError_t status() const noexcept { return status_; }
 [[nodiscard]] bool active() const noexcept { return restoration_required_ && !failed_closed_; }
 [[nodiscard]] bool restored() const noexcept { return captured_ && !restoration_required_ && !failed_closed_; }
 [[nodiscard]] cudaError_t Select(const CudaDeviceOwner owner) noexcept {
  if (!captured_ || !restoration_required_ || failed_closed_ || owner.device < 0) return cudaErrorInvalidDevice;
  active_owner_ = owner;
  const cudaError_t selected = api_.set(api_.context, owner.device);
  if (selected != cudaSuccess) record_unproved(owner, selected);
  return selected;
 }
 void RecordFailure(const cudaError_t status) noexcept {
  if (status != cudaSuccess) record_unproved(active_owner_, status);
 }
 [[nodiscard]] cudaError_t Finalize() noexcept {
  if (!captured_) return status_;
  if (!restoration_required_) return status_;
  const cudaError_t first = api_.set(api_.context, previous_device_);
  if (first == cudaSuccess) {
   restoration_required_ = false;
   valid_ = false;
   return status_;
  }
  const bool first_unproved = cuda_custody_unproved(first);
  bool worker_failed = first_unproved && record_terminal(active_owner_, first);
  const cudaError_t retry = api_.set(api_.context, previous_device_);
  if (retry == cudaSuccess) {
   restoration_required_ = false;
   valid_ = false;
   return status_ == cudaSuccess ? first : status_;
  }
  // Preserve the first failed restoration as the resource terminal. A
  // successful retry is an operation failure only; repeated ordinary
  // failure additionally selects the device-neutral worker terminal.
  const bool lost = cuda_custody_unproved(first) || cuda_custody_unproved(retry);
  if (!first_unproved) worker_failed = record_terminal(active_owner_, cuda_custody_unproved(retry) ? retry : first);
  if (!worker_failed) worker_failed = fail_current_resource_worker();
  // A resource owner callback closes its admission and records the
  // physical terminal even on an owner/shutdown thread where no generic
  // worker failure target is installed.
  failed_closed_ = lost || worker_failed || active_owner_.record_failure != nullptr;
  valid_ = false;
  return status_ == cudaSuccess ? first : status_;
 }
 [[nodiscard]] cudaError_t FinalizeStatus(const cudaError_t operation_status) noexcept {
  if (!active()) return operation_status;
  const cudaError_t restoration = Finalize();
  return operation_status == cudaSuccess ? restoration : operation_status;
 }

private:
 [[nodiscard]] static bool record_terminal(const CudaDeviceOwner owner, const cudaError_t status) noexcept {
  if (status == cudaSuccess) return false;
  if (owner.record_failure != nullptr) owner.record_failure(owner.context, status);
  return cuda_custody_unproved(status) && fail_current_resource_worker();
 }
 static void record_unproved(const CudaDeviceOwner owner, const cudaError_t status) noexcept {
  if (cuda_custody_unproved(status)) static_cast<void>(record_terminal(owner, status));
 }
 CudaDeviceApi api_{};
 CudaDeviceOwner active_owner_{};
 int previous_device_ = 0;
 cudaError_t status_ = cudaErrorUnknown;
 bool captured_ = false;
 bool restoration_required_ = false;
 bool failed_closed_ = false;
 bool valid_ = false;
};
}  // namespace mmltk::frameworks::gpu
