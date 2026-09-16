#pragma once
#include <cstddef>
#include <filesystem>
#include <meta>
#include <span>
#include <string_view>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
namespace mmltk::backend::models::rfdetr {
struct RfdetrCapabilities final {
    bool weights;
    bool onnx;
    bool tensorrt;
    bool training;
    bool live;
    constexpr bool operator==(const RfdetrCapabilities&) const noexcept = default;
};
// This aggregate is the model package's one reflected catalog contribution.
// Catalog projection inspects this stable value; RF-DETR implementation
// components consume the same preset/configuration authority.
struct RfdetrContractContribution final {
    using ArtifactPresetInference = std::string_view (*)(const std::filesystem::path&);
    std::string_view model_id;
    std::string_view display_name;
    RfdetrCapabilities capabilities;
    std::span<const PresetCatalogEntry> presets;
    ArtifactPresetInference infer_artifact_preset;
};
[[nodiscard]] std::string_view infer_artifact_preset(const std::filesystem::path& path);
inline constexpr RfdetrContractContribution kRfdetrContract{
    .model_id = "rfdetr",
    .display_name = "RF-DETR",
    .capabilities =
        {
            .weights = true,
            .onnx = true,
            .tensorrt = true,
            .training = true,
            .live = true,
        },
    .presets = kPresetCatalog,
    .infer_artifact_preset = &infer_artifact_preset,
};
[[nodiscard]] consteval bool reflected_contract_shape_is_valid() {
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^RfdetrContractContribution, std::meta::access_context::current()));
    return members.size() == 5U && !kRfdetrContract.model_id.empty() && !kRfdetrContract.display_name.empty() && !kRfdetrContract.presets.empty();
}
static_assert(reflected_contract_shape_is_valid());
}  // namespace mmltk::backend::models::rfdetr
