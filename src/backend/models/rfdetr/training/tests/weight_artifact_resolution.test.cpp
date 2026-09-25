#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <fstream>
#include <string_view>
#include <type_traits>
#include <filesystem>
#include <string>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace rfdetr = mmltk::backend::models::rfdetr;
TEST_CASE("external pretrained import retains filename and path preset resolution", "[model][rfdetr][checkpoint_resolution]") {
 const auto* upstream = rfdetr::infer_model_preset_from_path("/tmp/rf-detr-nano.pth");
 REQUIRE(upstream != nullptr);
 REQUIRE(upstream->preset_name == "rf-detr-nano");
 const std::filesystem::path external = "/tmp/import/output-seg-med/pretrained.pth";
 REQUIRE(rfdetr::infer_model_preset_from_path(external)->preset_name == "rf-detr-seg-medium");
}
TEST_CASE("training rejects ambiguous checkpoint filenames", "[model][rfdetr][checkpoint_resolution]") {
 REQUIRE(rfdetr::find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);
 REQUIRE(rfdetr::infer_model_preset_from_path("/tmp/checkpoint-untyped.pth") == nullptr);
}
TEST_CASE("fresh stock coefficients are independent of interpreted weight metadata", "[model][rfdetr][checkpoint_resolution]") {
 for (const auto& preset : rfdetr::model_presets()) {
  auto artifact = rfdetr::native_config_from_preset(preset);
  artifact.cls_loss_coef = 7.3;
  artifact.bbox_loss_coef = 9.1;
  artifact.mask_ce_loss_coef = 8.2;
  auto fresh = artifact;
  rfdetr::apply_stock_training_coefficients(fresh);
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
TEST_CASE("admitted native state resolves complete metadata without reopening its archive", "[model][rfdetr][checkpoint_resolution]") {
 mmltk::testsupport::ScopedTempDir temp("admitted-model-resolution");
 const auto path = temp.path() / "weights.pt";
 rfdetr::DecodedNativeModelState source;
 source.metadata.preset_name = "rf-detr-nano";
 source.metadata.source_kind = "native-training";
 source.metadata.source_path = "initial.pth";
 source.metadata.num_classes = 2; source.metadata.num_queries = 7; source.metadata.num_select = 3;
 source.metadata.class_layout = rfdetr::testsupport::synthetic_training_layout(1);
 source.metadata.for_each_detection_field([]<class Field>(const char*, Field& field) {
  using Value = typename Field::value_type;
  if constexpr (std::is_same_v<Value, bool>) field = true;
  else if constexpr (std::is_integral_v<Value>) field = 8;
  else field = .75;
 });
 source.replace_entries({{"value", torch::ones({2})}});
 rfdetr::save_native_checkpoint(path, source);
 auto admitted = rfdetr::decode_native_model_state(path);
 const auto proof = admitted.class_artifact;
 admitted.release_admission();
 REQUIRE(admitted.admitted_archive() == nullptr);
 REQUIRE(admitted.entries().empty());
 for (const auto preset : {"", "rf-detr-small"}) {
  const auto resolved = rfdetr::resolve_admitted_model_artifacts(admitted, path, preset, 384);
  CHECK(resolved.class_layout == source.metadata.class_layout);
  CHECK(resolved.input_path == path);
  CHECK(resolved.artifact_sha256 == mmltk::common::io::sha256_hex(proof->file()->sha256));
  CHECK(resolved.config.num_queries == 7);
  CHECK(resolved.config.num_select == 3);
  CHECK(resolved.config.num_classes == 2);
  CHECK(resolved.config.resolution == 384);
  CHECK(resolved.config.preset_name == (std::string_view(preset).empty() ? "rf-detr-nano" : preset));
  const auto projected = rfdetr::make_native_checkpoint_metadata(resolved, 2);
  auto expected = source.metadata;
  expected.preset_name = resolved.config.preset_name;
  CHECK(rfdetr::same_native_model_semantics(projected, expected));
 }
 const auto descriptor = temp.path() / "classes.json";
 {
  std::ofstream file(descriptor);
  file << rfdetr::encode_class_descriptor({1, mmltk::common::io::sha256_hex(proof->file()->sha256), source.metadata.class_layout});
 }
 CHECK_NOTHROW(rfdetr::resolve_admitted_model_artifacts(admitted, path, {}, 0, descriptor));
 std::ofstream(descriptor) << "invalid descriptor";
 REQUIRE_THROWS(rfdetr::resolve_admitted_model_artifacts(admitted, path, {}, 0, descriptor));
 REQUIRE_THROWS(rfdetr::resolve_admitted_model_artifacts(admitted, path, "unknown-preset", 0));
 const auto replacement = temp.path() / "replacement.pt";
 std::filesystem::copy_file(path, replacement);
 std::filesystem::rename(replacement, path);
 REQUIRE_THROWS(rfdetr::resolve_admitted_model_artifacts(admitted, path, {}, 0));
}
