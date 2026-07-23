#include "live/live_helpers.h"
#include "support/catch2_compat.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

struct MockBundle {
    std::uint32_t slot_index = 0;
    std::uint64_t generation = 0;
};

struct MockSlot {
    std::uint32_t slot_index = 0;
    std::atomic<std::uint32_t> state{mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree)};
    std::uint64_t generation = 0;
    int reset_count = 0;

    [[nodiscard]] MockBundle make_view() const noexcept {
        return MockBundle{slot_index, generation};
    }

    void reset_for_reuse() noexcept {
        generation = 0;
        ++reset_count;
    }
};

[[nodiscard]] std::vector<std::unique_ptr<MockSlot>> make_slots(const std::uint32_t count) {
    std::vector<std::unique_ptr<MockSlot>> slots;
    slots.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto slot = std::make_unique<MockSlot>();
        slot->slot_index = index;
        slots.push_back(std::move(slot));
    }
    return slots;
}

void publish(MockSlot& slot, std::atomic<int>& latest_index, const std::uint64_t generation) {
    slot.generation = generation;
    slot.state.store(mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished), std::memory_order_release);
    latest_index.store(static_cast<int>(slot.slot_index), std::memory_order_release);
}

[[nodiscard]] MockSlot* reserve(std::vector<std::unique_ptr<MockSlot>>& slots, std::atomic<int>& latest_index) {
    return mmltk::live::reserve_writable_slot(
        slots, latest_index, [](MockSlot& slot) { slot.reset_for_reuse(); },
        [](MockSlot&) -> cudaEvent_t { return nullptr; }, "mock live slot event query");
}

[[nodiscard]] bool try_acquire_latest(std::vector<std::unique_ptr<MockSlot>>& slots,
                                      const std::atomic<int>& latest_index, MockBundle* out) {
    return mmltk::live::try_acquire_latest_published_slot(slots, latest_index, out,
                                                          [](MockSlot& slot) { return slot.make_view(); });
}

void release(MockSlot& slot) {
    mmltk::live::release_acquired_slot(slot, "mock slot release failed");
}

void test_live_slot_ring_publish_acquire_release_lifecycle() {
    auto slots = make_slots(2U);
    std::atomic<int> latest_index{-1};

    MockSlot* first = reserve(slots, latest_index);
    assert(first == slots[0].get());
    assert(first->state.load(std::memory_order_acquire) ==
           mmltk::live::to_slot_state_value(mmltk::live::SlotState::kWriting));
    publish(*first, latest_index, 11U);

    MockBundle acquired{};
    assert(try_acquire_latest(slots, latest_index, &acquired));
    assert(acquired.slot_index == 0U);
    assert(acquired.generation == 11U);
    assert(slots[0]->state.load(std::memory_order_acquire) ==
           mmltk::live::to_slot_state_value(mmltk::live::SlotState::kAcquired));

    MockSlot* second = reserve(slots, latest_index);
    assert(second == slots[1].get());
    publish(*second, latest_index, 12U);

    release(*slots[0]);
    assert(slots[0]->state.load(std::memory_order_acquire) ==
           mmltk::live::to_slot_state_value(mmltk::live::SlotState::kFree));
}

void test_live_slot_ring_drops_when_all_slots_are_retained() {
    auto slots = make_slots(2U);
    std::atomic<int> latest_index{-1};

    MockSlot* first = reserve(slots, latest_index);
    assert(first == slots[0].get());
    publish(*first, latest_index, 21U);

    MockBundle first_acquired{};
    assert(try_acquire_latest(slots, latest_index, &first_acquired));
    assert(first_acquired.slot_index == 0U);

    MockSlot* second = reserve(slots, latest_index);
    assert(second == slots[1].get());
    publish(*second, latest_index, 22U);

    MockBundle second_acquired{};
    assert(try_acquire_latest(slots, latest_index, &second_acquired));
    assert(second_acquired.slot_index == 1U);

    assert(reserve(slots, latest_index) == nullptr);

    release(*slots[0]);
    release(*slots[1]);
}

void test_live_slot_ring_recycles_stale_published_slots_only() {
    auto slots = make_slots(2U);
    std::atomic<int> latest_index{-1};

    publish(*slots[0], latest_index, 31U);
    publish(*slots[1], latest_index, 32U);

    MockSlot* recycled = reserve(slots, latest_index);
    assert(recycled == slots[0].get());
    assert(recycled->reset_count == 1);
    assert(recycled->generation == 0U);
    assert(latest_index.load(std::memory_order_acquire) == 1);
    assert(slots[1]->state.load(std::memory_order_acquire) ==
           mmltk::live::to_slot_state_value(mmltk::live::SlotState::kPublished));
}

}  

MMLTK_REGISTER_TEST_CASE("[core][live_slot_ring]", test_live_slot_ring_publish_acquire_release_lifecycle);
MMLTK_REGISTER_TEST_CASE("[core][live_slot_ring]", test_live_slot_ring_drops_when_all_slots_are_retained);
MMLTK_REGISTER_TEST_CASE("[core][live_slot_ring]", test_live_slot_ring_recycles_stale_published_slots_only);
