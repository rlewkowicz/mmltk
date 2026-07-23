#pragma once

#include <filesystem>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

enum class CompilationMode : std::uint8_t {
    kNone,
    kSelective,
    kFullTrace,
};

struct ModelPresetConfig {
    std::string_view preset_name;
    std::string_view encoder;
    std::string_view canonical_weight_filename;
    int resolution = 0;
    int patch_size = 16;
    int num_windows = 2;
    int positional_encoding_size = 24;
    int dec_layers = 0;
    int num_queries = 0;
    int num_select = 0;
    int num_classes = 91;
    int hidden_dim = 256;
    int group_detr = 13;
    bool two_stage = true;
    bool segmentation_head = false;
    double cls_loss_coef = 1.0;
    double bbox_loss_coef = 5.0;
    double giou_loss_coef = 2.0;
    double mask_ce_loss_coef = 1.0;
    double mask_dice_loss_coef = 1.0;
};

const std::vector<ModelPresetConfig>& model_presets();
const ModelPresetConfig* find_model_preset(std::string_view preset_name);
const ModelPresetConfig* find_model_preset_by_weight_filename(std::string_view filename);
const ModelPresetConfig* infer_model_preset_from_path(const std::filesystem::path& path);

}  
