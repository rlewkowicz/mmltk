#include <cuda_runtime_api.h>
#include <condition_variable>
#include <mutex>
#include <new>
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
#include "shared_cuda_event.h"
#include "detail/shared_cuda_event_runtime.inc"
namespace mmltk::backend::ml::cuda {
namespace {
using RuntimeLease = CudaEventPoolOwner::Lease;
}  // namespace
struct CudaEventPool::State final {
    State(const mmltk::frameworks::gpu::CudaDeviceOwner device_owner, const std::size_t capacity,
          mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_authority)
        : owner(device_owner, capacity, retirement_authority) {}
    CudaEventPoolOwner owner;
};
CudaEventPool::Lease::~Lease() noexcept {
    static_assert(sizeof(RuntimeLease) <= kStorageBytes);
    static_assert(alignof(RuntimeLease) <= alignof(std::max_align_t));
    if (engaged_) std::destroy_at(std::launder(reinterpret_cast<RuntimeLease*>(storage_.data())));
}
CudaEventPool::Lease::Lease(Lease&& other) noexcept {
    auto* const destination = std::launder(reinterpret_cast<RuntimeLease*>(storage_.data()));
    auto* const source = std::launder(reinterpret_cast<RuntimeLease*>(other.storage_.data()));
    if (other.engaged_) {
        std::construct_at(destination, std::move(*source));
        engaged_ = true;
        std::destroy_at(source);
        other.engaged_ = false;
    }
}
CudaEventPool::Lease& CudaEventPool::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) return *this;
    auto* const destination = std::launder(reinterpret_cast<RuntimeLease*>(storage_.data()));
    auto* const source = std::launder(reinterpret_cast<RuntimeLease*>(other.storage_.data()));
    if (engaged_) std::destroy_at(destination);
    engaged_ = false;
    if (other.engaged_) {
        std::construct_at(destination, std::move(*source));
        engaged_ = true;
        std::destroy_at(source);
        other.engaged_ = false;
    }
    return *this;
}
void CudaEventPool::Lease::wait(const std::uintptr_t stream, const char* context) const {
    if (!engaged_) throw std::logic_error("cannot wait on a retired CUDA event lease");
    const auto* const lease = std::launder(reinterpret_cast<const RuntimeLease*>(storage_.data()));
    lease->wait(reinterpret_cast<cudaStream_t>(stream), context);
}
void CudaEventPool::Lease::retire() {
    if (!engaged_) throw std::logic_error("CUDA event lease was already retired");
    auto* const lease = std::launder(reinterpret_cast<RuntimeLease*>(storage_.data()));
    lease->retire();
    std::destroy_at(lease);
    engaged_ = false;
}
CudaEventPool::Lease::operator bool() const noexcept { return engaged_; }
CudaEventPool::CudaEventPool(const mmltk::frameworks::gpu::CudaDeviceOwner owner, const std::size_t capacity,
                             mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_authority)
    : state_(std::make_unique<State>(owner, capacity, retirement_authority)) {}
CudaEventPool::~CudaEventPool() noexcept = default;
std::optional<CudaEventPool::Lease> CudaEventPool::record(const std::uintptr_t stream, const char* context) {
    auto recorded = state_->owner.record(reinterpret_cast<cudaStream_t>(stream), context);
    if (!recorded) return std::nullopt;
    Lease result;
    auto* const lease = std::launder(reinterpret_cast<RuntimeLease*>(result.storage_.data()));
    std::construct_at(lease, std::move(*recorded));
    result.engaged_ = true;
    return result;
}
std::size_t CudaEventPool::capacity() const noexcept { return state_->owner.capacity(); }
}  // namespace mmltk::backend::ml::cuda
