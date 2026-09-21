#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include <stdexcept>
#include <utility>
namespace mmltk::frameworks::gpu {
TerminalCudaCustody* TerminalCudaRetirementOwner::Slot::uninitialized_custody() noexcept {
 return reinterpret_cast<TerminalCudaCustody*>(custody_storage.data());
}
bool TerminalCudaRetirementOwner::admission_open() const noexcept { return !terminal_.load(std::memory_order_acquire); }
TerminalCudaRetirementOwner::TerminalCudaRetirementOwner(const std::size_t capacity)
    : slots_(capacity == 0U || capacity > kMaximumCapacity ? nullptr : std::make_unique<Slot[]>(capacity)), capacity_(slots_ == nullptr ? 0U : capacity) {
 if (slots_ == nullptr) throw std::invalid_argument("terminal CUDA retirement capacity is outside the fixed bound");
}
TerminalCudaRetirementOwner::~TerminalCudaRetirementOwner() noexcept {
 // Reserved slots contain no object. Installed slots deliberately retain a
 // live shared_ptr in manual storage without running its destructor.
}
std::optional<TerminalCudaRetirementLease> TerminalCudaRetirementOwner::Reserve() noexcept {
 if (!admission_open()) return std::nullopt;
 std::lock_guard lock(mutex_);
 if (terminal_.load(std::memory_order_acquire)) return std::nullopt;
 for (std::size_t index = 0U; index != capacity_; ++index) {
  Slot& slot = slots_[index];
  if (slot.phase != SlotPhase::Free) continue;
  if (++next_generation_ == 0U) ++next_generation_;
  slot.phase = SlotPhase::Reserved;
  slot.generation = next_generation_;
  slot.failure = cudaSuccess;
  ++reservations_;
  return MakeLease(*this, index, slot.generation);
 }
 return std::nullopt;
}
void TerminalCudaRetirementOwner::Release(const std::size_t index, const std::uint64_t generation) noexcept {
 std::lock_guard lock(mutex_);
 if (index >= capacity_) return;
 Slot& slot = slots_[index];
 if (slot.phase != SlotPhase::Reserved || slot.generation != generation) return;
 slot.phase = SlotPhase::Free;
 slot.failure = cudaSuccess;
 --reservations_;
}
void TerminalCudaRetirementOwner::Install(const std::size_t index, const std::uint64_t generation, TerminalCudaCustody&& custody,
                                          const cudaError_t failure) noexcept {
 std::lock_guard lock(mutex_);
 if (index >= capacity_ || !custody) std::terminate();
 Slot& slot = slots_[index];
 // A live lease is the sole authority for this slot/generation. No fallible
 // capacity decision remains at installation time.
 if (slot.phase != SlotPhase::Reserved || slot.generation != generation) std::terminate();
 std::construct_at(slot.uninitialized_custody(), std::move(custody));
 slot.phase = SlotPhase::Installed;
 slot.failure = failure;
 --reservations_;
 ++occupancy_;
 if (first_failure_ == cudaSuccess && failure != cudaSuccess) first_failure_ = failure;
 terminal_.store(true, std::memory_order_release);
}
TerminalCudaRetirementFact TerminalCudaRetirementOwner::fact() const noexcept {
 std::lock_guard lock(mutex_);
 return {.terminal = terminal_.load(std::memory_order_acquire), .occupancy = occupancy_, .reservations = reservations_, .first_failure = first_failure_};
}
}  // namespace mmltk::frameworks::gpu
