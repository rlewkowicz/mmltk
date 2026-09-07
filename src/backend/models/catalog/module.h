#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "src/backend/models/catalog/artifacts.h"
namespace mmltk::backend::models::catalog {

struct ModelCapabilities final {
    bool weights = false;
    bool onnx = false;
    bool tensorrt = false;
    bool training = false;
    bool live = false;

    constexpr bool operator==(const ModelCapabilities&) const noexcept = default;
};

struct ModelPresetDescriptor final {
    std::string_view model_id;
    std::string_view preset_name;
    std::string_view display_name;
    std::string_view size_label;
    std::string_view task;
    std::string_view canonical_weight_filename;
    std::uint32_t resolution = 0U;

    constexpr bool operator==(const ModelPresetDescriptor&) const noexcept = default;
};

struct ModelDescriptor final {
    using ArtifactPresetInference = std::string_view (*)(const std::filesystem::path&);

    std::string_view model_id;
    std::string_view display_name;
    ModelCapabilities capabilities;
    std::span<const ModelPresetDescriptor> presets;
    ArtifactPresetInference infer_artifact_preset = nullptr;
};

}  // namespace mmltk::backend::models::catalog
