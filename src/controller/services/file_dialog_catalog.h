#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include "src/controller/contracts/settings_commands.h"
#include "src/controller/contracts/workflows.h"
#include "src/controller/services/file_dialog_types.h"
namespace mmltk::controller::services {
inline constexpr std::size_t kFileDialogCatalogCapacity = 64U;
// The service retains each declaration-derived entry once. Browser requests
// carry only the declaration's stable reflected field identity.
struct FileDialogDescriptor final {
 std::uint64_t stable_id = 0U;
 BoundedText<kFileDialogTextCapacity> field_path{};
 mmltk::controller::contracts::reflection::ReflectedWorkflowPolicy workflows{};
 mmltk::controller::contracts::FileDialogMode mode = mmltk::controller::contracts::FileDialogMode::OpenFile;
 mmltk::controller::services::FileDialogFilter filter{};
 BoundedText<kFileDialogTextCapacity> title{};
 std::optional<mmltk::backend::models::catalog::ModelArtifactInputKind> model_input{};
 [[nodiscard]] constexpr bool defer_apply() const noexcept { return model_input.has_value(); }
 constexpr bool operator==(const FileDialogDescriptor&) const noexcept = default;
};
struct ResolvedFileDialog final {
 FileDialogDescriptor descriptor{};
 FileDialogTarget target{};
 constexpr bool operator==(const ResolvedFileDialog&) const noexcept = default;

private:
 explicit constexpr ResolvedFileDialog(FileDialogDescriptor value, FileDialogTarget requested) noexcept : descriptor(value), target(requested) {}
 friend class FileDialogCatalog;
};
class FileDialogCatalog final {
public:
 [[nodiscard]] std::optional<ResolvedFileDialog> resolve(mmltk::controller::services::FileDialogOpen request) const noexcept;
 [[nodiscard]] std::span<const FileDialogDescriptor> entries() const noexcept { return {entries_.data(), size_}; }

private:
 FileDialogCatalog() = default;
 std::array<FileDialogDescriptor, kFileDialogCatalogCapacity> entries_{};
 std::uint16_t size_ = 0U;
 [[nodiscard]] static FileDialogCatalog Build();
 [[nodiscard]] static FileDialogCatalog Create(std::span<const FileDialogDescriptor> entries);
 friend const FileDialogCatalog& file_dialog_catalog();
};
[[nodiscard]] const FileDialogCatalog& file_dialog_catalog();
[[nodiscard]] std::optional<mmltk::controller::contracts::SettingsValueUpdate> resolve_file_dialog_path(std::uint64_t stable_field_id, std::string_view path);
}  // namespace mmltk::controller::services
