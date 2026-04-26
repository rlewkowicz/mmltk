#pragma once

#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/train_types.h"

#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

struct TrainRecipeConfig : TrainHyperparameterConfig {
    int lr_drop = 100;
    std::string_view lr_scheduler = "step";
};

struct TrainRecipeFieldOverrides {
    bool lr = false;
    bool lr_encoder = false;
    bool lr_component_decay = false;
    bool encoder_layer_decay = false;
    bool momentum = false;
    bool weight_decay = false;
    bool warmup_epochs = false;
    bool warmup_momentum = false;
    bool lr_min_factor = false;
    bool lr_drop = false;
    bool lr_scheduler = false;
};

struct TrainRecipePresetConfig {
    std::string_view preset_name;
    TrainRecipeConfig adamw;
    TrainRecipeConfig muon;
};

constexpr TrainRecipeConfig kAdamWTrainRecipe{
    1.0e-4, 1.5e-4, 0.7, 0.8, 0.95, 1.0e-4, 0.0, 0.0, 0.0, 100, "step",
};

constexpr TrainRecipeConfig kMuonTrainRecipe{
    2.0e-4, 3.0e-4, 0.7, 0.8, 0.9, 5.0e-4, 3.0, 0.8, 0.01, 100, "cosine",
};

inline const auto& train_recipe_presets() {
    static constexpr std::array<TrainRecipePresetConfig, 10> kPresets{{
        {"rf-detr-nano", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-small", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-medium", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-large", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-nano", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-small", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-medium", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-large", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-xlarge", kAdamWTrainRecipe, kMuonTrainRecipe},
        {"rf-detr-seg-xxlarge", kAdamWTrainRecipe, kMuonTrainRecipe},
    }};
    return kPresets;
}

inline const TrainRecipePresetConfig* find_train_recipe_preset(std::string_view preset_name) {
    for (const auto& preset : train_recipe_presets()) {
        if (preset.preset_name == preset_name) {
            return &preset;
        }
    }
    return nullptr;
}

inline TrainRecipeConfig default_train_recipe(TrainOptimizerKind optimizer) {
    return optimizer == TrainOptimizerKind::Muon ? kMuonTrainRecipe : kAdamWTrainRecipe;
}

inline TrainRecipeConfig resolve_train_recipe(std::string_view preset_name, TrainOptimizerKind optimizer) {
    if (const auto* preset = find_train_recipe_preset(preset_name)) {
        return optimizer == TrainOptimizerKind::Muon ? preset->muon : preset->adamw;
    }
    return default_train_recipe(optimizer);
}

template <typename Target>
inline void apply_train_recipe(Target& target, const TrainRecipeConfig& recipe,
                               const TrainRecipeFieldOverrides& overrides = {}) {
    if (!overrides.lr) {
        target.lr = recipe.lr;
    }
    if (!overrides.lr_encoder) {
        target.lr_encoder = recipe.lr_encoder;
    }
    if (!overrides.lr_component_decay) {
        target.lr_component_decay = recipe.lr_component_decay;
    }
    if (!overrides.encoder_layer_decay) {
        target.encoder_layer_decay = recipe.encoder_layer_decay;
    }
    if (!overrides.momentum) {
        target.momentum = recipe.momentum;
    }
    if (!overrides.weight_decay) {
        target.weight_decay = recipe.weight_decay;
    }
    if (!overrides.warmup_epochs) {
        target.warmup_epochs = recipe.warmup_epochs;
    }
    if (!overrides.warmup_momentum) {
        target.warmup_momentum = recipe.warmup_momentum;
    }
    if (!overrides.lr_min_factor) {
        target.lr_min_factor = recipe.lr_min_factor;
    }
    if (!overrides.lr_drop) {
        target.lr_drop = recipe.lr_drop;
    }
    if (!overrides.lr_scheduler) {
        target.lr_scheduler = std::string(recipe.lr_scheduler);
    }
}

inline std::string infer_train_recipe_preset_name_from_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    if (const auto* preset = infer_model_preset_from_path(path)) {
        return std::string(preset->preset_name);
    }
    return {};
}

inline bool train_recipe_value_matches(double lhs, double rhs, double eps = 1.0e-12) {
    return std::abs(lhs - rhs) <= eps;
}

}  // namespace mmltk::rfdetr
