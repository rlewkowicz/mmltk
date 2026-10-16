#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <fstream>
#include "training_continuation_fixture.h"
#include <string_view>
#include <type_traits>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/tests/class_artifact_fixture.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <limits>
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
  REQUIRE(fresh.cls_loss_coef == (fresh.has_keypoints() ? 2.0 : 1.0));
  REQUIRE(fresh.bbox_loss_coef == 5.0);
  REQUIRE(fresh.giou_loss_coef == 2.0);
  REQUIRE(fresh.mask_ce_loss_coef == (fresh.has_masks() ? 5.0 : 1.0));
  REQUIRE(fresh.mask_dice_loss_coef == (fresh.has_masks() ? 5.0 : 1.0));
  REQUIRE(fresh.keypoint_l1_loss_coef == (fresh.has_keypoints() ? 1.0 : 0.0));
  REQUIRE(fresh.keypoint_findable_loss_coef == (fresh.has_keypoints() ? 1.0 : 0.0));
  REQUIRE(fresh.keypoint_visible_loss_coef == (fresh.has_keypoints() ? 1.0 : 0.0));
  REQUIRE(fresh.keypoint_nll_loss_coef == (fresh.has_keypoints() ? 1.0 : 0.0));
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
 source.metadata.num_classes = 2;
 source.metadata.num_queries = 7;
 source.metadata.num_select = 3;
 source.metadata.class_layout = rfdetr::testsupport::synthetic_training_layout(1);
 source.metadata.for_each_detection_field([]<class Field>(const char*, Field& field) {
  using Value = typename Field::value_type;
  if constexpr (std::is_same_v<Value, bool>)
   field = true;
  else if constexpr (std::is_integral_v<Value>)
   field = 8;
  else
   field = .75;
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
  expected.native_configuration = rfdetr::encode_checkpoint_configuration(resolved.config);
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
TEST_CASE("Native configuration preserves admitted long custom names within its checkpoint envelope", "[model][rfdetr][checkpoint_resolution][boundary]") {
 rfdetr::test_support::with_configuration_envelope([&](const rfdetr::NativeRfDetrConfig& config) {
  const auto encoded = rfdetr::encode_checkpoint_configuration(config);
  REQUIRE(encoded.size() <= 65536);
  const auto decoded = rfdetr::decode_checkpoint_configuration(encoded);
  REQUIRE(decoded.preset_name == config.preset_name);
  CHECK(rfdetr::encode_checkpoint_configuration(decoded) == encoded);
  mmltk::testsupport::ScopedTempDir root("long-custom-configuration");
  const auto path = root.path() / "weights.pt";
  rfdetr::DecodedNativeModelState source;
  source.metadata.preset_name = config.preset_name;
  source.metadata.native_configuration = encoded;
  source.metadata.num_classes = config.num_classes;
  source.metadata.class_layout = rfdetr::unresolved_class_layout(config.num_classes);
  source.metadata.num_queries = config.num_queries;
  source.metadata.num_select = config.num_select;
  std::vector<rfdetr::NormalizedModelStateEntry> entries{{"value", torch::ones({2})}};
  if (config.has_keypoints()) {
   const rfdetr::KeypointTensorLayout joints(config.keypoints_per_class);
   entries.push_back({"_kp_active_mask", joints.active_mask()});
   entries.push_back({"transformer.decoder.keypoint_class_mask", joints.class_mask()});
  }
  source.replace_entries(std::move(entries));
  rfdetr::save_native_checkpoint(path, source);
  const auto admitted = rfdetr::decode_native_model_state(path);
  const auto resolved = rfdetr::resolve_admitted_model_artifacts(admitted, path, {}, 0);
  CHECK(resolved.config.preset_name == config.preset_name);
  CHECK(rfdetr::encode_checkpoint_configuration(resolved.config) == encoded);
 });
}
TEST_CASE("native encoder lineage survives archive normalization and ordinary artifact inspection", "[model][rfdetr][checkpoint_resolution][encoder_pretraining]") {
 namespace r = rfdetr;
 const mmltk::testsupport::ScopedTempDir root("encoder-lineage");
 r::DecodedNativeModelState state;
 state.metadata = r::testsupport::synthetic_training_metadata();
 // Serialization fixture identity deliberately claims no official source asset.
 state.metadata.encoder_pretraining = r::EncoderPretrainingProvenance{"serialization.fixture", std::string(40, 'a'), std::string(64, 'b'), 7};
 state.replace_entries({{"value", torch::ones({2})}});
 const auto source = root.path() / "source.pt";
 r::save_native_checkpoint(source, state);
 const auto decoded = r::decode_native_model_state(source);
 CHECK(decoded.metadata.encoder_pretraining == state.metadata.encoder_pretraining);
 const auto artifacts = r::resolve_model_artifacts(source, {}, 0);
 CHECK(artifacts.encoder_pretraining == state.metadata.encoder_pretraining);
 CHECK(r::make_native_checkpoint_metadata(artifacts, 2).encoder_pretraining == state.metadata.encoder_pretraining);
 const auto inspected = r::inspect_training_checkpoint(source).checkpoint();
 CHECK(inspected.encoder_pretraining == state.metadata.encoder_pretraining);
 CHECK_FALSE(inspected.resumable);
 const auto normalized = r::normalize_checkpoint_to_native(source, root.path() / "normalized.pt", {});
 CHECK(normalized.metadata.encoder_pretraining == state.metadata.encoder_pretraining);
 constexpr auto capacity = mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<r::EncoderPretrainingProvenance>();
 {
  torch::Tensor original;
  decoded.admitted_archive()->read("encoder_pretraining_cbor", original);
  auto strided = r::testsupport::strided_checkpoint_bytes(original);
  REQUIRE_FALSE(strided.is_contiguous());
  torch::serialize::OutputArchive output;
  r::testsupport::copy_checkpoint_archive(*decoded.admitted_archive(), output, "encoder_pretraining_cbor");
  output.write("encoder_pretraining_cbor", strided);
  const auto path = root.path() / "strided.pt";
  output.save_to(path.string());
  const auto restored = r::decode_native_model_state(path);
  restored.admitted_archive()->read("encoder_pretraining_cbor", original);
  REQUIRE_FALSE(original.is_contiguous());
  CHECK(restored.metadata.encoder_pretraining == state.metadata.encoder_pretraining);
 }
 {
  auto maximum = state.metadata.encoder_pretraining;
  maximum->asset_id.assign(mmltk::frameworks::reflection::kMaximumNameBytes, 'a');
  maximum->adaptation_revision = std::numeric_limits<std::uint32_t>::max();
  const auto original = state.metadata.encoder_pretraining;
  state.metadata.encoder_pretraining = maximum;
  const auto path = root.path() / "maximum.pt";
  r::save_native_checkpoint(path, state);
  const auto restored = r::decode_native_model_state(path);
  torch::Tensor tensor;
  restored.admitted_archive()->read("encoder_pretraining_cbor", tensor);
  CHECK(static_cast<std::size_t>(tensor.numel()) == capacity);
  CHECK(restored.metadata.encoder_pretraining == maximum);
  state.metadata.encoder_pretraining = original;
 }
 auto invalid = state.metadata.encoder_pretraining;
 invalid->sha256 = std::string(64, 'Z');
 state.metadata.encoder_pretraining = invalid;
 REQUIRE_THROWS(r::save_native_checkpoint(root.path() / "invalid.pt", state));
 // Present malformed data cannot become a missing optional record.
 for (int kind = 0; kind < 6; ++kind) {
  CAPTURE(kind);
  torch::serialize::OutputArchive output;
  r::testsupport::copy_checkpoint_archive(*decoded.admitted_archive(), output, "encoder_pretraining_cbor");
  if (kind == 0) output.write("encoder_pretraining_cbor", c10::IValue("bad"));
  if (kind == 1) output.write("encoder_pretraining_cbor", torch::zeros({2, 2}, torch::kUInt8));
  if (kind == 2) output.write("encoder_pretraining_cbor", torch::zeros({static_cast<int64_t>(capacity + 1)}, torch::kUInt8));
  if (kind == 3) output.write("encoder_pretraining_cbor", torch::zeros({static_cast<int64_t>(capacity)}, torch::kUInt8));
  if (kind == 4) output.write("encoder_pretraining_cbor", torch::empty({0}, torch::kUInt8));
  if (kind == 5) output.write("encoder_pretraining_cbor", torch::zeros({1}, torch::kInt64));
  const auto path = root.path() / ("malformed-" + std::to_string(kind) + ".pt");
  output.save_to(path.string());
  REQUIRE_THROWS_WITH(r::decode_native_model_state(path), kind == 0   ? "invalid encoder pretraining CBOR metadata"
                                                          : kind == 3 ? "invalid encoder pretraining provenance"
                                                                      : "invalid encoder pretraining CBOR extent");
 }
 state.metadata.encoder_pretraining.reset();
 r::save_native_checkpoint(root.path() / "legacy.pt", state);
 CHECK_FALSE(r::decode_native_model_state(root.path() / "legacy.pt").metadata.encoder_pretraining);
}
