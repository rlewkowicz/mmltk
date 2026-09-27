#pragma once
#include <cuda.h>
#include <cstddef>
#include <memory>
#include "src/frameworks/gpu/cuda/cuda_context_scope.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include "src/common/system/numa_topology.h"
namespace mmltk::frameworks::gpu {
// Callback state must outlive the buffer and every escaped tensor view.
struct PinnedHostBufferOperations final {
 CudaContextApi context{};
 CUresult (*synchronize)(void*) = [](void*) { return cuCtxSynchronize(); };
 CUresult (*unregister)(void*, void*) = [](void*, void* address) { return cuMemHostUnregister(address); };
};
// Owner calls ReleaseSettled only after every asynchronous borrower completes.
// Growth and destruction settle the owning context; failed resources enter the
// existing physical retirement owner with their registered pages intact.
class PinnedHostBuffer final {
public:
 using Operations = PinnedHostBufferOperations;
 using Register = CUresult (*)(void*, std::size_t, unsigned);
 PinnedHostBuffer(CUcontext, const mmltk::common::system::ExecutionPlacement&, bool portable = false, Register registration = &cuMemHostRegister,
  std::shared_ptr<TerminalCudaRetirementOwner> retirement = {}, Operations operations = {});
 [[nodiscard]] static std::unique_ptr<PinnedHostBuffer> ForCurrentDevice(bool portable = true, std::shared_ptr<TerminalCudaRetirementOwner> retirement = {}, Operations operations = {});
 ~PinnedHostBuffer() noexcept;
 PinnedHostBuffer(const PinnedHostBuffer&) = delete;
 PinnedHostBuffer& operator=(const PinnedHostBuffer&) = delete;
 void ensure_bytes(std::size_t);
 [[nodiscard]] CUresult ReleaseSettled() noexcept;
 [[nodiscard]] void* data() const noexcept;
 [[nodiscard]] std::size_t capacity_bytes() const noexcept;
 [[nodiscard]] int node() const noexcept;

private:
 void Retain() noexcept;
 [[nodiscard]] CUresult Release(bool synchronize) noexcept;
 Operations operations_;
 void log(const char*, std::size_t active_bytes = 0) const noexcept;
 Register registration_;
 mmltk::common::system::ExecutionPlacement placement_;
 struct State;
 struct Retention;
 std::unique_ptr<Retention> retention_;
 std::shared_ptr<State> state_;
};
}  // namespace mmltk::frameworks::gpu
