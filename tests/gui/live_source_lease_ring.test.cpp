#include "live/workspace_source_lease_ring.h"
#include "support/catch2_compat.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace mmltk::live;

struct MockSourceSlot {
    std::uint32_t slot_index = 0;
    std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
    std::uint64_t revision = 0;
    LiveFrameId frame_id{};

    void reset_for_reuse() noexcept {
        revision = 0;
        frame_id = {};
    }
};

[[nodiscard]] std::vector<std::unique_ptr<MockSourceSlot>> make_source_slots(const std::uint32_t count) {
    std::vector<std::unique_ptr<MockSourceSlot>> slots;
    slots.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto slot = std::make_unique<MockSourceSlot>();
        slot->slot_index = index;
        slots.push_back(std::move(slot));
    }
    return slots;
}

void publish_source_slot(WorkspaceSourceLeaseRing& ring, MockSourceSlot& slot, const std::uint64_t revision) {
    slot.revision = revision;
    slot.frame_id = LiveFrameId{7U, revision};
    slot.state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    ring.notify_all();
}

[[nodiscard]] WorkspaceSourceFrameLease make_mock_source_lease(const MockSourceSlot& slot,
                                                               const std::uint64_t generation,
                                                               const std::uint64_t revision) {
    const WorkspaceFrameMetadata metadata{
        revision,
        generation,
        slot.frame_id,
        WorkspaceDimensions{8U, 6U, 32U},
        WorkspaceDmaBufImage{},
        LiveCaptureRegion{0U, 0U, 8U, 6U},
        LiveCaptureRegion{0U, 0U, 8U, 6U},
        revision * 10U,
        revision * 10U + 1U,
        false,
    };
    return make_workspace_source_frame_lease(true, slot.slot_index, metadata);
}

[[nodiscard]] bool request_next_mock_source(WorkspaceSourceLeaseRing& ring,
                                            std::vector<std::unique_ptr<MockSourceSlot>>& slots,
                                            const std::uint64_t generation, const std::uint64_t after_revision,
                                            const std::atomic<bool>& stop_requested,
                                            const std::atomic<bool>& owner_stop_requested,
                                            const std::atomic<bool>& running, WorkspaceSourceFrameLease* out) {
    return ring.request_next(
        slots, generation, after_revision, stop_requested, owner_stop_requested,
        [&running]() noexcept { return running.load(std::memory_order_acquire); },
        [&slots](const std::uint32_t slot_index) noexcept {
            return slot_index < slots.size() ? slots[slot_index]->revision : 0U;
        },
        [](const MockSourceSlot&) noexcept { return true; },
        [](const MockSourceSlot& slot, const std::uint64_t lease_generation, const std::uint64_t revision) {
            return make_mock_source_lease(slot, lease_generation, revision);
        },
        out);
}

[[nodiscard]] MockSourceSlot* reserve_mock_source_slot(std::vector<std::unique_ptr<MockSourceSlot>>& slots,
                                                       WorkspaceSourceLeaseRing& ring) {
    bool source_pressure = false;
    for (auto& slot : slots) {
        std::uint32_t expected = to_slot_state_value(SlotState::kFree);
        if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                std::memory_order_acq_rel)) {
            return slot.get();
        }
        if (expected == to_slot_state_value(SlotState::kPublished)) {
            expected = to_slot_state_value(SlotState::kPublished);
            if (slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                    std::memory_order_acq_rel)) {
                slot->reset_for_reuse();
                return slot.get();
            }
            continue;
        }
        if (expected == to_slot_state_value(SlotState::kAcquired)) {
            source_pressure = source_pressure || ring.lease_active(slot->slot_index);
        }
    }
    if (source_pressure) {
        ring.note_slot_pressure();
    }
    return nullptr;
}

void wait_until_acquire_waits(const WorkspaceSourceLeaseRing& ring, const std::uint64_t waits) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ring.snapshot_status().acquire_waits >= waits) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false);
}

void test_source_lease_acquire_waits_until_newer_frame_and_releases_before_next_request() {
    WorkspaceSourceLeaseRing ring(std::chrono::milliseconds(1));
    auto slots = make_source_slots(2U);
    ring.initialize(slots.size());
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_stop_requested{false};
    std::atomic<bool> running{true};
    std::atomic<bool> acquired_first{false};
    WorkspaceSourceFrameLease first{};

    std::thread first_request([&] {
        acquired_first.store(
            request_next_mock_source(ring, slots, 5U, 10U, stop_requested, owner_stop_requested, running, &first),
            std::memory_order_release);
    });
    wait_until_acquire_waits(ring, 1U);
    assert(!acquired_first.load(std::memory_order_acquire));

    publish_source_slot(ring, *slots[0], 11U);
    first_request.join();
    assert(acquired_first.load(std::memory_order_acquire));
    assert(first.matches(5U, 0U, 11U));
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));

    std::atomic<bool> acquired_second{false};
    WorkspaceSourceFrameLease second{};
    std::thread second_request([&] {
        acquired_second.store(request_next_mock_source(ring, slots, 5U, first.revision, stop_requested,
                                                       owner_stop_requested, running, &second),
                              std::memory_order_release);
    });
    wait_until_acquire_waits(ring, 2U);
    assert(!acquired_second.load(std::memory_order_acquire));

    ring.release(slots, first);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kFree));
    publish_source_slot(ring, *slots[0], 12U);
    second_request.join();
    assert(acquired_second.load(std::memory_order_acquire));
    assert(second.matches(5U, 0U, 12U));
    ring.release(slots, second);

    const WorkspaceSourceLeaseRingStatus status = ring.snapshot_status();
    assert(status.acquire_waits == 2U);
    assert(status.leases_acquired == 2U);
    assert(status.leases_released == 2U);
    assert(status.stale_releases == 0U);
    assert(status.release_latency_ns > 0U);
}

