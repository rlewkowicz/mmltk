#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/backend/models/catalog/artifacts.h"
#include "src/controller/contracts/settings_commands.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::services {
inline constexpr std::size_t kFileDialogTextCapacity = 512U;
inline constexpr std::size_t kFileDialogPathStorageCapacity = mmltk::frameworks::reflection::kMaximumPathBytes + 1U;
template <std::size_t Capacity>
struct BoundedText final {
 std::array<char, Capacity> bytes{};
 std::uint16_t size = 0U;
 [[nodiscard]] static constexpr BoundedText From(const std::string_view value) noexcept {
  BoundedText result{};
  if (value.empty() || value.size() >= result.bytes.size()) return result;
  std::copy_n(value.data(), value.size(), result.bytes.data());
  result.size = static_cast<std::uint16_t>(value.size());
  return result;
 }
 [[nodiscard]] constexpr bool valid() const noexcept {
  return size != 0U && size < bytes.size() && bytes[size] == '\0' &&
         std::all_of(
          bytes.begin(), bytes.begin() + size, [](const char value) { return value != '\0'; }) &&
         std::all_of(bytes.begin() + size, bytes.end(), [](const char value) { return value == '\0'; });
 }
 [[nodiscard]] constexpr std::string_view view() const noexcept { return valid() ? std::string_view{bytes.data(), size} : std::string_view{}; }
 constexpr bool operator==(const BoundedText&) const noexcept = default;
};
struct FileDialogSelected final {
 [[= mmltk::frameworks::reflection::MinBytes{
  1U}]][[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string path{};
 constexpr bool operator==(const FileDialogSelected&) const noexcept = default;
};
struct FileDialogCancelled final {
 constexpr bool operator==(const FileDialogCancelled&) const noexcept = default;
};
struct SettingsFieldTarget final {
 [[= mmltk::controller::contracts::reflection::direct::FileDialogFieldIdentity{}]] std::uint64_t stable_id = 0U;
 constexpr bool operator==(const SettingsFieldTarget&) const noexcept = default;
};
struct ModelArtifactTarget final {
 [[= mmltk::controller::contracts::reflection::direct::FileDialogFieldIdentity{}]] std::uint64_t stable_id = 0U;
 mmltk::controller::contracts::FeatureId workflow = mmltk::controller::contracts::FeatureId::Train;
 mmltk::backend::models::catalog::ModelArtifactInputKind input = mmltk::backend::models::catalog::ModelArtifactInputKind::None;
 constexpr bool operator==(const ModelArtifactTarget&) const noexcept = default;
};
struct FileDialogTarget final {
 using variant_type = std::variant<SettingsFieldTarget, ModelArtifactTarget>;
 variant_type value{};
 constexpr bool operator==(const FileDialogTarget&) const noexcept = default;
};
[[nodiscard]] constexpr std::uint64_t file_dialog_stable_id(const FileDialogTarget& target) noexcept {
 return std::visit([](const auto& value) { return value.stable_id; }, target.value);
}
[[nodiscard]] constexpr bool valid_file_dialog_target(const FileDialogTarget& target) noexcept {
 return std::visit(
  [](const auto& value) {
   if (value.stable_id == 0U) return false;
   if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, ModelArtifactTarget>) {
    return mmltk::controller::contracts::valid_feature(value.workflow) && value.input != mmltk::backend::models::catalog::ModelArtifactInputKind::None;
   }
   return true;
  },
  target.value);
}
struct FileDialogSelection final {
 FileDialogTarget target{};
 std::variant<FileDialogCancelled, FileDialogSelected> result{};
 [[nodiscard]] bool selected() const noexcept { return std::holds_alternative<FileDialogSelected>(result); }
 [[nodiscard]] bool valid() const noexcept {
  if (!valid_file_dialog_target(target)) return false;
  const auto* selected_value = std::get_if<FileDialogSelected>(&result);
  return selected_value == nullptr || !mmltk::frameworks::reflection::validate_reflected_fields(*selected_value);
 }
 [[nodiscard]] bool valid_for(const FileDialogTarget& expected_target) const noexcept { return target == expected_target && valid(); }
};
// Browser input names one installed declaration by its stable reflected field
// identity and carries no client-authored dialog policy.
struct[[= mmltk::controller::contracts::reflection::all_feature_scope()]] FileDialogOpen final {
 FileDialogTarget target{};
 constexpr bool operator==(const FileDialogOpen&) const noexcept = default;
};
struct FileDialogFilter final {
 BoundedText<kFileDialogTextCapacity> name{};
 BoundedText<kFileDialogTextCapacity> pattern{};
 constexpr bool operator==(const FileDialogFilter&) const noexcept = default;
};
MMLTK_REFLECT_FIELDS(SettingsFieldTarget)
MMLTK_REFLECT_FIELDS(ModelArtifactTarget)
MMLTK_REFLECT_FIELDS(FileDialogTarget)
MMLTK_REFLECT_FIELDS(FileDialogOpen)
MMLTK_REFLECT_FIELDS(FileDialogSelected)
MMLTK_REFLECT_FIELDS(FileDialogCancelled)
MMLTK_REFLECT_FIELDS(FileDialogSelection)
}  // namespace mmltk::controller::services
