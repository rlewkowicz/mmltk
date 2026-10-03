#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/compile_observation.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>
namespace mmltk::backend::data::detail {
struct CompileAdmissionObservation;
struct CompileExecutionSnapshot;
}  // namespace mmltk::backend::data::detail
namespace mmltk::backend::data::benchmark_internal {
// A null value reports diagnostic construction failure; it is never product state.
using BenchmarkTraceSink = std::function<void(std::string_view, const nlohmann::json&)>;
void trace_compile_execution(const BenchmarkTraceSink&, std::string_view, const detail::CompileExecutionSnapshot&) noexcept;
template <class Observation>
[[nodiscard]] nlohmann::json benchmark_observation_fields(const Observation& observation) {
 nlohmann::json result = nlohmann::json::object();
 mmltk::frameworks::reflection::visit_materialized_members<Observation>([&]<class Declaration>(const auto& field) { result[field.member_name] = observation.*Declaration::pointer; });
 return result;
}
[[nodiscard]] nlohmann::json benchmark_observation_fields(const detail::CompileAdmissionObservation&);
template <class Builder>
 requires std::invocable<Builder> && std::convertible_to<std::invoke_result_t<Builder>, nlohmann::json>
inline void trace_benchmark_event(const BenchmarkTraceSink& sink, const std::string_view event, Builder&& fields) noexcept {
 if (!sink) { return; }
 nlohmann::json payload;
 try {
  payload = std::invoke(std::forward<Builder>(fields));
 } catch (...) {
  // Forward one failure indication, separately from sink delivery.
  payload = nullptr;
 }
 try {
  sink(event, payload);
 } catch (...) {
  // A sink may have delivered before throwing. Never retry it.
 }
}
enum class BenchmarkCostKind : std::uint8_t { PhysicalWait, RecoveryDrain, ComponentGather, InventorySeal, Selection, Compaction };
template <class Fields>
class BenchmarkCostSpan final {
 const BenchmarkTraceSink& sink_;
 BenchmarkCostKind kind_;
 Fields fields_;
 detail::CompileObservationClock clock_;
 int exceptions_ = 0;
 bool finished_ = false;
 void report(std::string_view event, bool failed) noexcept {
  trace_benchmark_event(sink_, event, [&] {
   auto value = fields_();
   value["kind"] = kind_;
   value["span_id"] = reinterpret_cast<std::uintptr_t>(this);
   value["duration_ns"] = clock_.elapsed();
   value["failed"] = failed;
   return value;
  });
 }

public:
 BenchmarkCostSpan(const BenchmarkTraceSink& sink, BenchmarkCostKind kind, Fields fields) : sink_(sink), kind_(kind), fields_(std::move(fields)), clock_(bool(sink)) {
  if (sink_) {
   exceptions_ = std::uncaught_exceptions();
   report("benchmark.cost.started", false);
  }
 }
 ~BenchmarkCostSpan() { finish(); }
 BenchmarkCostSpan(const BenchmarkCostSpan&) = delete;
 BenchmarkCostSpan& operator=(const BenchmarkCostSpan&) = delete;
 void finish() noexcept {
  if (!sink_ || finished_) return;
  finished_ = true;
  report("benchmark.cost.completed", std::uncaught_exceptions() > exceptions_);
 }
};
}  // namespace mmltk::backend::data::benchmark_internal
