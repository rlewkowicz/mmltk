#include "src/controller/contracts/gui_settings_states.h"
#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include "src/controller/contracts/settings_vocabulary.h"
namespace mmltk::controller::contracts {
namespace {
template <class T>
[[nodiscard]] bool equal_value(const T& left, const T& right) noexcept {
 using Value = std::remove_cvref_t<T>;
 if constexpr (mmltk::frameworks::reflection::kOpaqueRelationStorage<Value>) {
  return left == right;
 } else if constexpr (std::is_arithmetic_v<Value> || std::is_enum_v<Value> || std::same_as<Value, std::string> || std::same_as<Value, std::filesystem::path>) {
  return left == right;
 } else if constexpr (settings_vocabulary::is_optional<Value>::value) {
  return left.has_value() == right.has_value() && (!left.has_value() || equal_value(*left, *right));
 } else if constexpr (requires {
                       left.size();
                       left.begin();
                       left.end();
                      }) {
  if (left.size() != right.size()) return false;
  auto right_item = right.begin();
  for (const auto& left_item : left)
   if (!equal_value(left_item, *right_item++)) return false;
  return true;
 } else {
  bool equal = true;
  settings_vocabulary::for_each_member_pair(left, right, [&](const std::string_view, const auto& member, const auto& other) {
   if (equal) equal = equal_value(member, other);
  });
  return equal;
 }
}
}  // namespace
bool GuiSettingsState::operator==(const GuiSettingsState& other) const noexcept { return equal_value(*this, other); }
}  // namespace mmltk::controller::contracts
