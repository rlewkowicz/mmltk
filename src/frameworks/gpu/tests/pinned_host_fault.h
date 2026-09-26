#pragma once
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include <optional>
namespace mmltk::frameworks::gpu::test_support {
// Deterministic faults at real registered-page retirement boundaries. Zero
// selects every armed unregister; a positive value selects that armed call.
class PinnedHostFault final {
public:
 struct Configuration {
  bool synchronize = false;
  std::optional<unsigned> unregister_call{};
  bool restore = false;
 };
 explicit PinnedHostFault(Configuration configuration) : configuration_(configuration) {}
 PinnedHostFault(const PinnedHostFault&) = delete;
 PinnedHostFault& operator=(const PinnedHostFault&) = delete;
 void Arm(bool enabled = true) noexcept { armed_ = enabled; }
 [[nodiscard]] unsigned unregisters() const noexcept { return unregisters_; }
 [[nodiscard]] unsigned synchronizations() const noexcept { return synchronizations_; }
 [[nodiscard]] PinnedHostBuffer::Operations operations() noexcept {
  return {
   .context = {
    this, [](void*, CUcontext* context) noexcept { return cuCtxGetCurrent(context); },
    [](void* value, CUcontext context) noexcept {
   auto& owner = *static_cast<PinnedHostFault*>(value);
   if (owner.armed_ && owner.configuration_.restore && ++owner.restores_ == 2U) return CUDA_ERROR_CONTEXT_IS_DESTROYED;
   return cuCtxSetCurrent(context);
  }
   },
   .synchronize =
    [](void* value) {
   auto& owner = *static_cast<PinnedHostFault*>(value);
   ++owner.synchronizations_;
   return owner.armed_ && owner.configuration_.synchronize ? CUDA_ERROR_UNKNOWN : cuCtxSynchronize();
  },
   .unregister = [](void* value, void* address) {
   auto& owner = *static_cast<PinnedHostFault*>(value);
   ++owner.unregisters_;
   if (owner.armed_) {
    ++owner.armed_unregisters_;
    const auto call = owner.configuration_.unregister_call;
    if (call && (*call == 0U || *call == owner.armed_unregisters_)) return CUDA_ERROR_UNKNOWN;
   }
   return cuMemHostUnregister(address);
  },
  };
 }

private:
 Configuration configuration_;
 bool armed_ = false;
 unsigned unregisters_ = 0, armed_unregisters_ = 0, synchronizations_ = 0, restores_ = 0;
};
}  // namespace mmltk::frameworks::gpu::test_support
