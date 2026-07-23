#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mmltk::rfdetr {

enum class ModelTask : std::uint8_t {
    Detection,
    Segmentation,
};

struct PresetCatalogEntry {
    std::string_view preset_name;
    std::string_view display_name;
    std::string_view size_label;
    ModelTask task;
    std::uint32_t resolution;
    std::string_view canonical_weight_filename;
};

inline constexpr std::array<PresetCatalogEntry, 10U> kPresetCatalog{{
    {"rf-detr-nano", "RF-DETR Nano", "N", ModelTask::Detection, 384U, "rf-detr-nano.pth"},
    {"rf-detr-small", "RF-DETR Small", "S", ModelTask::Detection, 512U, "rf-detr-small.pth"},
    {"rf-detr-medium", "RF-DETR Medium", "M", ModelTask::Detection, 576U, "rf-detr-medium.pth"},
    {"rf-detr-large", "RF-DETR Large", "L", ModelTask::Detection, 704U, "rf-detr-large-2026.pth"},
    {"rf-detr-seg-nano", "RF-DETR Seg Nano", "N", ModelTask::Segmentation, 312U, "rf-detr-seg-nano.pt"},
    {"rf-detr-seg-small", "RF-DETR Seg Small", "S", ModelTask::Segmentation, 384U, "rf-detr-seg-small.pt"},
    {"rf-detr-seg-medium", "RF-DETR Seg Medium", "M", ModelTask::Segmentation, 432U, "rf-detr-seg-medium.pt"},
    {"rf-detr-seg-large", "RF-DETR Seg Large", "L", ModelTask::Segmentation, 504U, "rf-detr-seg-large.pt"},
    {"rf-detr-seg-xlarge", "RF-DETR Seg XLarge", "XL", ModelTask::Segmentation, 624U, "rf-detr-seg-xlarge.pt"},
    {"rf-detr-seg-xxlarge", "RF-DETR Seg 2XLarge", "2XL", ModelTask::Segmentation, 768U, "rf-detr-seg-xxlarge.pt"},
}};

[[nodiscard]] consteval bool preset_catalog_is_valid() {
    for (std::size_t i = 0U; i < kPresetCatalog.size(); ++i) {
        const PresetCatalogEntry& current = kPresetCatalog[i];
        if (current.preset_name.empty() || current.display_name.empty() || current.size_label.empty() ||
            current.resolution == 0U || current.canonical_weight_filename.empty()) {
            return false;
        }
        for (std::size_t j = i + 1U; j < kPresetCatalog.size(); ++j) {
            if (current.preset_name == kPresetCatalog[j].preset_name ||
                current.canonical_weight_filename == kPresetCatalog[j].canonical_weight_filename) {
                return false;
            }
        }
    }
    return true;
}

static_assert(preset_catalog_is_valid(), "RF-DETR preset catalog contains invalid or duplicate entries");

[[nodiscard]] constexpr const PresetCatalogEntry* find_preset_catalog_entry(
    const std::string_view preset_name) noexcept {
    for (const PresetCatalogEntry& entry : kPresetCatalog) {
        if (entry.preset_name == preset_name) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::string_view model_task_name(const ModelTask task) noexcept {
    return task == ModelTask::Segmentation ? "segmentation" : "detection";
}

}  
