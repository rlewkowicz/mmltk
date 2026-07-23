#include "mmltk/rfdetr/weight_catalog.h"
#include "mmltk/rfdetr/preset_catalog.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace mmltk::rfdetr {

const std::vector<WeightAsset>& weight_catalog() {
    static const std::vector<WeightAsset> kCatalog = {
        {"rf-detr-large-2026.pth", "https://storage.googleapis.com/rfdetr/rf-detr-large-2026.pth",
         "5cb72153541cbcb9aa6efa26222acc75"},
        {"rf-detr-nano.pth", "https://storage.googleapis.com/rfdetr/nano_coco/checkpoint_best_regular.pth",
         "fb6504cce7fbdc783f7a46991f07639f"},
        {"rf-detr-small.pth", "https://storage.googleapis.com/rfdetr/small_coco/checkpoint_best_regular.pth",
         "fb37061c1af7bace359c91b723a8d5c1"},
        {"rf-detr-medium.pth", "https://storage.googleapis.com/rfdetr/medium_coco/checkpoint_best_regular.pth",
         "7223f764a87b863f02eb8d52bf0ce2ee"},
        {"rf-detr-seg-nano.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-n-ft.pth",
         "9995497791d0ff1664a1d9ddee9cfd20"},
        {"rf-detr-seg-small.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-s-ft.pth",
         "0a2a3006381d0c42853907e700eadd08"},
        {"rf-detr-seg-medium.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-m-ft.pth",
         "a49af1562c3719227ad43d0ca53b4c7a"},
        {"rf-detr-seg-large.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-l-ft.pth",
         "275f7b094909544ed2841c94a677d07e"},
        {"rf-detr-seg-xlarge.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-xl-ft.pth",
         "3693b35d0eea86ebb3e0444f4a611fba"},
        {"rf-detr-seg-xxlarge.pt", "https://storage.googleapis.com/rfdetr/rf-detr-seg-2xl-ft.pth",
         "040bc3412af840fa8a47e0ff69b552ba"},
    };
    static const bool kCatalogValidated = [&] {
        if (kCatalog.size() != kPresetCatalog.size()) {
            throw std::logic_error("RF-DETR weight asset count does not match the canonical preset catalog");
        }
        for (const PresetCatalogEntry& preset : kPresetCatalog) {
            const auto found = std::ranges::find(kCatalog, preset.canonical_weight_filename, &WeightAsset::filename);
            if (found == kCatalog.end() || found->download_url.empty() || found->md5_hash.size() != 32U) {
                throw std::logic_error("RF-DETR preset has no valid canonical weight asset: " +
                                       std::string(preset.preset_name));
            }
        }
        return true;
    }();
    (void)kCatalogValidated;
    return kCatalog;
}

// cppcheck-suppress passedByValue
const WeightAsset* find_weight_asset(std::string_view filename) {
    for (const auto& asset : weight_catalog()) {
        if (asset.filename == filename) {
            return &asset;
        }
    }
    return nullptr;
}

std::optional<WeightAsset> resolve_weight_asset_for_path(const std::string& path) {
    const std::string filename = std::filesystem::path(path).filename().string();
    if (const auto* asset = find_weight_asset(filename)) {
        return *asset;
    }
    return std::nullopt;
}

// cppcheck-suppress passedByValue
bool is_registered_weight_asset(std::string_view filename) {
    return find_weight_asset(filename) != nullptr;
}

}  
