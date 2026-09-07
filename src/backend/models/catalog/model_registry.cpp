#include "src/backend/models/catalog/model_registry.h"

#include <filesystem>
#include <span>
#include <string_view>

#include "src/backend/models/catalog/artifacts.h"
#include "src/backend/models/catalog/module.h"

namespace mmltk::backend::models::catalog {

namespace detail {

[[nodiscard]] std::span<const ModelDescriptor> models() noexcept;

}  // namespace detail

std::span<const ModelDescriptor> models() noexcept { return detail::models(); }

const ModelDescriptor* find_model(const std::string_view model_id) noexcept {
    for (const ModelDescriptor& model : models()) {
        if (model.model_id == model_id) return &model;
    }
    return nullptr;
}

const ModelDescriptor* find_model_for_preset(const std::string_view preset_name) noexcept {
    for (const ModelDescriptor& model : models()) {
        for (const ModelPresetDescriptor& preset : model.presets) {
            if (preset.preset_name == preset_name) return &model;
        }
    }
    return nullptr;
}

const ModelDescriptor* find_model_for_artifact_path(const std::filesystem::path& artifact_path) {
    const ModelDescriptor* candidate = nullptr;
    for (const ModelDescriptor& model : models()) {
        if (model.infer_artifact_preset != nullptr && !model.infer_artifact_preset(artifact_path).empty()) {
            if (candidate != nullptr && candidate != &model) return nullptr;
            candidate = &model;
        }
    }
    return candidate;
}

}  // namespace mmltk::backend::models::catalog
