#pragma once

#include <atomic>

namespace mmltk::common::concurrency {

// Allocation-free borrowed cancellation input. The referenced source must
// outlive every observation made through this value.
class CancellationObservation final {
   public:
    constexpr CancellationObservation() noexcept = default;

    [[nodiscard]] bool requested() const noexcept { return observe_ != nullptr && observe_(source_); }

    [[nodiscard]] static CancellationObservation Atomic(const std::atomic<bool>& source) noexcept {
        return {&source,
                [](const void* value) noexcept { return static_cast<const std::atomic<bool>*>(value)->load(std::memory_order_relaxed); }};
    }
    static CancellationObservation Atomic(const std::atomic<bool>&&) = delete;

    template <class Source>
    [[nodiscard]] static CancellationObservation Borrow(const Source& source) noexcept {
        return {&source, [](const void* value) noexcept { return static_cast<const Source*>(value)->cancelled(); }};
    }
    template <class Source>
    static CancellationObservation Borrow(const Source&&) = delete;

   private:
    using Observer = bool (*)(const void*) noexcept;

    constexpr CancellationObservation(const void* source, const Observer observe) noexcept : source_(source), observe_(observe) {}

    const void* source_ = nullptr;
    Observer observe_ = nullptr;
};

static_assert(sizeof(CancellationObservation) == sizeof(void*) * 2U);

}  // namespace mmltk::common::concurrency
