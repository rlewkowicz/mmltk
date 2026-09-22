#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string_view>
#include <type_traits>
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::contracts::terminal_presentation {
enum class Classification : std::uint8_t {
 Success,
 Refused,
 Failed,
 Cancelled,
 Closed,
};
struct ClassificationPolicy final {
 Classification classification = Classification::Failed;
 bool success = false;
};
inline constexpr std::array kClassificationPolicy{
 ClassificationPolicy{Classification::Success, true},
 ClassificationPolicy{Classification::Refused, false},
 ClassificationPolicy{Classification::Failed, false},
 ClassificationPolicy{Classification::Cancelled, false},
 ClassificationPolicy{Classification::Closed, false},
};
// GCC 16 expands each template-for iteration into the same diagnostic scope
// and reports the reflected loop variable as self-shadowing.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
[[nodiscard]] consteval bool classification_policy_complete() {
 static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Classification));
 if (kClassificationPolicy.size() != enumerators.size()) return false;
 std::size_t successes = 0U;
 for (std::size_t index = 0U; index < kClassificationPolicy.size(); ++index) {
  const auto& policy = kClassificationPolicy[index];
  successes += policy.success;
  std::size_t matches = 0U;
  template for (constexpr auto enumerator : enumerators) matches += policy.classification == [:enumerator:];
  if (matches != 1U) return false;
  for (std::size_t prior = 0U; prior < index; ++prior)
   if (kClassificationPolicy[prior].classification == policy.classification) return false;
 }
 template for (constexpr auto enumerator : enumerators) {
  std::size_t matches = 0U;
  for (const auto& policy : kClassificationPolicy) matches += policy.classification == [:enumerator:];
  if (matches != 1U) return false;
 }
 return successes == 1U;
}
static_assert(classification_policy_complete());
[[nodiscard]] constexpr const ClassificationPolicy* classification_policy(const Classification classification) noexcept {
 for (const auto& policy : kClassificationPolicy)
  if (policy.classification == classification) return &policy;
 return nullptr;
}
MMLTK_REFLECT_ENUM(Classification)
template <class Terminal>
 requires std::is_enum_v<Terminal>
struct Policy final {
 Terminal terminal{};
 Classification classification = Classification::Failed;
 std::string_view message_identity;
 std::string_view message;
};
template <class Terminal, std::size_t Size>
[[nodiscard]] consteval bool complete(const std::array<Policy<Terminal>, Size>& policies) {
 static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Terminal));
 if (policies.size() != enumerators.size()) return false;
 for (std::size_t index = 0U; index < policies.size(); ++index) {
  const auto& policy = policies[index];
  bool valid_terminal = false;
  template for (constexpr auto enumerator : enumerators) valid_terminal = valid_terminal || policy.terminal == [:enumerator:];
  const auto* classification = classification_policy(policy.classification);
  if (!valid_terminal || classification == nullptr || policy.message_identity.empty() || classification->success != policy.message.empty()) return false;
  for (std::size_t prior = 0U; prior < index; ++prior) {
   if (policies[prior].terminal == policy.terminal || policies[prior].message_identity == policy.message_identity) return false;
  }
 }
 template for (constexpr auto enumerator : enumerators) {
  std::size_t matches = 0U;
  for (const auto& policy : policies) matches += policy.terminal == [:enumerator:];
  if (matches != 1U) return false;
 }
 return true;
}
template <class Enum>
 requires std::is_enum_v<Enum>
[[nodiscard]] consteval auto reflected_values() {
 static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Enum));
 std::array<Enum, enumerators.size()> values{};
 std::size_t index = 0U;
 template for (constexpr auto enumerator : enumerators) values[index++] = [:enumerator:];
 return values;
}
template <class Enum>
 requires std::is_enum_v<Enum>
[[nodiscard]] constexpr bool valid(const Enum value) noexcept {
 static constexpr auto values = reflected_values<Enum>();
 for (const Enum candidate : values)
  if (candidate == value) return true;
 return false;
}
struct TerminalField final {
 std::meta::info type = ^^void;
 std::string_view name;
};
template <class Result>
[[nodiscard]] consteval TerminalField terminal_field() {
 using Value = std::remove_cvref_t<Result>;
 if constexpr (std::is_enum_v<Value>) {
  return {.type = ^^Value};
 } else if constexpr (!std::is_class_v<Value>) {
  return {};
 } else {
  TerminalField result{};
  std::size_t matches = 0U;
  template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^Value, std::meta::access_context::unchecked()))) {
   constexpr std::string_view name = std::meta::identifier_of(member);
   using Member = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
   if constexpr ((name == "terminal" || name == "outcome") && std::is_enum_v<Member>) {
    result = {.type = ^^Member, .name = name};
    ++matches;
   }
  }
  if (matches > 1U) throw "browser result has more than one native terminal field";
  return result;
 }
}
template <class Result>
using terminal_enum_t = typename[:terminal_field<Result>().type:];
template <class Result>
[[nodiscard]] consteval std::string_view terminal_field_name() {
 return terminal_field<Result>().name;
}
#pragma GCC diagnostic pop
template <class Result>
inline constexpr bool has_policy = !std::same_as<terminal_enum_t<Result>, void> && requires { materialized_terminal_presentation_policy(std::type_identity<terminal_enum_t<Result>>{}); };
template <class Result>
[[nodiscard]] consteval const auto& materialized_policy()
 requires has_policy<Result>
{
 constexpr const auto& policies = materialized_terminal_presentation_policy(std::type_identity<terminal_enum_t<Result>>{});
 static_assert(complete(policies), "native terminal presentation policy must cover its reflected enum exactly once");
 return policies;
}
}  // namespace mmltk::controller::contracts::terminal_presentation
