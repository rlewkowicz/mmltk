#pragma once

#include "mmltk/live/live_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mmltk::live {

struct WorkspaceSourceLeaseRingStatus {
    std::uint64_t acquire_waits = 0;
    std::uint64_t leases_acquired = 0;
    std::uint64_t leases_released = 0;
    std::uint64_t stale_releases = 0;
    std::uint64_t skipped_stale_frames = 0;
    std::uint64_t slot_pressure = 0;
    std::uint64_t release_latency_ns = 0;
};

class WorkspaceSourceLeaseRing {
   public:
    explicit WorkspaceSourceLeaseRing(std::chrono::milliseconds acquire_cancel_poll = std::chrono::milliseconds(10))
        : acquire_cancel_poll_(acquire_cancel_poll) {}

    void initialize(const std::size_t slot_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        leases_.clear();
        leases_.resize(slot_count);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        leases_.clear();
        condition_.notify_all();
    }

    void reset_counters() noexcept {
        acquire_waits_.store(0U, std::memory_order_release);
        leases_acquired_.store(0U, std::memory_order_release);
        leases_released_.store(0U, std::memory_order_release);
        stale_releases_.store(0U, std::memory_order_release);
        skipped_stale_frames_.store(0U, std::memory_order_release);
        slot_pressure_.store(0U, std::memory_order_release);
        release_latency_ns_.store(0U, std::memory_order_release);
    }

