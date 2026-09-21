#pragma once
#include <chrono>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include "src/controller/contracts/diagnostic_context.h"
namespace mmltk::controller::services {
// Process-local diagnostic identities, never resource or scheduling state.
class DiagnosticSpanIds final {
public:
 [[nodiscard]] static contracts::DiagnosticLink Next(const contracts::DiagnosticLink parent = {}) noexcept {
  auto previous = sequence_.load(std::memory_order_relaxed);
  do {
   if (previous == std::numeric_limits<std::uint64_t>::max()) return {};
  } while (!sequence_.compare_exchange_weak(previous, previous + 1U, std::memory_order_relaxed));
  const auto value = previous + 1U;
  return {.trace_id = parent.trace_id ? parent.trace_id : contracts::DiagnosticTraceId{value},
          .span_id = contracts::DiagnosticSpanId{value},
          .parent_span_id = parent.span_id};
 }
 [[nodiscard]] static std::uint64_t issued() noexcept { return sequence_.load(std::memory_order_relaxed); }

private:
 static inline std::atomic<std::uint64_t> sequence_{0U};
};
// A synchronous effect only. The lazy factory runs after sink admission and
// supplies the existing begin/end vocabulary. No work or GPU lifetime is owned.
template <class Sink, class Fact, class Clock = std::chrono::steady_clock>
class RuntimeDiagnosticSpan final {
 static_assert(noexcept(Clock::now()));
 static_assert(noexcept(std::declval<const Sink&>().valid()));
 static_assert(noexcept(std::declval<const Sink&>()(std::declval<Fact>())));

public:
 template <class Factory>
 RuntimeDiagnosticSpan(Sink sink, Factory&& facts, const contracts::DiagnosticLink parent = {}) noexcept : sink_(std::move(sink)) {
  if (!sink_.valid()) return;
  try {
   auto pair = std::forward<Factory>(facts)();
   pair.first.context.link = DiagnosticSpanIds::Next(parent);
   if (!pair.first.context.link.span_id) return;
   pair.first.context.span = {};
   ended_.emplace(std::move(pair.second));
   ended_->context.link = pair.first.context.link;
   exceptions_ = std::uncaught_exceptions();
   started_ = Clock::now();
   sink_(pair.first);
  } catch (...) { ended_.reset(); }
 }
 RuntimeDiagnosticSpan(const RuntimeDiagnosticSpan&) = delete;
 RuntimeDiagnosticSpan& operator=(const RuntimeDiagnosticSpan&) = delete;
 ~RuntimeDiagnosticSpan() noexcept {
  if (!ended_) return;
  Finish(std::uncaught_exceptions() > exceptions_ ? contracts::DiagnosticSpanOutcome::Exception : contracts::DiagnosticSpanOutcome::ScopeExit);
 }
 void Finish(contracts::DiagnosticSpanOutcome outcome = contracts::DiagnosticSpanOutcome::Success) noexcept {
  if (!ended_) return;
  if (sink_.valid()) {
   ended_->context.span = {
    .duration_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_).count()),
    .span_outcome = outcome,
   };
   sink_(*ended_);
  }
  ended_.reset();
 }
 [[nodiscard]] contracts::DiagnosticLink link() const noexcept { return ended_ ? ended_->context.link : contracts::DiagnosticLink{}; }
 template <class Update>
 void FinishWith(Update&& update) noexcept {
  if (!ended_) return;
  if (!sink_.valid()) {
   ended_.reset();
   return;
  }
  const auto link = ended_->context.link;
  try {
   std::forward<Update>(update)(*ended_);
   ended_->context.link = link;
  } catch (...) {
   ended_->context.link = link;
   Finish(contracts::DiagnosticSpanOutcome::Exception);
   return;
  }
  Finish();
 }

private:
 Sink sink_;
 std::optional<Fact> ended_;
 typename Clock::time_point started_{};
 int exceptions_ = 0;
};
template <class Sink, class Factory>
RuntimeDiagnosticSpan(Sink, Factory) -> RuntimeDiagnosticSpan<Sink, typename std::invoke_result_t<Factory>::second_type>;
template <class Sink, class Factory>
RuntimeDiagnosticSpan(Sink, Factory, contracts::DiagnosticLink) -> RuntimeDiagnosticSpan<Sink, typename std::invoke_result_t<Factory>::second_type>;
}  // namespace mmltk::controller::services
