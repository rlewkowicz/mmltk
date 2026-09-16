#pragma once
#include <filesystem>
#include <span>
#include <string_view>
#include "src/backend/models/catalog/module.h"
namespace mmltk::backend::models::catalog {
// CLEANUP-IGNORE: Registry lookup functions are a projected catalog API, not duplicate model-configuration fields.
[[nodiscard]] std::span<const ModelDescriptor> models() noexcept;
[[nodiscard]] const ModelDescriptor* find_model(std::string_view model_id) noexcept;
[[nodiscard]] const ModelDescriptor* find_model_for_preset(std::string_view preset_name) noexcept;
[[nodiscard]] const ModelDescriptor* find_model_for_artifact_path(const std::filesystem::path& artifact_path);
}  // namespace mmltk::backend::models::catalog