    void reset_inactive_leases_for_restart() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (LeaseState& lease : leases_) {
            if (!lease.active) {
                lease = {};
            }
        }
        condition_.notify_all();
    }

    void notify_all() noexcept {
        condition_.notify_all();
    }

    [[nodiscard]] bool lease_active(const std::uint32_t slot_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slot_index < leases_.size() && leases_[slot_index].active;
    }

    void note_slot_pressure() noexcept {
        slot_pressure_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] WorkspaceSourceLeaseRingStatus snapshot_status() const noexcept {
        return WorkspaceSourceLeaseRingStatus{
            acquire_waits_.load(std::memory_order_acquire),        leases_acquired_.load(std::memory_order_acquire),
            leases_released_.load(std::memory_order_acquire),      stale_releases_.load(std::memory_order_acquire),
            skipped_stale_frames_.load(std::memory_order_acquire), slot_pressure_.load(std::memory_order_acquire),
            release_latency_ns_.load(std::memory_order_acquire),
        };
    }

    template <typename SlotRange, typename RevisionAt, typename RunningFn, typename ReadyFn, typename MakeLeaseFn>
    [[nodiscard]] bool request_next(SlotRange& slots, const std::uint64_t swapchain_generation,
                                    const std::uint64_t after_revision, const std::atomic<bool>& caller_stop_requested,
                                    const std::atomic<bool>& owner_stop_requested, RunningFn&& running,
                                    RevisionAt&& revision_at, ReadyFn&& ready, MakeLeaseFn&& make_lease,
                                    WorkspaceSourceFrameLease* out) {
        if (out == nullptr) {
            throw std::runtime_error("workspace source frame lease acquire requires an output lease");
        }
        *out = {};
        acquire_waits_.fetch_add(1U, std::memory_order_relaxed);

        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            condition_.wait_for(lock, acquire_cancel_poll_, [&] {
                if (caller_stop_requested.load(std::memory_order_acquire) ||
                    owner_stop_requested.load(std::memory_order_acquire) || !running()) {
                    return true;
                }
                std::uint64_t ignored_revision = 0;
                return newest_candidate_locked(slots, after_revision, revision_at, ready, &ignored_revision) != nullptr;
            });

            if (caller_stop_requested.load(std::memory_order_acquire) ||
                owner_stop_requested.load(std::memory_order_acquire) || !running()) {
                return false;
            }

            std::uint64_t revision = 0;
            auto* slot = newest_candidate_locked(slots, after_revision, revision_at, ready, &revision);
            if (slot == nullptr) {
                continue;
            }

            std::uint32_t expected = to_slot_state_value(SlotState::kPublished);
            if (!slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                                     std::memory_order_acq_rel)) {
                continue;
            }

            const std::uint32_t slot_index = slot->slot_index;
            if (slot_index >= leases_.size()) {
                slot->state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
                return false;
            }

            LeaseState& lease = leases_[slot_index];
            lease.active = true;
            lease.generation = swapchain_generation;
            lease.revision = revision;
            lease.acquired_ns = steady_clock_now_ns();
            *out = make_lease(*slot, swapchain_generation, revision);
            leases_acquired_.fetch_add(1U, std::memory_order_relaxed);
            if (revision > after_revision + 1U) {
                skipped_stale_frames_.fetch_add(revision - after_revision - 1U, std::memory_order_relaxed);
            }
            return true;
        }
    }

    template <typename SlotRange>
    void release(SlotRange& slots, const WorkspaceSourceFrameLease& lease) noexcept {
        if (!lease.valid) {
            return;
        }

        bool released = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lease.slot_index < leases_.size()) {
                LeaseState& current = leases_[lease.slot_index];
                if (current.active && current.generation == lease.swapchain_generation &&
                    current.revision == lease.revision) {
                    current.active = false;
                    current.generation = 0U;
                    current.revision = 0U;
                    const std::uint64_t now_ns = steady_clock_now_ns();
                    if (current.acquired_ns != 0U && now_ns >= current.acquired_ns) {
                        release_latency_ns_.fetch_add(now_ns - current.acquired_ns, std::memory_order_relaxed);
                    }
                    current.acquired_ns = 0U;

                    if (lease.slot_index < slots.size()) {
                        std::uint32_t expected = to_slot_state_value(SlotState::kAcquired);
                        released = slots[lease.slot_index]->state.compare_exchange_strong(
                            expected, to_slot_state_value(SlotState::kFree), std::memory_order_acq_rel);
                    }
                }
            }
            if (released) {
                leases_released_.fetch_add(1U, std::memory_order_relaxed);
            } else {
                stale_releases_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        condition_.notify_all();
    }

   private:
    struct LeaseState {
        bool active = false;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        std::uint64_t acquired_ns = 0;
    };

    [[nodiscard]] static std::uint64_t steady_clock_now_ns() noexcept {
        using Clock = std::chrono::steady_clock;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    }

    template <typename SlotRange, typename RevisionAt, typename ReadyFn>
    [[nodiscard]] static auto* newest_candidate_locked(SlotRange& slots, const std::uint64_t after_revision,
                                                       RevisionAt&& revision_at, ReadyFn&& ready,
                                                       std::uint64_t* out_revision) {
        using Slot = std::remove_reference_t<decltype(*std::declval<SlotRange&>().front().get())>;
        Slot* best_slot = nullptr;
        std::uint64_t best_revision = after_revision;
        for (const auto& slot_entry : slots) {
            Slot* slot = slot_entry.get();
            if (slot == nullptr) {
                continue;
            }
            const std::uint64_t revision = revision_at(slot->slot_index);
            if (revision <= best_revision) {
                continue;
            }
            if (slot->state.load(std::memory_order_acquire) != to_slot_state_value(SlotState::kPublished)) {
                continue;
            }
            if (!ready(*slot)) {
                continue;
            }
            best_slot = slot;
            best_revision = revision;
        }
        if (out_revision != nullptr) {
            *out_revision = best_revision;
        }
        return best_slot;
    }

    std::chrono::milliseconds acquire_cancel_poll_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<LeaseState> leases_;
    std::atomic<std::uint64_t> acquire_waits_{0};
    std::atomic<std::uint64_t> leases_acquired_{0};
    std::atomic<std::uint64_t> leases_released_{0};
    std::atomic<std::uint64_t> stale_releases_{0};
    std::atomic<std::uint64_t> skipped_stale_frames_{0};
    std::atomic<std::uint64_t> slot_pressure_{0};
    std::atomic<std::uint64_t> release_latency_ns_{0};
};

}  
