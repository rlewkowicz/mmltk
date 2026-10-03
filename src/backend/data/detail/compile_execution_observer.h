#pragma once
#include <concepts>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>
namespace mmltk::backend::data::detail {
struct CompileExecutionSnapshot;
// Immutable diagnostic configuration, independent of execution and resource state.
struct CompileExecutionObservationDemand {
 bool aggregates = false, snapshots = false, record_rejections = false, continuation_rejections = false, reservations = false, retired = false;
 [[nodiscard]] bool any() const noexcept { return aggregates || snapshots || record_rejections || continuation_rejections || reservations || retired; }
 [[nodiscard]] bool detailed_demands() const noexcept { return snapshots || record_rejections || continuation_rejections || reservations; }
 [[nodiscard]] static constexpr CompileExecutionObservationDemand all() noexcept { return {true, true, true, true, true, true}; }
};
class CompileExecutionObserver final {
 CompileExecutionObservationDemand demand_{};
 std::function<void(std::string_view, const CompileExecutionSnapshot&)> delivery_;

public:
 CompileExecutionObserver() = default;
 template <class F> requires (!std::same_as<std::remove_cvref_t<F>, CompileExecutionObserver> && std::is_invocable_r_v<void, F&, std::string_view, const CompileExecutionSnapshot&>)
 CompileExecutionObserver(F&& delivery) : CompileExecutionObserver(CompileExecutionObservationDemand::all(), std::forward<F>(delivery)) {}
 template <class F> requires std::is_invocable_r_v<void, F&, std::string_view, const CompileExecutionSnapshot&>
 CompileExecutionObserver(CompileExecutionObservationDemand demand, F&& delivery) : demand_(demand), delivery_(std::forward<F>(delivery)) { if (!delivery_) demand_ = {}; }
 [[nodiscard]] explicit operator bool() const noexcept { return delivery_ && demand_.any(); }
 [[nodiscard]] const CompileExecutionObservationDemand& demand() const noexcept { return demand_; }
 void operator()(std::string_view event, const CompileExecutionSnapshot& value) const { if (delivery_) delivery_(event, value); }
};
}  // namespace mmltk::backend::data::detail
