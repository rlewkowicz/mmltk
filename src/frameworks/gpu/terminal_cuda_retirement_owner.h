#pragma once
#include <cuda_runtime_api.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include "src/frameworks/gpu/terminal_cuda_retirement_authority.h"
namespace mmltk::frameworks::gpu {
// Fixed terminal quarantine. Installed CUDA custody is held in
// manual storage so destroying a terminal shell never runs unsafe CUDA
// destruction on its destructor thread. Ordinary retryable cleanup stays with
// its resource owner and never enters this object. The application supplies
// the exact capacity derived from its own topology.
class TerminalCudaRetirementOwner final : public TerminalCudaRetirementAuthority {
public:
 explicit TerminalCudaRetirementOwner(std::size_t capacity);
 TerminalCudaRetirementOwner(const TerminalCudaRetirementOwner&) = delete;
 TerminalCudaRetirementOwner& operator=(const TerminalCudaRetirementOwner&) = delete;
 ~TerminalCudaRetirementOwner() noexcept;
 [[nodiscard]] bool admission_open() const noexcept override;
 [[nodiscard]] std::optional<TerminalCudaRetirementLease> Reserve() noexcept override;
 [[nodiscard]] TerminalCudaRetirementFact fact() const noexcept override;

private:
 static constexpr std::size_t kMaximumCapacity = 64U;
 enum class SlotPhase : std::uint8_t { Free, Reserved, Installed };
 struct Slot final {
  alignas(TerminalCudaCustody) std::array<std::byte, sizeof(TerminalCudaCustody)> custody_storage{};
  SlotPhase phase = SlotPhase::Free;
  std::uint64_t generation = 0U;
  cudaError_t failure = cudaSuccess;
  [[nodiscard]] TerminalCudaCustody* uninitialized_custody() noexcept;
 };
 void Release(std::size_t, std::uint64_t) noexcept override;
 void Install(std::size_t, std::uint64_t, TerminalCudaCustody&&, cudaError_t) noexcept override;
 mutable std::mutex mutex_{};
 std::unique_ptr<Slot[]> slots_{};
 std::size_t capacity_ = 0U;
 std::atomic<bool> terminal_{false};
 std::size_t reservations_ = 0U;
 std::size_t occupancy_ = 0U;
 std::uint64_t next_generation_ = 1U;
 cudaError_t first_failure_ = cudaSuccess;
};
}  // namespace mmltk::frameworks::gpu
