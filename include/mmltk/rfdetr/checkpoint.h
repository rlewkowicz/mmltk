#pragma once

#include "mmltk/rfdetr/artifacts.h"

#include <torch/torch.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

inline constexpr const char* kNativeCheckpointFormat = "mmltk.rfdetr.native_checkpoint";
inline constexpr int64_t kNativeCheckpointFormatVersion = 1;

struct StateDictEntry {
    std::string name;
    torch::Tensor tensor;
};

struct NativeCheckpointMetadata {
    std::string preset_name;
    std::string source_kind;
    std::string source_path;
    int64_t num_classes = 0;
    std::optional<bool> sum_group_losses;
    std::optional<bool> use_varifocal_loss;
    std::optional<bool> use_position_supervised_loss;
    std::optional<bool> ia_bce_loss;
    std::optional<bool> aux_loss;
    std::optional<int64_t> mask_point_sample_ratio;
    std::optional<double> focal_alpha;
    std::optional<double> cls_loss_coef;
    std::optional<double> bbox_loss_coef;
    std::optional<double> giou_loss_coef;
    std::optional<double> mask_ce_loss_coef;
    std::optional<double> mask_dice_loss_coef;
    std::optional<double> set_cost_class;
    std::optional<double> set_cost_bbox;
    std::optional<double> set_cost_giou;
};

struct NativeCheckpoint {
    NativeCheckpointMetadata metadata;
    std::vector<StateDictEntry> state_dict;
};

struct StateDictLoadSummary {
    std::vector<std::string> loaded_names;
    std::vector<std::string> missing_names;
    std::vector<std::string> unexpected_names;
    std::vector<std::string> incompatible_names;
};

bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path);
NativeCheckpoint load_checkpoint(const std::filesystem::path& checkpoint_path);
StateDictLoadSummary apply_checkpoint_to_module(torch::nn::Module& module, const NativeCheckpoint& checkpoint,
                                                bool strict = true);
StateDictLoadSummary apply_checkpoint_to_module(torch::nn::Module& module, const std::filesystem::path& checkpoint_path,
                                                bool strict = true);
void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const NativeCheckpoint& checkpoint);
NativeCheckpoint normalize_checkpoint_to_native(const std::filesystem::path& input_path,
                                                const std::filesystem::path& output_path);

}  // namespace mmltk::rfdetr
