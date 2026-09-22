#pragma once
#include <cstdint>
#include <cstddef>
#include <concepts>
#include <type_traits>
#include <meta>
#include <string>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::contracts {
// Explicit acceptance control, carried only by an installed integration driver.
// These receipts never participate in ordinary product state or diagnostics.
struct IntegrationCommandDirection final {
 bool server;
 bool read_generation = false;
 bool compiled_index = false;
};
enum class IntegrationControlKind : std::uint8_t {
 Advance[[= IntegrationCommandDirection{true}]],
 Settled[[= IntegrationCommandDirection{false}]],
 Failed[[= IntegrationCommandDirection{false}]],
 Progress[[= IntegrationCommandDirection{false}]],
 PressureEntered[[= IntegrationCommandDirection{false}]],
 CapacityArmRequested[[= IntegrationCommandDirection{false}]],
 CapacityArmed[[= IntegrationCommandDirection{true}]],
 CapacityCompletionReleased[[= IntegrationCommandDirection{true}]],
 VisibleReadArmRequested[[= IntegrationCommandDirection{false, false, true}]],
 VisibleReadArmed[[= IntegrationCommandDirection{true}]],
 VisibleReadHeld[[= IntegrationCommandDirection{true, true, true}]],
 VisibleReadReleaseRequested[[= IntegrationCommandDirection{false, true, true}]],
 GalleryReadCompletionHeld[[= IntegrationCommandDirection{true, true, true}]],
};
template <auto Kind>
 requires std::is_enum_v<decltype(Kind)>
[[nodiscard]] consteval IntegrationCommandDirection integration_command_direction() {
 using KindType = decltype(Kind);
 IntegrationCommandDirection result{};
 std::size_t count = 0U;
 template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^KindType))) {
  if constexpr (std::meta::extract<KindType>(enumerator) == Kind) {
   template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(enumerator))) {
    if constexpr (std::same_as<std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>, IntegrationCommandDirection>) {
     result = std::meta::extract<IntegrationCommandDirection>(annotation);
     ++count;
    }
   }
  }
 }
 if (count != 1U) throw "each integration kind requires its canonical direction";
 return result;
}
template <class Kind = IntegrationControlKind, class Visitor>
 requires std::is_enum_v<Kind>
constexpr void visit_integration_commands(Visitor&& visitor) {
 template for (constexpr auto entry : mmltk::frameworks::reflection::kReflectedEnumEntries<Kind>) {
  visitor.template operator()<entry.value, integration_command_direction<entry.value>()>(entry.name);
 }
}
[[nodiscard]] constexpr bool integration_server_command(const IntegrationControlKind kind) noexcept {
 bool server = false;
 visit_integration_commands([&]<auto Value, auto Policy>(auto) {
  if (kind == Value) server = Policy.server;
 });
 return server;
}
inline constexpr std::size_t kIntegrationFailureMaxBytes = 4096U;
struct IntegrationControlReceipt final {
 IntegrationControlKind kind = IntegrationControlKind::Progress;
 [[= mmltk::frameworks::reflection::Minimum{std::uint64_t{1U}}]] std::uint64_t sequence = 0U;
 std::uint64_t progress = 0U;
 // Static integration-driver caller line; zero unless kind is Failed.
 std::uint32_t failureline = 0U;
 std::uint64_t read_generation = 0U;
 std::uint32_t compiled_index = 0U;
 [[= mmltk::frameworks::reflection::MaxBytes{kIntegrationFailureMaxBytes}]] std::string failure{};
 bool operator==(const IntegrationControlReceipt&) const = default;
};
[[nodiscard]] constexpr bool integration_receipt_valid(const IntegrationControlReceipt& receipt) noexcept {
 if (receipt.sequence == 0U || (receipt.kind == IntegrationControlKind::Failed) != (receipt.failureline != 0U)) return false;
 if (receipt.failure.size() > kIntegrationFailureMaxBytes || (receipt.kind != IntegrationControlKind::Failed && !receipt.failure.empty())) return false;
 bool valid = false;
 visit_integration_commands([&]<auto Kind, auto Policy>(auto) {
  if (receipt.kind == Kind) valid = (receipt.read_generation != 0U) == Policy.read_generation && (Policy.compiled_index || receipt.compiled_index == 0U) && (!Policy.server || receipt.progress == 0U);
 });
 return valid;
}
MMLTK_REFLECT_FIELDS(IntegrationCommandDirection)
MMLTK_REFLECT_ENUM(IntegrationControlKind)
MMLTK_REFLECT_FIELDS(IntegrationControlReceipt)
}  // namespace mmltk::controller::contracts
