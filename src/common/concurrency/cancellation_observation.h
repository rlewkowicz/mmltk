#pragma once
#include <atomic>
namespace mmltk::common::concurrency {
// Allocation-free borrowed cancellation input. The referenced source must
// outlive every observation made through this value.
class CancellationObservation final {
public:
 constexpr CancellationObservation() noexcept = default;
 [[nodiscard]] int descriptor() const noexcept { return descriptor_ ? descriptor_(source_) : -1; }
 [[nodiscard]] bool requested() const noexcept { return observe_ != nullptr && observe_(source_); }
 [[nodiscard]] static CancellationObservation Atomic(const std::atomic<bool>& source) noexcept {
  return {&source, [](const void* value) noexcept { return static_cast<const std::atomic<bool>*>(value)->load(std::memory_order_relaxed); }};
 }
 static CancellationObservation Atomic(const std::atomic<bool>&&) = delete;
 template <class Source>
 [[nodiscard]] static CancellationObservation Borrow(const Source& source) noexcept {
  return {&source, [](const void* value) noexcept { return static_cast<const Source*>(value)->cancelled(); }, [](const void* value) noexcept {
   if constexpr (requires { static_cast<const Source*>(value)->descriptor(); }) return static_cast<const Source*>(value)->descriptor();
   else return -1;
  }};
 }
 template <class Source>
 static CancellationObservation Borrow(const Source&&) = delete;

private:
 using Observer = bool (*)(const void*) noexcept;
 using Descriptor = int (*)(const void*) noexcept;
 constexpr CancellationObservation(const void* source, const Observer observe, const Descriptor descriptor = nullptr) noexcept : source_(source), observe_(observe), descriptor_(descriptor) {}
 const void* source_ = nullptr;
 Observer observe_ = nullptr;
 Descriptor descriptor_ = nullptr;
};
static_assert(sizeof(CancellationObservation) == sizeof(void*) * 3U);
}  // namespace mmltk::common::concurrency
