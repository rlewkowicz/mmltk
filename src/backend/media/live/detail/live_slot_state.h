#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
namespace mmltk::backend::media::live {
enum class SlotState : std::uint8_t {
    Free,
    Uploading,
    Published,
    Acquired,
    Completing,
    Terminal,
};
[[nodiscard]] constexpr std::uint32_t slot_state_value(const SlotState state) noexcept { return static_cast<std::uint32_t>(state); }
[[nodiscard]] inline bool slot_state_is(const std::atomic<std::uint32_t>& state, const SlotState expected) noexcept {
    return state.load(std::memory_order_acquire) == slot_state_value(expected);
}
inline void publish_live_slot_state(std::atomic<std::uint32_t>& state, const SlotState value) noexcept {
    state.store(slot_state_value(value), std::memory_order_release);
}
[[nodiscard]] inline bool transition_slot_state(std::atomic<std::uint32_t>& state, const SlotState expected, const SlotState desired) noexcept {
    auto encoded_expected = slot_state_value(expected);
    return state.compare_exchange_strong(encoded_expected, slot_state_value(desired), std::memory_order_acq_rel);
}
[[nodiscard]] inline bool claim_live_slot(std::atomic<std::uint32_t>& state, const SlotState expected) noexcept {
    return transition_slot_state(state, expected, SlotState::Completing);
}
inline void clear_latest_live_slot(std::atomic<int>& latest, const std::uint32_t slot) noexcept {
    int expected = static_cast<int>(slot);
    static_cast<void>(latest.compare_exchange_strong(expected, -1, std::memory_order_acq_rel, std::memory_order_acquire));
}
template <class Slot>
struct ReusableLiveSlot final {
    Slot* slot = nullptr;
    bool replaced = false;
};
template <class Slot>
[[nodiscard]] ReusableLiveSlot<Slot> reserve_live_slot(Slot* const slots, const std::uint32_t count, const std::atomic<int>* const latest = nullptr) noexcept {
    for (std::uint32_t index = 0U; index < count; ++index) {
        if (transition_slot_state(slots[index].state, SlotState::Free, SlotState::Uploading)) return {.slot = &slots[index]};
    }
    const int candidate = latest == nullptr ? -1 : latest->load(std::memory_order_acquire);
    if (candidate >= 0 && candidate < static_cast<int>(count) && claim_live_slot(slots[candidate].state, SlotState::Published)) {
        publish_live_slot_state(slots[candidate].state, SlotState::Uploading);
        return {.slot = &slots[candidate], .replaced = true};
    }
    for (std::uint32_t index = 0U; index < count; ++index) {
        if (static_cast<int>(index) == candidate) continue;
        if (claim_live_slot(slots[index].state, SlotState::Published)) {
            publish_live_slot_state(slots[index].state, SlotState::Uploading);
            return {.slot = &slots[index], .replaced = true};
        }
    }
    return {};
}
template <class Scrub>
void publish_live_owner_slot(std::atomic<std::uint32_t>& state, const SlotState published, Scrub&& scrub) noexcept {
    std::forward<Scrub>(scrub)();
    publish_live_slot_state(state, published);
}
template <class Scrub>
void publish_latest_live_owner_slot(std::atomic<int>& latest, const std::uint32_t index, std::atomic<std::uint32_t>& state, const SlotState published,
                                    Scrub&& scrub) noexcept {
    clear_latest_live_slot(latest, index);
    publish_live_owner_slot(state, published, std::forward<Scrub>(scrub));
}
}  // namespace mmltk::backend::media::live
