#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/weight_catalog.h"
#include "string_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

namespace {

std::string_view url_basename(std::string_view url) {
    const size_t slash = url.find_last_of('/');
    return slash == std::string_view::npos ? url : url.substr(slash + 1);
}

bool is_match_boundary(char ch) {
    return !std::isalnum(static_cast<unsigned char>(ch));
}

bool contains_path_token(std::string_view normalized_path, std::string_view candidate) {
    if (candidate.empty() || normalized_path.size() < candidate.size()) {
        return false;
    }

    size_t pos = normalized_path.find(candidate);
    while (pos != std::string_view::npos) {
        const size_t end = pos + candidate.size();
        const bool left_ok = pos == 0 || is_match_boundary(normalized_path[pos - 1]);
        const bool right_ok = end == normalized_path.size() || is_match_boundary(normalized_path[end]);
        if (left_ok && right_ok) {
            return true;
        }
        pos = normalized_path.find(candidate, pos + 1);
    }
    return false;
}

std::string_view preset_suffix(std::string_view preset_name) {
    constexpr std::string_view kPrefix = "rf-detr-";
    if (preset_name.starts_with(kPrefix)) {
        return preset_name.substr(kPrefix.size());
    }
    return {};
}

void consider_match_candidate(const ModelPresetConfig& preset, std::string_view normalized_path,
                              std::string_view candidate, size_t base_score, const ModelPresetConfig*& best_match,
                              size_t& best_score) {
    if (candidate.empty() || !contains_path_token(normalized_path, candidate)) {
        return;
    }
    const size_t score = base_score + candidate.size();
    if (score > best_score) {
        best_match = &preset;
        best_score = score;
    }
}

void consider_preset_aliases(const ModelPresetConfig& preset, std::string_view normalized_path,
                             const ModelPresetConfig*& best_match, size_t& best_score) {
    const std::string_view suffix = preset_suffix(preset.preset_name);
    if (suffix.empty()) {
        return;
    }

    consider_match_candidate(preset, normalized_path, strings::to_lower(suffix), 1500, best_match, best_score);

    if (suffix == "seg-nano") {
        consider_match_candidate(preset, normalized_path, "seg-n", 1400, best_match, best_score);
    } else if (suffix == "seg-small") {
        consider_match_candidate(preset, normalized_path, "seg-s", 1400, best_match, best_score);
    } else if (suffix == "seg-medium") {
        consider_match_candidate(preset, normalized_path, "seg-med", 1400, best_match, best_score);
        consider_match_candidate(preset, normalized_path, "seg-m", 1390, best_match, best_score);
    } else if (suffix == "seg-large") {
        consider_match_candidate(preset, normalized_path, "seg-l", 1400, best_match, best_score);
    } else if (suffix == "seg-xlarge") {
        consider_match_candidate(preset, normalized_path, "seg-xl", 1400, best_match, best_score);
    } else if (suffix == "seg-xxlarge") {
        consider_match_candidate(preset, normalized_path, "seg-2xl", 1400, best_match, best_score);
        consider_match_candidate(preset, normalized_path, "seg-xxl", 1390, best_match, best_score);
    }
}

const std::vector<ModelPresetConfig>& preset_table() {
    static const std::vector<ModelPresetConfig> kPresets = {
        {"rf-detr-nano",
         "dinov2_windowed_small",
         "rf-detr-nano.pth",
         384,
         16,
         2,
         24,
         2,
         300,
         300,
         91,
         256,
         13,
         true,
         false,
         1.0,
         5.0,
         2.0,
         1.0,
         1.0},
        {"rf-detr-small",
         "dinov2_windowed_small",
         "rf-detr-small.pth",
         512,
         16,
         2,
         32,
         3,
         300,
         300,
         91,
         256,
         13,
         true,
         false,
         1.0,
         5.0,
         2.0,
         1.0,
         1.0},
        {"rf-detr-medium",
         "dinov2_windowed_small",
         "rf-detr-medium.pth",
         576,
         16,
         2,
         36,
         4,
         300,
         300,
         91,
         256,
         13,
         true,
         false,
         1.0,
         5.0,
         2.0,
         1.0,
         1.0},
        {"rf-detr-large",
         "dinov2_windowed_small",
         "rf-detr-large-2026.pth",
         704,
         16,
         2,
         44,
         4,
         300,
         300,
         91,
         256,
         13,
         true,
         false,
         1.0,
         5.0,
         2.0,
         1.0,
         1.0},
        {"rf-detr-seg-nano",
         "dinov2_windowed_small",
         "rf-detr-seg-nano.pt",
         312,
         12,
         1,
         26,
         4,
         100,
         100,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
        {"rf-detr-seg-small",
         "dinov2_windowed_small",
         "rf-detr-seg-small.pt",
         384,
         12,
         2,
         32,
         4,
         100,
         100,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
        {"rf-detr-seg-medium",
         "dinov2_windowed_small",
         "rf-detr-seg-medium.pt",
         432,
         12,
         2,
         36,
         5,
         200,
         200,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
        {"rf-detr-seg-large",
         "dinov2_windowed_small",
         "rf-detr-seg-large.pt",
         504,
         12,
         2,
         42,
         5,
         200,
         200,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
        {"rf-detr-seg-xlarge",
         "dinov2_windowed_small",
         "rf-detr-seg-xlarge.pt",
         624,
         12,
         2,
         52,
         6,
         300,
         300,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
        {"rf-detr-seg-xxlarge",
         "dinov2_windowed_small",
         "rf-detr-seg-xxlarge.pt",
         768,
         12,
         2,
         64,
         6,
         300,
         300,
         91,
         256,
         13,
         true,
         true,
         5.0,
         5.0,
         2.0,
         5.0,
         5.0},
    };
    return kPresets;
}

}  // namespace

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

const ModelPresetConfig* infer_model_preset_from_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return nullptr;
    }

    if (const auto* preset = find_model_preset_by_weight_filename(path.filename().string())) {
        return preset;
    }

    const std::string normalized_path = strings::to_lower(path.lexically_normal().string());
    const ModelPresetConfig* best_match = nullptr;
    size_t best_score = 0;

    for (const auto& preset : preset_table()) {
        const std::string canonical_name = strings::to_lower(preset.canonical_weight_filename);
        consider_match_candidate(preset, normalized_path, canonical_name, 3000, best_match, best_score);

        const std::string canonical_stem =
            strings::to_lower(std::filesystem::path(std::string(preset.canonical_weight_filename)).stem().string());
        consider_match_candidate(preset, normalized_path, canonical_stem, 2900, best_match, best_score);

        const std::string preset_name = strings::to_lower(preset.preset_name);
        consider_match_candidate(preset, normalized_path, preset_name, 2800, best_match, best_score);

        if (const auto* asset = find_weight_asset(preset.canonical_weight_filename)) {
            const std::string_view basename = url_basename(asset->download_url);
            if (find_model_preset_by_weight_filename(basename) == &preset) {
                const std::string upstream_name = strings::to_lower(basename);
                consider_match_candidate(preset, normalized_path, upstream_name, 2700, best_match, best_score);

                const std::string upstream_stem =
                    strings::to_lower(std::filesystem::path(std::string(basename)).stem().string());
                consider_match_candidate(preset, normalized_path, upstream_stem, 2600, best_match, best_score);
            }
        }

        consider_preset_aliases(preset, normalized_path, best_match, best_score);
    }

    return best_match;
}

}  // namespace mmltk::rfdetr
