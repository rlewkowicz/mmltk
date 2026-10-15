#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <torch/utils.h>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
// Native-to-native checkpoint round-trip/admission coverage. Independent
// upstream execution is in the separate [upstream_oracle] cases.
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include "src/test_support/filesystem_test_utils.hpp"
// Import-bearing support follows every textual standard-library test helper.
#include "checkpoint_fixture_support.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "model_state_fixture.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "parity_fixture_support.h"
#include "upstream_oracle_fixture.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
#include <torch/types.h>
#include <torch/serialize.h>
namespace fs = std::filesystem;
// CLEANUP-IGNORE: This checkpoint test names the exact RF-DETR types used by its independent parity oracles.
namespace {
using mmltk::backend::models::rfdetr::find_preset_catalog_entry;
// CLEANUP-IGNORE: This parity test names the concrete checkpoint types used by its standalone fixtures.
using mmltk::backend::models::rfdetr::ModelOutputs;
using mmltk::backend::models::rfdetr::ModelStateLoadSummary;
using mmltk::backend::models::rfdetr::NativeRfDetrConfig;
using mmltk::backend::models::rfdetr::NativeRfDetrModel;
using mmltk::backend::models::rfdetr::nested_tensor_from_tensor_list;
using mmltk::backend::models::rfdetr::OutputLayer;  // CLEANUP-IGNORE: The parity fixture's remaining type inventory is
                                                    // independent of native-integration setup.
using mmltk::backend::models::rfdetr::testsupport::clone_normalized_model_state;
using mmltk::backend::models::rfdetr::testsupport::log_fixture_phase;
using mmltk::backend::models::rfdetr::testsupport::make_fixture_image;
using mmltk::backend::models::rfdetr::testsupport::parity_fixture_cases;
using mmltk::backend::models::rfdetr::testsupport::ParityFixtureCase;
NativeRfDetrConfig config_for_fixture(const ParityFixtureCase& fixture) {
 const auto* preset = find_preset_catalog_entry(fixture.preset_name);
 if (preset == nullptr) { throw std::runtime_error(std::string("missing model preset for parity fixture: ") + fixture.preset_name); }
 return native_config_from_preset(*preset);
}
std::string shape_string(const torch::Tensor& tensor) {
 std::ostringstream stream;
 stream << "[";
 for (int64_t index = 0; index < tensor.dim(); ++index) {
  if (index > 0) { stream << ", "; }
  stream << tensor.size(index);
 }
 stream << "]";
 return stream.str();
}
void assert_clean_summary(const ModelStateLoadSummary& summary, const std::string& label) {
 if (summary.missing_names.empty() && summary.unexpected_names.empty() && summary.incompatible_names.empty()) { return; }
 throw std::runtime_error(label + " state_dict mismatch");
}
void write_module_upstream_checkpoint(const fs::path& path, const NativeRfDetrModel& module) {
 mmltk::backend::models::rfdetr::DecodedNativeModelState state;
 state.metadata.class_layout = module.class_layout()->record();
 state.metadata.num_classes = module.config().num_classes;
 state.metadata.native_configuration = mmltk::backend::models::rfdetr::encode_checkpoint_configuration(module.config());
 state.metadata.num_queries = module.config().num_queries;
 state.metadata.num_select = module.config().num_select;
 state.metadata.cls_loss_coef = 7.3;
 state.metadata.bbox_loss_coef = 9.1;
 state.metadata.mask_ce_loss_coef = 8.2;
 const auto& technical_module = (module);
 mmltk::backend::models::rfdetr::testsupport::set_synthetic_model_state(state, clone_normalized_model_state(technical_module, true));
 mmltk::backend::models::rfdetr::write_upstream_model_state(path, state);
}
[[nodiscard]] bool same_shape(const torch::Tensor& left, const torch::Tensor& right) {
 if (left.dim() != right.dim()) return false;
 for (std::int64_t dimension = 0; dimension < left.dim(); ++dimension) {
  if (left.size(dimension) != right.size(dimension)) return false;
 }
 return true;
}
void assert_tensor_bitwise_equal(const torch::Tensor& actual, const torch::Tensor& expected, const std::string& label) {
 if (!same_shape(actual, expected)) { throw std::runtime_error(label + " shape mismatch: actual=" + shape_string(actual) + " expected=" + shape_string(expected)); }
 const auto actual_cpu = actual.detach().cpu().contiguous();
 const auto expected_cpu = expected.detach().cpu().to(actual_cpu.scalar_type()).contiguous();
 if (torch::equal(actual_cpu, expected_cpu)) { return; }
 const auto abs_diff = actual_cpu.sub(expected_cpu).abs();
 const auto max_abs = abs_diff.max().item<double>();
 throw std::runtime_error(label + " differs at the bitwise level: max_abs=" + std::to_string(max_abs));
}
void assert_output_layer_bitwise_equal(const OutputLayer& actual, const OutputLayer& expected, const std::string& label) {
 assert_tensor_bitwise_equal(actual.pred_logits, expected.pred_logits, label + ".pred_logits");
 assert_tensor_bitwise_equal(actual.pred_boxes, expected.pred_boxes, label + ".pred_boxes");
 if (actual.pred_masks.has_value() != expected.pred_masks.has_value()) { throw std::runtime_error(label + ".pred_masks presence mismatch"); }
 if (actual.pred_masks.has_value()) { assert_tensor_bitwise_equal(*actual.pred_masks, *expected.pred_masks, label + ".pred_masks"); }
}
void assert_outputs_bitwise_equal(const ModelOutputs& actual, const ModelOutputs& expected, const char* preset_name) {
 assert_output_layer_bitwise_equal(actual.main, expected.main, std::string(preset_name) + ".main");
 if (actual.aux_outputs.size() != expected.aux_outputs.size()) { throw std::runtime_error(std::string(preset_name) + ".aux_outputs size mismatch"); }
 for (size_t index = 0; index < actual.aux_outputs.size(); ++index) {
  assert_output_layer_bitwise_equal(actual.aux_outputs[index], expected.aux_outputs[index], std::string(preset_name) + ".aux[" + std::to_string(index) + "]");
 }
 if (actual.enc_outputs.has_value() != expected.enc_outputs.has_value()) { throw std::runtime_error(std::string(preset_name) + ".enc_outputs presence mismatch"); }
 if (actual.enc_outputs.has_value()) { assert_output_layer_bitwise_equal(*actual.enc_outputs, *expected.enc_outputs, std::string(preset_name) + ".enc"); }
}
void run_checkpoint_parity_case(const ParityFixtureCase& fixture, size_t index, size_t total) {
 const mmltk::testsupport::ScopedTempDir temp_dir("mmltk_rfdetr_checkpoint_parity");
 const fs::path upstream_path = temp_dir.path() / "weights" / fixture.upstream_filename;
 const fs::path native_path = temp_dir.path() / "weights" / (std::string(fixture.preset_name) + ".native.pt");
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "seed", fixture.preset_name);
 torch::manual_seed(fixture.query_rows + fixture.input_size);
 NativeRfDetrModel seeded_model(config_for_fixture(fixture));
 seeded_model.eval();
 write_module_upstream_checkpoint(upstream_path, seeded_model);
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "normalize", fixture.preset_name);
 const auto normalized = mmltk::backend::models::rfdetr::normalize_checkpoint_to_native(upstream_path, native_path);
 if (!fs::exists(native_path)) { throw std::runtime_error("failed to write native checkpoint parity fixture"); }
 if (normalized.metadata.preset_name != fixture.preset_name) { throw std::runtime_error("normalized checkpoint preset mismatch for parity fixture"); }
 const auto artifacts = mmltk::backend::models::rfdetr::resolve_model_artifacts(upstream_path, {}, 0);
 REQUIRE(artifacts.config.cls_loss_coef == 7.3);
 REQUIRE(artifacts.config.bbox_loss_coef == 9.1);
 REQUIRE(artifacts.config.mask_ce_loss_coef == 8.2);
 for (const auto& path : {upstream_path, native_path}) {
  auto admitted = mmltk::backend::models::rfdetr::resolve_model_state(path, {}, 0);
  mmltk::backend::models::rfdetr::apply_stock_training_coefficients(admitted.artifacts.config);
  REQUIRE(admitted.artifacts.config.cls_loss_coef == 1.0);
  REQUIRE(admitted.artifacts.config.bbox_loss_coef == 5.0);
  REQUIRE(admitted.artifacts.config.mask_ce_loss_coef == (admitted.artifacts.config.has_masks() ? 5.0 : 1.0));
  REQUIRE(admitted.model_state.metadata.cls_loss_coef == 7.3);
 }
 // Both executions are native: this is checkpoint storage/admission evidence,
 // not an independently executed upstream forward oracle.
 NativeRfDetrModel python_format_model(artifacts.config);
 NativeRfDetrModel native_model(artifacts.config);
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "load", fixture.preset_name);
 assert_clean_summary(mmltk::backend::models::rfdetr::load_model_weights(python_format_model, upstream_path, true), "Python format");
 assert_clean_summary(mmltk::backend::models::rfdetr::load_model_weights(native_model, native_path, true), "native");
 python_format_model.eval();
 native_model.eval();
 const auto image = make_fixture_image(fixture);
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "forward", fixture.preset_name);
 const auto python_format_outputs = python_format_model.forward(nested_tensor_from_tensor_list({image.clone()}), true);
 const auto native_outputs = native_model.forward(nested_tensor_from_tensor_list({image.clone()}), true);
 assert_outputs_bitwise_equal(python_format_outputs, native_outputs, fixture.preset_name);
 namespace rf = mmltk::backend::models::rfdetr;
 using rf::testsupport::CheckpointEnvelope;
 for (const auto envelope : {CheckpointEnvelope::Legacy, CheckpointEnvelope::Raw, CheckpointEnvelope::Lightning, CheckpointEnvelope::CompiledLightning, CheckpointEnvelope::RawStateDict}) {
  const auto path = temp_dir.path() / fixture.upstream_filename;
  rf::testsupport::write_checkpoint_envelope(upstream_path, path, envelope);
  auto admitted = rf::resolve_model_state(path, fixture.preset_name, fixture.input_size);
  auto candidate = python_format_model.stage_normalized_state(admitted.model_state.entries(), rf::detail::NormalizedModelStateAdmission::RequiredCore);
  assert_clean_summary(candidate.summary, "complete envelope");
  REQUIRE(candidate.summary.loaded_names.size() == admitted.model_state.tensor_count());
  python_format_model.commit_normalized_state(std::move(candidate));
  assert_outputs_bitwise_equal(python_format_model.forward(nested_tensor_from_tensor_list({image.clone()}), true), native_outputs, fixture.preset_name);
  if (envelope != CheckpointEnvelope::Raw) REQUIRE(admitted.artifacts.config.cls_loss_coef == 7.3);
 }
 const auto malformed = temp_dir.path() / "malformed.pth";
 rf::testsupport::write_checkpoint_envelope(upstream_path, malformed, CheckpointEnvelope::Collision);
 REQUIRE_THROWS(rf::decode_model_state(malformed));
 for (const auto metadata : {R"({"use_grouppose_keypoints":true})", R"({"window_block_indexes":[2,5,8,11]})", R"({"dropout":0.1})"}) {
  rf::testsupport::write_checkpoint_envelope(upstream_path, malformed, CheckpointEnvelope::Lightning, metadata);
  REQUIRE_THROWS(rf::decode_model_state(malformed));
 }
 // Same-shaped conflicting topology is rejected at external admission.
 rf::testsupport::write_checkpoint_envelope(upstream_path, malformed, CheckpointEnvelope::CompiledLightning, R"({"num_windows":7})");
 REQUIRE_THROWS(rf::decode_model_state(malformed));
}
}  // namespace
void test_checkpoint_parity_matches_for_all_registered_fixtures() {
 const auto& fixtures = parity_fixture_cases();
 for (size_t index = 0; index < fixtures.size(); ++index) { run_checkpoint_parity_case(fixtures[index], index + 1, fixtures.size()); }
}
TEST_CASE("Native checkpoint format round-trip matches for all registered fixtures", "[model][rfdetr][checkpoint_parity][roundtrip][integration]") {
 test_checkpoint_parity_matches_for_all_registered_fixtures();
}