void test_source_lease_skips_stale_frames_and_blocks_source_owned_slot_reuse() {
    WorkspaceSourceLeaseRing ring(std::chrono::milliseconds(1));
    auto slots = make_source_slots(1U);
    ring.initialize(slots.size());
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_stop_requested{false};
    std::atomic<bool> running{true};

    publish_source_slot(ring, *slots[0], 23U);
    WorkspaceSourceFrameLease lease{};
    assert(request_next_mock_source(ring, slots, 9U, 20U, stop_requested, owner_stop_requested, running, &lease));
    assert(lease.matches(9U, 0U, 23U));
    assert(ring.lease_active(0U));

    assert(reserve_mock_source_slot(slots, ring) == nullptr);
    WorkspaceSourceLeaseRingStatus status = ring.snapshot_status();
    assert(status.skipped_stale_frames == 2U);
    assert(status.slot_pressure == 1U);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));

    ring.release(slots, lease);
    MockSourceSlot* reusable = reserve_mock_source_slot(slots, ring);
    assert(reusable == slots[0].get());
    assert(reusable->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kWriting));
}

void test_source_lease_stale_releases_are_counted_without_crashing_teardown() {
    WorkspaceSourceLeaseRing ring(std::chrono::milliseconds(1));
    auto slots = make_source_slots(1U);
    ring.initialize(slots.size());
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_stop_requested{false};
    std::atomic<bool> running{true};

    publish_source_slot(ring, *slots[0], 31U);
    WorkspaceSourceFrameLease lease{};
    assert(request_next_mock_source(ring, slots, 3U, 30U, stop_requested, owner_stop_requested, running, &lease));

    WorkspaceSourceFrameLease stale = lease;
    stale.revision += 1U;
    ring.release(slots, stale);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));
    assert(ring.snapshot_status().stale_releases == 1U);

    ring.release(slots, lease);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kFree));
    assert(ring.snapshot_status().leases_released == 1U);

    publish_source_slot(ring, *slots[0], 32U);
    WorkspaceSourceFrameLease teardown_lease{};
    assert(
        request_next_mock_source(ring, slots, 3U, 31U, stop_requested, owner_stop_requested, running, &teardown_lease));
    ring.clear();
    std::vector<std::unique_ptr<MockSourceSlot>> no_slots;
    ring.release(no_slots, teardown_lease);
    assert(ring.snapshot_status().stale_releases == 2U);
}

void test_source_lease_blocked_acquire_cancels_without_frame() {
    WorkspaceSourceLeaseRing ring(std::chrono::milliseconds(1));
    auto slots = make_source_slots(1U);
    ring.initialize(slots.size());
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_stop_requested{false};
    std::atomic<bool> running{true};
    std::atomic<bool> acquired{true};
    WorkspaceSourceFrameLease lease{};

    std::thread request([&] {
        acquired.store(
            request_next_mock_source(ring, slots, 6U, 40U, stop_requested, owner_stop_requested, running, &lease),
            std::memory_order_release);
    });
    wait_until_acquire_waits(ring, 1U);
    stop_requested.store(true, std::memory_order_release);
    ring.notify_all();
    request.join();

    assert(!acquired.load(std::memory_order_acquire));
    assert(!lease.valid);
    const WorkspaceSourceLeaseRingStatus status = ring.snapshot_status();
    assert(status.acquire_waits == 1U);
    assert(status.leases_acquired == 0U);
    assert(status.leases_released == 0U);
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][live_source_lease_ring]",
                         test_source_lease_acquire_waits_until_newer_frame_and_releases_before_next_request);
MMLTK_REGISTER_TEST_CASE("[gui][live_source_lease_ring]",
                         test_source_lease_skips_stale_frames_and_blocks_source_owned_slot_reuse);
MMLTK_REGISTER_TEST_CASE("[gui][live_source_lease_ring]",
                         test_source_lease_stale_releases_are_counted_without_crashing_teardown);
MMLTK_REGISTER_TEST_CASE("[gui][live_source_lease_ring]", test_source_lease_blocked_acquire_cancels_without_frame);
