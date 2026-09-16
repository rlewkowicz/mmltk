#pragma once
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
enum class CompilationMode : std::uint8_t {
    kNone,
    kSelective,
    kFullTrace,
};
MMLTK_REFLECT_ENUM(CompilationMode)
[[nodiscard]] constexpr std::string_view cli_enum_spelling(const CompilationMode mode) noexcept {
    switch (mode) {
        case CompilationMode::kNone: return "none";
        case CompilationMode::kSelective: return "selective";
        case CompilationMode::kFullTrace: return "full";
    }
    return {};
}
struct NativeRfDetrConfig {
    std::string preset_name;
    int resolution = 0;
    bool segmentation = false;
    int num_classes = 0;
    int num_queries = 0;
    int num_select = 0;
    int dec_layers = 0;
    int group_detr = 1;
    bool two_stage = true;
    int hidden_dim = 256;
    int patch_size = 16;
    int num_windows = 2;
    int positional_encoding_size = 24;
    int sa_nheads = 8;
    int ca_nheads = 16;
    int dec_n_points = 2;
    int dim_feedforward = 2048;
    bool bbox_reparam = true;
    bool lite_refpoint_refine = true;
    double cls_loss_coef = 1.0;
    double bbox_loss_coef = 5.0;
    double giou_loss_coef = 2.0;
    double mask_ce_loss_coef = 1.0;
    double mask_dice_loss_coef = 1.0;
    bool sum_group_losses = false;
    bool use_varifocal_loss = false;
    bool use_position_supervised_loss = false;
    bool ia_bce_loss = true;
    bool aux_loss = true;
    std::int64_t mask_point_sample_ratio = 16;
    double focal_alpha = 0.25;
    double set_cost_class = 2.0;
    double set_cost_bbox = 5.0;
    double set_cost_giou = 2.0;
    TrainingSupervisionConfig training_supervision;
};
[[nodiscard]] inline bool training_supervision_model_config_valid(const NativeRfDetrConfig& config) noexcept {
    if (!training_supervision_config_valid(config.training_supervision)) return false;
    if (training_supervision_enabled(config.training_supervision) &&
        (config.num_queries < 0 || config.group_detr <= 0 ||
         !training_supervision_query_layout_valid(config.training_supervision, static_cast<std::size_t>(config.num_queries),
                                                  static_cast<std::size_t>(config.group_detr), 0U))) {
        return false;
    }
    if (config.training_supervision.assignment == TrainAssignmentKind::MatchFree) {
        if (config.segmentation || !std::isfinite(config.set_cost_class) || config.set_cost_class < 0.0 || !std::isfinite(config.set_cost_bbox) ||
            config.set_cost_bbox < 0.0 || !std::isfinite(config.set_cost_giou) || config.set_cost_giou < 0.0 ||
            (config.set_cost_class == 0.0 && config.set_cost_bbox == 0.0 && config.set_cost_giou == 0.0)) {
            return false;
        }
    }
    if (training_supervision_enabled(config.training_supervision) &&
        (!std::isfinite(config.focal_alpha) || config.focal_alpha < 0.0 || config.focal_alpha > 1.0)) {
        return false;
    }
    if (config.training_supervision.denoising.enabled &&
        (!std::isfinite(config.cls_loss_coef) || config.cls_loss_coef < 0.0 || !std::isfinite(config.bbox_loss_coef) || config.bbox_loss_coef < 0.0 ||
         !std::isfinite(config.giou_loss_coef) || config.giou_loss_coef < 0.0 ||
         (config.cls_loss_coef == 0.0 && config.bbox_loss_coef == 0.0 && config.giou_loss_coef == 0.0))) {
        return false;
    }
    return true;
}
[[nodiscard]] std::span<const PresetCatalogEntry> model_presets() noexcept;
[[nodiscard]] const PresetCatalogEntry* find_model_preset(std::string_view preset_name) noexcept;
[[nodiscard]] const PresetCatalogEntry* find_model_preset_by_weight_filename(std::string_view filename) noexcept;
[[nodiscard]] const PresetCatalogEntry* infer_model_preset_from_path(const std::filesystem::path& path);
[[nodiscard]] NativeRfDetrConfig native_config_from_preset(const PresetCatalogEntry& preset);
inline std::string infer_train_recipe_preset_name_from_path(const std::filesystem::path& path) {
    if (path.empty()) { return {}; }
    if (const auto* preset = infer_model_preset_from_path(path)) { return std::string(preset->preset_name); }
    return {};
}
}  // namespace mmltk::backend::models::rfdetr
