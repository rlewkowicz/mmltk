#include "src/backend/models/rfdetr/contract/weight_catalog.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "src/backend/models/rfdetr/contract/cli.h"
#include "src/backend/models/rfdetr/contract/contract.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/encoder_pretraining.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
namespace {
constexpr std::array kRetiredWeightFilenames{
 "rf-detr-base.pth",
 "rf-detr-base-o365.pth",
 "rf-detr-base-2.pth",
 "rf-detr-large.pth",
 "rf-detr-seg-preview.pt",
};
constexpr std::array kRetiredPresetNames{
 "rf-detr-base",
 "rf-detr-base-o365",
 "rf-detr-base-2",
 "rf-detr-large-deprecated",
 "rf-detr-seg-preview",
};
TEST_CASE("RF-DETR weight assets derive from the preset authority", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 REQUIRE(weight_catalog().size() == 16U);
 const auto* seg_medium = find_weight_asset("rf-detr-seg-medium.pt");
 REQUIRE(seg_medium != nullptr);
 REQUIRE(seg_medium->filename == std::string_view{"rf-detr-seg-medium.pt"});
 REQUIRE(seg_medium->download_url == std::string_view{"https://repo.roboflow.com/rfdetr/rf-detr-seg-m-ft.pth"});
 REQUIRE(seg_medium->md5_hash == std::string_view{"a49af1562c3719227ad43d0ca53b4c7a"});
 const auto* nano = find_weight_asset("rf-detr-nano.pth");
 REQUIRE(nano != nullptr);
 REQUIRE(nano->download_url == std::string_view{"https://repo.roboflow.com/rfdetr/nano_coco/"
                                                "checkpoint_best_regular.pth"});
 for (const auto* filename : kRetiredWeightFilenames) {
  REQUIRE(find_weight_asset(filename) == nullptr);
  REQUIRE_FALSE(is_registered_weight_asset(filename));
 }
}
TEST_CASE("RF-DETR weight path lookup preserves membership semantics", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 const auto resolved = resolve_weight_asset_for_path(std::string{"/tmp/models/rf-detr-seg-small.pt"});
 REQUIRE(resolved.has_value());
 REQUIRE(resolved->filename == std::string_view{"rf-detr-seg-small.pt"});
 REQUIRE(is_registered_weight_asset("rf-detr-seg-small.pt"));
 REQUIRE_FALSE(resolve_weight_asset_for_path(std::string{"/tmp/models/rf-detr-large.pth"}).has_value());
 REQUIRE_FALSE(is_registered_weight_asset("missing.pt"));
 REQUIRE(find_weight_asset("missing.pt") == nullptr);
}
TEST_CASE("RF-DETR preset projection preserves every architecture field", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 REQUIRE(kRfdetrContract.model_id == std::string_view{"rfdetr"});
 REQUIRE(kRfdetrContract.presets.size() == 16U);
 REQUIRE(model_presets().size() == kRfdetrContract.presets.size());
 const auto* preset = find_preset_catalog_entry("rf-detr-seg-medium");
 REQUIRE(preset != nullptr);
 REQUIRE(preset == &kPresetCatalog[6U]);
 REQUIRE(preset->preset_name == "rf-detr-seg-medium");
 REQUIRE(backbone_encoder_name(preset->backbone.kind) == "dinov2_windowed_small");
 REQUIRE(preset->canonical_weight_filename == "rf-detr-seg-medium.pt");
 REQUIRE(preset->resolution == 432);
 REQUIRE(preset->patch_size == 12);
 REQUIRE(preset->window_count == 2);
 REQUIRE(preset->positional_encoding_size == 36);
 REQUIRE(preset->decoder_layer_count == 5);
 REQUIRE(preset->query_count == 200);
 REQUIRE(preset->selected_query_count == 200);
 REQUIRE(preset->class_count == 91);
 REQUIRE(preset->hidden_dimension == 256);
 REQUIRE(preset->group_count == 13);
 REQUIRE(preset->two_stage);
 REQUIRE(preset->task == ModelTask::Segmentation);
 REQUIRE(preset->classification_loss_coefficient == 1.0);
 REQUIRE(preset->bounding_box_loss_coefficient == 5.0);
 REQUIRE(preset->generalized_iou_loss_coefficient == 2.0);
 REQUIRE(preset->mask_cross_entropy_loss_coefficient == 5.0);
 REQUIRE(preset->mask_dice_loss_coefficient == 5.0);
 const auto* large = find_model_preset_by_weight_filename("rf-detr-large-2026.pth");
 REQUIRE(large != nullptr);
 REQUIRE(large->preset_name == std::string_view{"rf-detr-large"});
 REQUIRE(large->patch_size == 16);
 REQUIRE(large->window_count == 2);
 REQUIRE(large->positional_encoding_size == 44);
 REQUIRE(large->task == ModelTask::Detection);
 const auto* seg_nano = find_preset_catalog_entry("rf-detr-seg-nano");
 REQUIRE(seg_nano != nullptr);
 REQUIRE(seg_nano->patch_size == 12);
 REQUIRE(seg_nano->window_count == 1);
 REQUIRE(seg_nano->positional_encoding_size == 26);
}
TEST_CASE("RF-DETR preset lookup rejects retired ambiguous and unknown names", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 const auto* upstream = find_model_preset_by_weight_filename("rf-detr-seg-m-ft.pth");
 REQUIRE(upstream != nullptr);
 REQUIRE(upstream->preset_name == std::string_view{"rf-detr-seg-medium"});
 const auto* legacy = infer_model_preset_from_path(std::filesystem::path{"/tmp/engines/output-seg-med/1train/checkpoint.pt"});
 REQUIRE(legacy != nullptr);
 REQUIRE(legacy->preset_name == std::string_view{"rf-detr-seg-medium"});
 for (const auto* preset_name : kRetiredPresetNames) REQUIRE(find_preset_catalog_entry(preset_name) == nullptr);
 for (const auto* filename : kRetiredWeightFilenames) REQUIRE(find_model_preset_by_weight_filename(filename) == nullptr);
 REQUIRE(find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);
 REQUIRE(find_preset_catalog_entry("unknown-preset") == nullptr);
 REQUIRE(find_preset_catalog_entry("") == nullptr);
 REQUIRE(find_preset_catalog_entry("RF-DETR-NANO") == nullptr);
 for (const auto& entry : kPresetCatalog) REQUIRE(find_preset_catalog_entry(entry.preset_name) == &entry);
 REQUIRE(find_model_preset_by_weight_filename("unknown-file.pth") == nullptr);
 REQUIRE(infer_model_preset_from_path(std::filesystem::path{"/tmp/unrelated.bin"}) == nullptr);
}
TEST_CASE("RF-DETR command spellings resolve to one typed vocabulary", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 REQUIRE(parse_rfdetr_command("evaluate") == RfdetrCommand::Evaluate);
 REQUIRE(parse_rfdetr_command("eval") == RfdetrCommand::Evaluate);
 REQUIRE(parse_rfdetr_command("val") == RfdetrCommand::Evaluate);
 REQUIRE(parse_rfdetr_command("train") == RfdetrCommand::Train);
 REQUIRE_FALSE(parse_rfdetr_command("unknown").has_value());
 const auto* descriptor = rfdetr_command_descriptor(RfdetrCommand::NormalizeWeights);
 REQUIRE(descriptor != nullptr);
 REQUIRE(descriptor->name == std::string_view{"normalize-weights"});
 REQUIRE(rfdetr_command_descriptor(static_cast<RfdetrCommand>(std::numeric_limits<std::uint8_t>::max())) == nullptr);
}
TEST_CASE("RF-DETR enum projections make unknown values observable", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 REQUIRE(model_task_name(ModelTask::Detection) == std::string_view{"detection"});
 REQUIRE(model_task_name(ModelTask::Segmentation) == std::string_view{"segmentation"});
 REQUIRE(model_task_name(ModelTask::Keypoints) == std::string_view{"keypoints"});
 REQUIRE_FALSE(model_task_name(static_cast<ModelTask>(std::numeric_limits<std::uint8_t>::max())).has_value());
}
TEST_CASE("RF-DETR reflected scalar policy precedes relationship admission", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 TrainRequest request;
 request.train_compiled_path = "/tmp/train.mmltk";
 request.val_compiled_path = "/tmp/val.mmltk";
 request.weights_path = "/tmp/weights.pt";
 request.output_dir = "/tmp/output";
 request.gpu_augmentation.geometry.probability = std::numeric_limits<float>::quiet_NaN();
 REQUIRE_THROWS_AS(validate_train_request(request), std::runtime_error);
 request.gpu_augmentation.geometry.probability = 0.5F;
 request.gpu_augmentation.geometry.min_strength = 0.75F;
 request.gpu_augmentation.geometry.max_strength = 0.25F;
 REQUIRE_THROWS_AS(validate_train_request(request), std::runtime_error);
}
}  // namespace
TEST_CASE("RF-DETR complete catalog projects task-aware topology and stock coefficients", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 for (const auto& row : kPresetCatalog) {
  const auto config = native_config_from_preset(row);
  INFO(row.preset_name);
  CHECK(native_model_config_valid(config));
  CHECK(config.task == row.task);
  CHECK(config.backbone == row.backbone);
  CHECK(config.sa_nheads == row.self_attention_heads);
  CHECK(config.ca_nheads == row.cross_attention_heads);
  CHECK(config.dim_feedforward == row.feedforward_dimension);
  CHECK(config.dec_n_points == row.sampling_points);
  CHECK(config.has_masks() == (row.task == ModelTask::Segmentation));
  CHECK(config.has_keypoints() == (row.task == ModelTask::Keypoints));
  const auto* asset = find_weight_asset(row.canonical_weight_filename);
  REQUIRE(asset);
  CHECK(asset->classifier_width == row.class_count);
  CHECK(asset->class_layout == row.official_class_layout);
 }
 auto atto = native_config_from_preset(*find_preset_catalog_entry("rf-detr-atto"));
 CHECK(atto.dec_layers == 0);
 CHECK(atto.num_queries == 300);
 CHECK(atto.hidden_dim == 128);
 atto.two_stage = false;
 CHECK_FALSE(native_model_config_valid(atto));
 auto pose = native_config_from_preset(*find_preset_catalog_entry("rf-detr-keypoint-preview"));
 CHECK(pose.keypoints_per_class == std::vector<std::uint16_t>{0U, 17U});
 CHECK(find_preset_catalog_entry("rf-detr-keypoint-preview")->official_class_layout == OfficialClassLayout::PersonBackgroundFirst);
 CHECK(pose.cls_loss_coef == 2.0);
 CHECK(pose.keypoint_l1_loss_coef == 1.0);
 CHECK(pose.keypoint_findable_loss_coef == 1.0);
 CHECK(pose.keypoint_visible_loss_coef == 1.0);
 CHECK(pose.keypoint_nll_loss_coef == 1.0);
 pose.keypoint_head = KeypointHeadKind::None;
 CHECK_FALSE(native_model_config_valid(pose));
 CHECK(find_preset_catalog_entry("rf-detr-2xlarge") == find_preset_catalog_entry("rf-detr-xxlarge"));
 CHECK(find_preset_catalog_entry("rf-detr-seg-2xlarge") == find_preset_catalog_entry("rf-detr-seg-xxlarge"));
 CHECK(infer_model_preset_from_path("/models/rf-detr-2xlarge.pth") == find_preset_catalog_entry("rf-detr-xxlarge"));
 CHECK(infer_model_preset_from_path("/models/rf-detr-seg-2xlarge.pt") == find_preset_catalog_entry("rf-detr-seg-xxlarge"));
}
TEST_CASE("RF-DETR configuration admission validates backbone patch grids", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 auto nano = native_config_from_preset(*find_preset_catalog_entry("rf-detr-nano"));
 nano.resolution = 385;
 CHECK(native_model_config_valid(nano));
 nano.resolution = 400;
 CHECK_FALSE(native_model_config_valid(nano));
 nano.resolution = 15;
 CHECK_FALSE(native_model_config_valid(nano));
 auto xlarge = native_config_from_preset(*find_preset_catalog_entry("rf-detr-xlarge"));
 xlarge.resolution = 701;
 CHECK(native_model_config_valid(xlarge));
 auto atto = native_config_from_preset(*find_preset_catalog_entry("rf-detr-atto"));
 CHECK(native_model_config_valid(atto));
 atto.resolution = 381;
 CHECK_FALSE(native_model_config_valid(atto));
}
TEST_CASE("RF-DETR supervision capabilities do not admit malformed topology", "[backend][models][rfdetr][contract]") {
 using namespace mmltk::backend::models::rfdetr;
 auto config = native_config_from_preset(*find_preset_catalog_entry("rf-detr-nano"));
 config.training_supervision.assignment = TrainAssignmentKind::MatchFree;
 config.training_supervision.denoising.enabled = true;
 SECTION("negative decoder depth") { config.dec_layers = -1; }
 SECTION("unknown task") { config.task = static_cast<ModelTask>(std::numeric_limits<std::uint8_t>::max()); }
 SECTION("zero backbone attention heads") { config.backbone.heads = 0U; }
 SECTION("repeated backbone features") { config.backbone.features[1U] = config.backbone.features[0U]; }
 SECTION("keypoint head for detection") { config.keypoint_head = KeypointHeadKind::GroupPose; }
 CHECK(training_supervision_model_config_valid(config));
 CHECK_FALSE(native_model_config_valid(config));
}
TEST_CASE("base encoder assets cover every backbone without becoming detector weights", "[backend][models][rfdetr][contract][encoder_pretraining]") {
 using namespace mmltk::backend::models::rfdetr;
 REQUIRE(kEncoderPretrainingAssets.size() == 3U);
 CHECK(encoder_pretraining_catalog_is_valid());
 for (const auto& preset : kPresetCatalog) {
  INFO(preset.preset_name);
  const auto* asset = find_encoder_pretraining_asset(preset.backbone.kind);
  REQUIRE(asset != nullptr);
  CHECK(asset->geometry.dimension == preset.backbone.dimension);
  CHECK(asset->geometry.feedforward_dimension == preset.backbone.feedforward_dimension);
  CHECK(asset->geometry.heads == preset.backbone.heads);
  CHECK(asset->geometry.layers == preset.backbone.layers);
  CHECK(asset->geometry.register_tokens == 0U);
  CHECK(find_weight_asset(asset->filename) == nullptr);
  CHECK(asset->download_url != preset.canonical_weight_url);
  const bool pe = preset.backbone.kind == BackboneKind::PeCoreTiny;
  CHECK(asset->geometry.patch_size == (pe ? 16U : 14U));
  CHECK(asset->geometry.position_grid == (pe ? 24U : 37U));
  CHECK(asset->geometry.mask_token == !pe);
  CHECK(encoder_pretraining_provenance_valid(encoder_pretraining_provenance(*asset)));
 }
 for (std::size_t index = 0; index < kEncoderPretrainingAssets.size(); ++index) {
  const auto& asset = kEncoderPretrainingAssets[index];
  CHECK(asset.filename == "model.safetensors");
  for (std::size_t sibling = index + 1U; sibling < kEncoderPretrainingAssets.size(); ++sibling) {
   CHECK(asset.source_repository != kEncoderPretrainingAssets[sibling].source_repository);
   CHECK(asset.sha256 != kEncoderPretrainingAssets[sibling].sha256);
  }
 }
 CHECK(find_encoder_pretraining_asset(static_cast<BackboneKind>(255U)) == nullptr);
 auto historical = encoder_pretraining_provenance(kEncoderPretrainingAssets.front());
 historical.asset_id = "publisher/retired-source";
 historical.source_revision.assign(40U, '0');
 CHECK(encoder_pretraining_provenance_valid(historical));
 historical.sha256[0] = 'g';
 CHECK_FALSE(encoder_pretraining_provenance_valid(historical));
}
TEST_CASE("training source admission distinguishes Transfer scratch and Resume", "[backend][models][rfdetr][contract][encoder_pretraining]") {
 using namespace mmltk::backend::models::rfdetr;
 TrainRequest request;
 CHECK_FALSE(request.from_scratch);
 CHECK(request.encoder_weights_path.empty());
 request.train_compiled_path = "/train.bin";
 request.val_compiled_path = "/val.bin";
 request.output_dir = "/output";
 for (const bool scratch : {false, true}) for (const bool weights : {false, true}) for (const bool resume : {false, true})
  for (const bool layout : {false, true}) for (const bool encoder : {false, true}) for (const std::string_view preset : {"", "unknown", "rf-detr-nano"}) {
   request.from_scratch = scratch;
   request.weights_path = weights ? "/detector.pt" : "";
   request.resume_path = resume ? "/session" : "";
   request.class_layout_path = layout ? "/layout.json" : "";
   request.encoder_weights_path = encoder ? "/encoder.safetensors" : "";
   request.preset_name = preset;
   INFO("scratch=" << scratch << " weights=" << weights << " resume=" << resume << " layout=" << layout << " encoder=" << encoder << " preset=" << preset);
   const bool accepted = (static_cast<int>(scratch) + static_cast<int>(weights) + static_cast<int>(resume) == 1) && (!scratch || (preset == "rf-detr-nano" && !layout)) && (scratch || !encoder);
   if (accepted) CHECK_NOTHROW(validate_train_request(request));
   else CHECK_THROWS(validate_train_request(request));
  }
 request.weights_path.clear();
 request.resume_path.clear();
 request.class_layout_path.clear();
 request.from_scratch = true;
 for (const auto& preset : kPresetCatalog) {
  request.preset_name = preset.preset_name;
  CHECK_NOTHROW(validate_train_request(request));
 }
 const auto encoded = encode_train_request_json(request);
 CHECK(decode_train_request_json(encoded) == request);
 auto document = nlohmann::json::parse(encoded);
 CHECK(document.at("from_scratch") == true);
 CHECK(document.at("encoder_weights_path") == "/encoder.safetensors");
 document["from_scratch"] = "true";
 CHECK_THROWS(decode_train_request_json(document.dump()));
 document["from_scratch"] = true;
 document["encoder_weights_path"] = 7;
 CHECK_THROWS(decode_train_request_json(document.dump()));
 document["encoder_weights_path"] = std::string(4097U, 'x');
 CHECK_THROWS(decode_train_request_json(document.dump()));
 request.from_scratch = false;
 request.encoder_weights_path.clear();
 request.weights_path = "/detector.pt";
 const auto transfer = decode_train_request_json(encode_train_request_json(request));
 CHECK_FALSE(transfer.from_scratch);
 CHECK(transfer.encoder_weights_path.empty());
 CHECK(transfer == request);
}
