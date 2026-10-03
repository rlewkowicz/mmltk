#pragma once
#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::backend::data {
// Immutable diagnostic configuration, cheaply shared by every delivery adapter.
// An empty prefix set selects all events; a missing delivery selects none.
class CompileTraceSelection final {
 std::shared_ptr<const std::vector<std::string>> prefixes_;

public:
 CompileTraceSelection() = default;
 explicit CompileTraceSelection(std::vector<std::string> prefixes)
     : prefixes_(prefixes.empty() ? nullptr : std::make_shared<const std::vector<std::string>>(std::move(prefixes))) {}
 [[nodiscard]] bool accepts(std::string_view event) const noexcept {
  return !prefixes_ || std::ranges::any_of(*prefixes_, [&](const auto& prefix) { return event.starts_with(prefix); });
 }
};
// Selection travels with delivery through serialized and native adapters.
// Payload ownership and error policy stay with the concrete producer.
template <class Payload>
class CompileTraceSink final {
 CompileTraceSelection selection_;
 std::function<void(std::string_view, Payload)> delivery_;

public:
 CompileTraceSink() = default;
 template <class F> requires (!std::same_as<std::remove_cvref_t<F>, CompileTraceSink> && std::is_invocable_r_v<void, F&, std::string_view, Payload>)
 CompileTraceSink(F&& delivery) : delivery_(std::forward<F>(delivery)) {}
 template <class F> requires std::is_invocable_r_v<void, F&, std::string_view, Payload>
 CompileTraceSink(CompileTraceSelection selection, F&& delivery) : selection_(std::move(selection)), delivery_(std::forward<F>(delivery)) {}
 [[nodiscard]] explicit operator bool() const noexcept { return bool(delivery_); }
 [[nodiscard]] bool accepts(std::string_view event) const noexcept { return delivery_ && selection_.accepts(event); }
 [[nodiscard]] const CompileTraceSelection& selection() const noexcept { return selection_; }
 void operator()(std::string_view event, Payload payload) const {
  // Empty event is the once-only setup-failure channel and bypasses filtering.
  if (delivery_ && (event.empty() || selection_.accepts(event))) delivery_(event, payload);
 }
};
}  // namespace mmltk::backend::data
