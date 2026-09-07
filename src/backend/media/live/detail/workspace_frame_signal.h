#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
namespace mmltk::backend::media::live {

// One coherent coalesced level owned by the Live physical data plane.
// Observers may skip revisions; slot custody remains exclusively with the
// compositor's move-only output lease.
class LiveCompletedFramePublication final {
   public:
    void publish(PhysicalFrameRevision revision) noexcept;
    [[nodiscard]] PhysicalFrameRevision snapshot() const noexcept;
    void clear() noexcept;

   private:
    void store(PhysicalFrameRevision revision) noexcept;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    // CLEANUP-IGNORE: This production signal owns atomic frame identity fields unrelated to test-backend counters.
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);

    // CLEANUP-IGNORE: Frame publication identity is production signal state, not fake-backend transfer telemetry.
    std::atomic<std::uint64_t> sequence_{0U};
    std::atomic<std::uint64_t> revision_{0U};
    std::atomic<std::uint64_t> frame_session_{0U};
    std::atomic<std::uint64_t> frame_sequence_{0U};
    std::atomic<std::uint32_t> slot_{0U};
    std::atomic<std::uintptr_t> ready_{0U};
};
}  // namespace mmltk::backend::media::live
