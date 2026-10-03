#pragma once
#include <chrono>
#include <cstdint>
#include <time.h>
namespace mmltk::backend::data::detail {
// Optional clocks are inert when disabled. No timestamp participates in product
// scheduling, resource eligibility, or lifetime.
class CompileObservationClock final {
 using Clock = std::chrono::steady_clock;
 bool enabled_;
 bool thread_cpu_;
 std::uint64_t started_ = 0;
 mutable bool valid_ = false;
 mutable bool incomplete_ = false;
 [[nodiscard]] std::uint64_t now() const noexcept {
  if (!thread_cpu_) return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
  timespec time{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
   valid_ = false;
   incomplete_ = true;
   return 0;
  }
  return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(time.tv_nsec);
 }

public:
 explicit CompileObservationClock(bool enabled, bool thread_cpu = false) noexcept : enabled_(enabled), thread_cpu_(thread_cpu) {
  if (enabled_) {
   valid_ = true;
   started_ = now();
  }
 }
 [[nodiscard]] bool incomplete() const noexcept { return incomplete_; }
 [[nodiscard]] std::uint64_t elapsed() const noexcept {
  if (!enabled_ || !valid_) return 0;
  const auto value = now();
  return valid_ && value >= started_ ? value - started_ : 0;
 }
 [[nodiscard]] std::uint64_t lap() noexcept {
  if (!enabled_ || !valid_) return 0;
  const auto value = now();
  const auto elapsed = valid_ && value >= started_ ? value - started_ : 0;
  started_ = value;
  return elapsed;
 }
};
}  // namespace mmltk::backend::data::detail
