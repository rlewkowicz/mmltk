#pragma once
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace mmltk::backend::models::rfdetr {
using TrainRecipeConfig = TrainRecipeCatalogEntry;
inline TrainRecipeConfig default_train_recipe(const TrainOptimizerKind optimizer) { return train_recipe(optimizer); }
inline TrainRecipeConfig resolve_train_recipe([[maybe_unused]] std::string_view preset_name, const TrainOptimizerKind optimizer) {
    return default_train_recipe(optimizer);
}
inline void apply_train_recipe(TrainRequest& target, const TrainRecipeConfig& recipe, const TrainRecipeOverrideState& overrides = {}) {
    using Relation = mmltk::frameworks::reflection::catalog_provider_relation<TrainRecipeCatalog>;
    Relation::VisitMembers([&]<class Entry>() {
        if (!Relation::template overridden<Entry::destination>(overrides)) {
            Entry::transform::apply(mmltk::frameworks::reflection::access<TrainRequest, Entry::destination>(target),
                                    mmltk::frameworks::reflection::access<const TrainRecipeConfig, Entry::source>(recipe));
        }
    });
}
inline std::string infer_train_recipe_preset_name_from_path(const std::filesystem::path& path) {
    if (path.empty()) { return {}; }
    if (const auto* preset = infer_model_preset_from_path(path)) { return std::string(preset->preset_name); }
    return {};
}
inline bool train_recipe_value_matches(double lhs, double rhs, double eps = 1.0e-12) { return std::abs(lhs - rhs) <= eps; }
}  // namespace mmltk::backend::models::rfdetr
