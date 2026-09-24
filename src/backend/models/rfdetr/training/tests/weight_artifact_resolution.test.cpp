#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/training/detail/training_continuation.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
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

TEST_CASE("fresh stock coefficients are independent of interpreted weight metadata", "[model][rfdetr][checkpoint_resolution]") {
 for (const auto& preset : rfdetr::model_presets()) {
  auto artifact = rfdetr::native_config_from_preset(preset);
  artifact.cls_loss_coef = 7.3; artifact.bbox_loss_coef = 9.1; artifact.mask_ce_loss_coef = 8.2;
  auto fresh = artifact;
  REQUIRE_FALSE(rfdetr::detail::admit_training_configuration(fresh,nullptr,rfdetr::TrainRequest{}).has_value());
  REQUIRE(fresh.cls_loss_coef == 1.0);
  REQUIRE(fresh.bbox_loss_coef == 5.0);
  REQUIRE(fresh.giou_loss_coef == 2.0);
  REQUIRE(fresh.mask_ce_loss_coef == (fresh.segmentation ? 5.0 : 1.0));
  REQUIRE(fresh.mask_dice_loss_coef == (fresh.segmentation ? 5.0 : 1.0));
  REQUIRE(artifact.cls_loss_coef == 7.3);
  REQUIRE(artifact.bbox_loss_coef == 9.1);
  REQUIRE(artifact.mask_ce_loss_coef == 8.2);
  REQUIRE(fresh.num_queries == artifact.num_queries);
  REQUIRE(fresh.resolution == artifact.resolution);
 }
}
