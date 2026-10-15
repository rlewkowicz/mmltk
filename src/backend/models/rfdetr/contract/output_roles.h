#pragma once
#include "src/frameworks/reflection/reflected_declarations.h"
#include <cstdint>
#include <cstddef>
#include <compare>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include "src/backend/models/rfdetr/contract/model_topology.h"
#include "mmltk/frameworks/reflection/materializer.h"
namespace mmltk::backend::models::rfdetr {
struct RfdetrOutputPolicy final {
 std::string_view name{};
 std::uint8_t rank = 0;
 bool executable = false;
 // An executable role without a task is required by every model task.
 std::optional<ModelTask> task{};
 [[nodiscard]] constexpr bool supports(ModelTask selected) const noexcept { return executable && (!task || *task == selected); }
};
// Persisted identities and spellings are independent of execution support.
// Executable declaration order is the external tensor order.
enum class RfdetrOutputRole : std::uint8_t {
 Unspecified [[= RfdetrOutputPolicy{.executable = false}]] = 0,
 Logits [[= RfdetrOutputPolicy{"pred_logits", 3, true}]] = 1,
 Boxes [[= RfdetrOutputPolicy{"pred_boxes", 3, true}]] = 2,
 Masks [[= RfdetrOutputPolicy{"pred_masks", 4, true, ModelTask::Segmentation}]] = 3,
 Keypoints [[= RfdetrOutputPolicy{"pred_keypoints", 4, true, ModelTask::Keypoints}]] = 4,
 KeypointScores [[= RfdetrOutputPolicy{.executable = false}]] = 5,
 KeypointPrecision [[= RfdetrOutputPolicy{.executable = false}]] = 6,
};
MMLTK_REFLECT_ENUM(RfdetrOutputRole)
struct RfdetrOutputSchema final {
 inline static constexpr auto members = std::define_static_array(std::meta::enumerators_of(^^RfdetrOutputRole));
 template <std::meta::info Member>
 [[nodiscard]] static consteval RfdetrOutputPolicy policy() {
  constexpr auto annotations = std::define_static_array(std::meta::annotations_of(Member));
  static_assert(annotations.size() == 1);
  constexpr auto result = std::meta::extract<RfdetrOutputPolicy>(annotations[0]);
  static_assert(result.executable ? !result.name.empty() && result.rank >= 3 : result.name.empty() && result.rank == 0 && !result.task);
  static_assert(!result.task || mmltk::frameworks::reflection::enum_contains(*result.task));
  return result;
 }
 template <class Visitor>
 static constexpr void visit(Visitor&& visitor) {
  template for (constexpr auto member : members) {
   constexpr auto facts = policy<member>();
   if constexpr (facts.executable) visitor.template operator()<std::meta::extract<RfdetrOutputRole>(member)>(facts);
  }
 }
 template <RfdetrOutputRole Role>
 [[nodiscard]] static consteval std::size_t index() {
  std::size_t current = 0, result = members.size();
  visit([&]<RfdetrOutputRole Candidate>(RfdetrOutputPolicy) {
   if constexpr (Role == Candidate) result = current;
   ++current;
  });
  if (result == members.size()) throw "RF-DETR output role is not executable";
  return result;
 }
 [[nodiscard]] static constexpr std::size_t count(ModelTask task) {
  std::size_t result = 0;
  visit([&]<RfdetrOutputRole>(RfdetrOutputPolicy facts) { result += facts.supports(task); });
  return result;
 }
 [[nodiscard]] static constexpr RfdetrOutputRole canonical_role(std::string_view name) {
  auto result = RfdetrOutputRole::Unspecified;
  visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) { if (facts.name == name) result = Role; });
  return result;
 }
};
inline constexpr std::size_t kRfdetrExecutableOutputCount = [] consteval {
 std::size_t count = 0;
 RfdetrOutputSchema::visit([&]<RfdetrOutputRole>(RfdetrOutputPolicy) { ++count; });
 return count;
}();
static_assert([] consteval {
 std::size_t identity = 0;
 template for (constexpr auto member : RfdetrOutputSchema::members) {
  if (static_cast<std::size_t>(std::meta::extract<RfdetrOutputRole>(member)) != identity++) return false;
  constexpr auto facts = RfdetrOutputSchema::policy<member>();
  template for (constexpr auto other : RfdetrOutputSchema::members) {
   if constexpr (member != other && facts.executable && RfdetrOutputSchema::policy<other>().executable)
    if (facts.name == RfdetrOutputSchema::policy<other>().name) return false;
  }
 }
 return true;
}());
inline constexpr std::size_t kMaximumNamedOutputRoles = mmltk::frameworks::reflection::enum_entries<RfdetrOutputRole>().size() - 1U;
struct RfdetrNamedOutputRole final {
 MMLTK_MAX_BYTES(1024U) std::string name;
 RfdetrOutputRole role = RfdetrOutputRole::Unspecified;
 auto operator<=>(const RfdetrNamedOutputRole&) const = default;
};
MMLTK_REFLECT_FIELDS(RfdetrNamedOutputRole)
}  // namespace mmltk::backend::models::rfdetr