TEST_CASE("All official and controlled assets retain strict topology and checkpoint normalization", "[model][rfdetr][checkpoint_parity][upstream_oracle]") {
 namespace rf=mmltk::backend::models::rfdetr;
 const auto requested=rf::testsupport::UpstreamOracle::requested();
 REQUIRE(requested);
 const rf::testsupport::UpstreamOracle oracle(*requested);
 for (const auto& row:rf::kPresetCatalog) {
  DYNAMIC_SECTION(row.preset_name) {
   const auto original=oracle.official_checkpoint(row.preset_name);
   auto admitted=rf::resolve_model_state(original,row.preset_name,row.resolution);
   const auto& config=admitted.artifacts.config;
   CHECK(config.task==row.task);
   CHECK(config.resolution==row.resolution);
   CHECK(config.num_classes==row.class_count);
   CHECK(config.num_queries==row.query_count);
   CHECK(config.num_select==row.selected_query_count);
   CHECK(config.group_detr==row.group_count);
   CHECK(config.backbone==row.backbone);
   const rf::ResolvedClassLayout layout(admitted.artifacts.class_layout);
   REQUIRE(layout.semantic());
   REQUIRE_FALSE(layout.catalog()->empty());
   const auto& entries=admitted.model_state.entries();
   const auto classifier=std::ranges::find(entries,std::string("class_embed.weight"),&rf::NormalizedModelStateEntry::name);
   REQUIRE(classifier!=entries.end());
   REQUIRE(classifier->tensor.size(0)==row.class_count);
   const auto active=std::ranges::find(entries,std::string("_kp_active_mask"),&rf::NormalizedModelStateEntry::name);
   if (row.task==rf::ModelTask::Keypoints) {
    REQUIRE(active!=entries.end());
    const auto joints=rf::KeypointTensorLayout::from_active_mask(active->tensor);
    REQUIRE(std::ranges::equal(joints.counts(),config.keypoints_per_class));
    joints.require_class_layout(layout);
    REQUIRE(layout.catalog()->names().size()==1);
    CHECK(layout.catalog()->names().front()=="person");
    CHECK(layout.catalog()->keypoints(0)==mmltk::backend::data::catalog::coco_person_keypoint_schema());
   } else REQUIRE(active==entries.end());
   const mmltk::testsupport::ScopedTempDir root("official-checkpoint-normalization");
   const auto require_normalization=[&](const fs::path& source_path,const rf::ResolvedModelState& source,const char* filename) {
    const auto normalized=root.path()/filename;
    rf::normalize_checkpoint_to_native(source_path,normalized);
    const auto roundtrip=rf::resolve_model_state(normalized,row.preset_name,row.resolution);
    REQUIRE_FALSE(roundtrip.model_state.metadata.native_configuration.empty());
    REQUIRE(roundtrip.artifacts.config==source.artifacts.config);
    REQUIRE(rf::decode_checkpoint_configuration(roundtrip.model_state.metadata.native_configuration)==source.artifacts.config);
    REQUIRE(roundtrip.artifacts.class_layout==source.artifacts.class_layout);
    const rf::ResolvedClassLayout source_layout(source.artifacts.class_layout);
    if (source_layout.semantic()) REQUIRE(rf::ResolvedClassLayout(roundtrip.artifacts.class_layout).catalog()->ordered_equal(*source_layout.catalog()));
    REQUIRE(roundtrip.model_state.entries().size()==source.model_state.entries().size());
    for (const auto& entry:source.model_state.entries()) {
     const auto target=std::ranges::find(roundtrip.model_state.entries(),entry.name,&rf::NormalizedModelStateEntry::name);
     REQUIRE(target!=roundtrip.model_state.entries().end());
     CAPTURE(entry.name);
     CHECK(target->tensor.scalar_type()==entry.tensor.scalar_type());
     CHECK(target->tensor.sizes()==entry.tensor.sizes());
     CHECK(torch::equal(target->tensor,entry.tensor));
    }
   };
   require_normalization(original,admitted,"stock.pt");
   admitted.model_state.release_admission();
   // The derived bytes have their own identity. Official semantic meaning is
   // admitted independently above and attached by UpstreamOracle::model.
   const auto controlled_path=oracle.checkpoint(row.preset_name);
   auto controlled=rf::resolve_model_state(controlled_path,row.preset_name,row.resolution);
   const auto& controlled_config=controlled.artifacts.config;
   const auto& metadata=controlled.model_state.metadata;
   REQUIRE_FALSE(metadata.native_configuration.empty());
   REQUIRE(rf::decode_checkpoint_configuration(metadata.native_configuration)==controlled_config);
   REQUIRE(controlled_config.group_detr==2);
   REQUIRE(controlled_config.resolution==config.resolution);
   REQUIRE(controlled_config.num_classes==config.num_classes);
   REQUIRE(controlled_config.num_queries==config.num_queries);
   REQUIRE(controlled_config.num_select==config.num_select);
   REQUIRE(controlled_config.backbone==config.backbone);
   REQUIRE(controlled_config.keypoints_per_class==config.keypoints_per_class);
   auto expected=rf::native_config_from_preset(row);
   expected.group_detr=2;
   REQUIRE_NOTHROW(rf::require_checkpoint_model_topology(expected,metadata));
   REQUIRE(controlled_config.training_supervision==expected.training_supervision);
   REQUIRE(controlled.artifacts.class_layout.provenance.origin!=rf::ClassLayoutOrigin::VerifiedAsset);
   REQUIRE_FALSE(rf::ResolvedClassLayout(controlled.artifacts.class_layout).semantic());
   REQUIRE(rf::ResolvedClassLayout(controlled.artifacts.class_layout).output_width()==layout.output_width());
   const auto external=nlohmann::json::parse(metadata.external_topology);
   REQUIRE(external.at("group_detr")==2);
   const auto& controlled_entries=controlled.model_state.entries();
   for (const auto* name:{"query_feat.weight","refpoint_embed.weight"}) {
    const auto query=std::ranges::find(controlled_entries,std::string(name),&rf::NormalizedModelStateEntry::name);
    REQUIRE(query!=controlled_entries.end());
    REQUIRE(query->tensor.size(0)==controlled_config.num_queries*controlled_config.group_detr);
   }
   if (row.task==rf::ModelTask::Keypoints) {
    const auto mask=std::ranges::find(controlled_entries,std::string("_kp_active_mask"),&rf::NormalizedModelStateEntry::name);
    REQUIRE(mask!=controlled_entries.end());
    const auto joints=rf::KeypointTensorLayout::from_active_mask(mask->tensor);
    REQUIRE(std::ranges::equal(joints.counts(),config.keypoints_per_class));
    joints.require_class_layout(layout);
   }
   require_normalization(controlled_path,controlled,"controlled.pt");
   auto conflicting=metadata;
   conflicting.native_configuration.clear();
   REQUIRE_THROWS_AS(rf::checkpoint_configuration(conflicting),std::invalid_argument);
   conflicting=metadata;
   auto wrong_config=controlled_config;
   wrong_config.group_detr=row.group_count;
   conflicting.native_configuration=rf::encode_checkpoint_configuration(wrong_config);
   REQUIRE_THROWS_AS(rf::checkpoint_configuration(conflicting),std::invalid_argument);
   conflicting=metadata;
   auto wrong_external=external;
   wrong_external["group_detr"]=row.group_count;
   conflicting.external_topology=wrong_external.dump();
   REQUIRE_THROWS_AS(rf::checkpoint_configuration(conflicting),std::invalid_argument);
  }
 }
}
