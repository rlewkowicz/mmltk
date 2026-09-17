#include "src/backend/models/rfdetr/contract/model_config.h"
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include "src/common/types/string_utils.h"
namespace mmltk::backend::models::rfdetr {
namespace {
[[nodiscard]] std::string_view url_basename(const std::string_view url) {
    const std::size_t slash = url.find_last_of('/');
    return slash == std::string_view::npos ? url : url.substr(slash + 1U);
}
[[nodiscard]] bool is_match_boundary(const char character) { return !std::isalnum(static_cast<unsigned char>(character)); }
[[nodiscard]] bool contains_path_token(const std::string_view normalized_path, const std::string_view candidate) {
    if (candidate.empty() || normalized_path.size() < candidate.size()) return false;
    std::size_t position = normalized_path.find(candidate);
    while (position != std::string_view::npos) {
        const std::size_t end = position + candidate.size();
        if ((position == 0U || is_match_boundary(normalized_path[position - 1U])) &&
            (end == normalized_path.size() || is_match_boundary(normalized_path[end]))) {
            return true;
        }
        position = normalized_path.find(candidate, position + 1U);
    }
    return false;
}
void consider_candidate(const PresetCatalogEntry& preset, const std::string_view normalized_path, const std::string_view candidate,
                        const std::size_t base_score, const PresetCatalogEntry*& best, std::size_t& best_score) {
    if (!contains_path_token(normalized_path, candidate)) return;
    const std::size_t score = base_score + candidate.size();
    if (score > best_score) {
        best = &preset;
        best_score = score;
    }
}
void consider_known_aliases(const PresetCatalogEntry& preset, const std::string_view normalized_path, const PresetCatalogEntry*& best,
                            std::size_t& best_score) {
    struct Alias final {
        std::string_view preset;
        std::string_view path_token;
    };
    constexpr std::array aliases{
        Alias{"rf-detr-seg-nano", "seg-n"},      Alias{"rf-detr-seg-small", "seg-s"},     Alias{"rf-detr-seg-medium", "seg-med"},
        Alias{"rf-detr-seg-medium", "seg-m"},    Alias{"rf-detr-seg-large", "seg-l"},     Alias{"rf-detr-seg-xlarge", "seg-xl"},
        Alias{"rf-detr-seg-xxlarge", "seg-2xl"}, Alias{"rf-detr-seg-xxlarge", "seg-xxl"},
    };
    for (const Alias& alias : aliases) {
        if (alias.preset == preset.preset_name) { consider_candidate(preset, normalized_path, alias.path_token, 1400U, best, best_score); }
    }
}
}  // namespace
std::span<const PresetCatalogEntry> model_presets() noexcept { return kPresetCatalog; }
const PresetCatalogEntry* find_model_preset_by_weight_filename(const std::string_view filename) noexcept {
    const PresetCatalogEntry* match = nullptr;
    for (const auto& preset : kPresetCatalog) {
        if (preset.canonical_weight_filename == filename) return &preset;
        const auto* asset = find_weight_asset(preset.canonical_weight_filename);
        if (asset != nullptr && url_basename(asset->download_url) == filename) {
            if (match != nullptr) return nullptr;
            match = &preset;
        }
    }
    return match;
}
const PresetCatalogEntry* infer_model_preset_from_path(const std::filesystem::path& path) {
    if (path.empty()) return nullptr;
    if (const auto* preset = find_model_preset_by_weight_filename(path.filename().string())) { return preset; }
    const std::string normalized = mmltk::common::types::to_lower(path.lexically_normal().string());
    const PresetCatalogEntry* best = nullptr;
    std::size_t best_score = 0U;
    for (const auto& preset : kPresetCatalog) {
        const std::string weight = mmltk::common::types::to_lower(preset.canonical_weight_filename);
        consider_candidate(preset, normalized, weight, 3000U, best, best_score);
        consider_candidate(preset, normalized, mmltk::common::types::to_lower(std::filesystem::path(weight).stem().string()), 2900U, best, best_score);
        consider_candidate(preset, normalized, mmltk::common::types::to_lower(preset.preset_name), 2800U, best, best_score);
        if (preset.preset_name.starts_with("rf-detr-")) {
            consider_candidate(preset, normalized, mmltk::common::types::to_lower(preset.preset_name.substr(std::string_view{"rf-detr-"}.size())), 1500U, best,
                               best_score);
        }
        consider_known_aliases(preset, normalized, best, best_score);
    }
    return best;
}
NativeRfDetrConfig native_config_from_preset(const PresetCatalogEntry& preset) {
    return {
        .preset_name = std::string(preset.preset_name),
        .resolution = static_cast<int>(preset.resolution),
        .segmentation = preset.task == ModelTask::Segmentation,
        .num_classes = preset.class_count,
        .num_queries = preset.query_count,
        .num_select = preset.selected_query_count,
        .dec_layers = preset.decoder_layer_count,
        .group_detr = preset.group_count,
        .two_stage = preset.two_stage,
        .hidden_dim = preset.hidden_dimension,
        .patch_size = preset.patch_size,
        .num_windows = preset.window_count,
        .positional_encoding_size = preset.positional_encoding_size,
        .sa_nheads = 8,
        .ca_nheads = 16,
        .dec_n_points = 2,
        .dim_feedforward = 2048,
        .bbox_reparam = true,
        .lite_refpoint_refine = true,
        .cls_loss_coef = preset.classification_loss_coefficient,
        .bbox_loss_coef = preset.bounding_box_loss_coefficient,
        .giou_loss_coef = preset.generalized_iou_loss_coefficient,
        .mask_ce_loss_coef = preset.mask_cross_entropy_loss_coefficient,
        .mask_dice_loss_coef = preset.mask_dice_loss_coefficient,
        .training_supervision = {},
    };
}
}  // namespace mmltk::backend::models::rfdetr
#include "src/backend/models/rfdetr/contract/contract.h"
namespace mmltk::backend::models::rfdetr {
std::string_view infer_artifact_preset(const std::filesystem::path& path) {
    const auto* preset = infer_model_preset_from_path(path);
    return preset == nullptr ? std::string_view{} : preset->preset_name;
}
}  // namespace mmltk::backend::models::rfdetr
