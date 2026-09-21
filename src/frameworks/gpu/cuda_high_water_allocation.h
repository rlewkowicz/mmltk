#pragma once
#include <cuda_runtime_api.h>
#include <cstdint>
#include <initializer_list>
#include <utility>
namespace mmltk::frameworks::gpu {
enum class CudaFailureDisposition : std::uint8_t {
 Released,
 Retryable,
 Unproved,
};
struct CudaReleaseReceipt final {
 CudaFailureDisposition disposition = CudaFailureDisposition::Released;
 cudaError_t failure = cudaSuccess;
 [[nodiscard]] bool released() const noexcept { return disposition == CudaFailureDisposition::Released; }
};
[[nodiscard]] constexpr CudaFailureDisposition cuda_failure_disposition(const cudaError_t status) noexcept {
 if (status == cudaSuccess) return CudaFailureDisposition::Released;
 if (status == cudaErrorContextIsDestroyed || status == cudaErrorCudartUnloading) return CudaFailureDisposition::Unproved;
 return CudaFailureDisposition::Retryable;
}
[[nodiscard]] constexpr CudaReleaseReceipt cuda_release_receipt(const cudaError_t status) noexcept {
 return {.disposition = cuda_failure_disposition(status), .failure = status};
}
[[nodiscard]] constexpr bool cuda_custody_unproved(const cudaError_t status) noexcept {
 return cuda_failure_disposition(status) == CudaFailureDisposition::Unproved;
}
// Sole identity owner for one reusable high-water allocation. A replacement
// is adopted only after the incumbent release succeeds. A retryable incumbent
// release retains that incumbent and transfers the candidate to the one
// pending slot. Terminal teardown releases pending, candidate, and active
// identities in that order, exactly once each.
template <class Handle>
class CudaHighWaterAllocation final {
public:
 [[nodiscard]] Handle active() const noexcept { return active_; }
 [[nodiscard]] Handle candidate() const noexcept { return candidate_; }
 [[nodiscard]] Handle pending_release() const noexcept { return pending_; }
 [[nodiscard]] bool empty() const noexcept { return active_ == Handle{} && candidate_ == Handle{} && pending_ == Handle{}; }
 [[nodiscard]] bool replacement_available() const noexcept { return candidate_ == Handle{} && pending_ == Handle{}; }
 template <class Allocate>
 [[nodiscard]] CudaReleaseReceipt AllocateCandidate(Allocate&& allocate) noexcept {
  if (!replacement_available()) return {.disposition = CudaFailureDisposition::Retryable, .failure = cudaErrorNotReady};
  Handle replacement{};
  const cudaError_t status = allocate(replacement);
  if (status != cudaSuccess) return cuda_release_receipt(status);
  if (replacement == Handle{}) return {.disposition = CudaFailureDisposition::Retryable, .failure = cudaErrorInvalidValue};
  candidate_ = replacement;
  return {};
 }
 template <class Release>
 [[nodiscard]] CudaReleaseReceipt PromoteCandidate(Release&& release) noexcept {
  if (candidate_ == Handle{}) return {};
  if (active_ == Handle{}) {
   active_ = std::exchange(candidate_, Handle{});
   return {};
  }
  const CudaReleaseReceipt receipt = cuda_release_receipt(release(active_));
  if (receipt.released()) {
   active_ = std::exchange(candidate_, Handle{});
  } else if (receipt.disposition == CudaFailureDisposition::Retryable && pending_ == Handle{}) {
   pending_ = std::exchange(candidate_, Handle{});
  }
  return receipt;
 }
 template <class Release>
 [[nodiscard]] CudaReleaseReceipt RetryPending(Release&& release) noexcept {
  if (pending_ == Handle{}) return {};
  const CudaReleaseReceipt receipt = cuda_release_receipt(release(pending_));
  if (receipt.released()) pending_ = Handle{};
  return receipt;
 }
 template <class Release>
 [[nodiscard]] CudaReleaseReceipt ReleaseAll(Release&& release) noexcept {
  for (Handle* handle : {&pending_, &candidate_, &active_}) {
   if (*handle == Handle{}) continue;
   const CudaReleaseReceipt receipt = cuda_release_receipt(release(*handle));
   if (!receipt.released()) return receipt;
   *handle = Handle{};
  }
  return {};
 }

private:
 Handle active_{};
 Handle candidate_{};
 Handle pending_{};
};
}  // namespace mmltk::frameworks::gpu
