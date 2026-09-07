#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/training/train_recipe.h"

import mmltk.backend.models.rfdetr.training.checkpoint;

namespace rfdetr = mmltk::backend::models::rfdetr;

TEST_CASE("training recipe resolves upstream and legacy checkpoint names", "[model][rfdetr][checkpoint_resolution]") {
    const auto* upstream = rfdetr::infer_model_preset_from_path("/tmp/rf-detr-nano.pth");
    REQUIRE(upstream != nullptr);
    REQUIRE(upstream->preset_name == "rf-detr-nano");

    const std::filesystem::path legacy = "/tmp/engines/output-seg-med/1train/checkpoint.pt";
    REQUIRE(rfdetr::infer_train_recipe_preset_name_from_path(legacy) == "rf-detr-seg-medium");
}

TEST_CASE("training rejects ambiguous checkpoint filenames", "[model][rfdetr][checkpoint_resolution]") {
    REQUIRE(rfdetr::find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);
    REQUIRE(rfdetr::infer_train_recipe_preset_name_from_path("/tmp/checkpoint-untyped.pth").empty());
}
