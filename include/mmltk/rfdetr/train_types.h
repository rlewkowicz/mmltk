#pragma once

#include <cmath>
#include <cstdint>

namespace mmltk::rfdetr {

enum class TrainOptimizerKind : std::uint8_t {
    AdamW = 0,
    Muon = 1,
};

struct AugmentationGroupConfig {
    float probability = 0.0F;
    float min_strength = 0.0F;
    float max_strength = 0.0F;
};

struct GpuAugmentationConfig {
    bool enabled = false;
    AugmentationGroupConfig geometry{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig resize{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig color{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig noise{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig blur{0.50F, 0.05F, 0.50F};
    AugmentationGroupConfig occlusion{0.50F, 0.05F, 0.50F};
    float copy_paste_probability = 0.50F;
};

[[nodiscard]] inline bool valid_augmentation_group(const AugmentationGroupConfig& group) noexcept {
    return std::isfinite(group.probability) && std::isfinite(group.min_strength) && std::isfinite(group.max_strength) &&
           group.probability >= 0.0F && group.probability <= 1.0F && group.min_strength >= 0.0F &&
           group.min_strength <= group.max_strength && group.max_strength <= 1.0F;
}

[[nodiscard]] inline bool valid_gpu_augmentation_config(const GpuAugmentationConfig& config) noexcept {
    return valid_augmentation_group(config.geometry) && valid_augmentation_group(config.resize) &&
           valid_augmentation_group(config.color) && valid_augmentation_group(config.noise) &&
           valid_augmentation_group(config.blur) && valid_augmentation_group(config.occlusion) &&
           std::isfinite(config.copy_paste_probability) && config.copy_paste_probability >= 0.0F &&
           config.copy_paste_probability <= 1.0F;
}

struct TrainHyperparameterConfig {
    double lr = 1.0e-4;
    double lr_encoder = 1.5e-4;
    double lr_component_decay = 0.7;
    double encoder_layer_decay = 0.8;
    double momentum = 0.95;
    double weight_decay = 1.0e-4;
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    double lr_min_factor = 0.0;
};

}  
