#include "fastloader/rfdetr/model_config.h"
#include "fastloader/rfdetr/weight_catalog.h"

#include <string_view>

namespace fastloader::rfdetr {

namespace {

std::string_view url_basename(std::string_view url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string_view::npos ? url : url.substr(slash + 1);
}

const std::vector<ModelPresetConfig>& preset_table() {
    static const std::vector<ModelPresetConfig> kPresets = {
        {"rf-detr-nano", "dinov2_windowed_small", "rf-detr-nano.pth", 384, 16, 2, 24, 2, 300, 300, 91, 256, 13, true, false, 1.0, 5.0, 2.0, 1.0, 1.0},
        {"rf-detr-small", "dinov2_windowed_small", "rf-detr-small.pth", 512, 16, 2, 32, 3, 300, 300, 91, 256, 13, true, false, 1.0, 5.0, 2.0, 1.0, 1.0},
        {"rf-detr-medium", "dinov2_windowed_small", "rf-detr-medium.pth", 576, 16, 2, 36, 4, 300, 300, 91, 256, 13, true, false, 1.0, 5.0, 2.0, 1.0, 1.0},
        {"rf-detr-large", "dinov2_windowed_small", "rf-detr-large-2026.pth", 704, 16, 2, 44, 4, 300, 300, 91, 256, 13, true, false, 1.0, 5.0, 2.0, 1.0, 1.0},
        {"rf-detr-seg-nano", "dinov2_windowed_small", "rf-detr-seg-nano.pt", 312, 12, 1, 26, 4, 100, 100, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
        {"rf-detr-seg-small", "dinov2_windowed_small", "rf-detr-seg-small.pt", 384, 12, 2, 32, 4, 100, 100, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
        {"rf-detr-seg-medium", "dinov2_windowed_small", "rf-detr-seg-medium.pt", 432, 12, 2, 36, 5, 200, 200, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
        {"rf-detr-seg-large", "dinov2_windowed_small", "rf-detr-seg-large.pt", 504, 12, 2, 42, 5, 200, 200, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
        {"rf-detr-seg-xlarge", "dinov2_windowed_small", "rf-detr-seg-xlarge.pt", 624, 12, 2, 52, 6, 300, 300, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
        {"rf-detr-seg-xxlarge", "dinov2_windowed_small", "rf-detr-seg-xxlarge.pt", 768, 12, 2, 64, 6, 300, 300, 91, 256, 13, true, true, 5.0, 5.0, 2.0, 5.0, 5.0},
    };
    return kPresets;
}

} // namespace

const std::vector<ModelPresetConfig>& model_presets() {
    return preset_table();
}

// cppcheck-suppress passedByValue
const ModelPresetConfig* find_model_preset(std::string_view preset_name) {
    for (const auto& preset : preset_table()) {
        if (preset.preset_name == preset_name) {
            return &preset;
        }
    }
    return nullptr;
}

// cppcheck-suppress passedByValue
const ModelPresetConfig* find_model_preset_by_weight_filename(std::string_view filename) {
    for (const auto& preset : preset_table()) {
        if (preset.canonical_weight_filename == filename) {
            return &preset;
        }
    }
    const ModelPresetConfig* matched = nullptr;
    for (const auto& preset : preset_table()) {
        const auto* asset = find_weight_asset(preset.canonical_weight_filename);
        if (asset == nullptr || url_basename(asset->download_url) != filename) {
            continue;
        }
        if (matched != nullptr) {
            return nullptr;
        }
        matched = &preset;
    }
    if (matched != nullptr) {
        return matched;
    }
    return nullptr;
}

} // namespace fastloader::rfdetr
