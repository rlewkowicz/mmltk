#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <torch/utils.h>
#include <filesystem>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
// RF-DETR training checkpoint parity coverage.
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
#include <torch/types.h>
#include <torch/serialize.h>
namespace fs = std::filesystem;
// CLEANUP-IGNORE: The parity fixture's namespace aliases are independent of the optimizer fixture's typed inventory.
namespace model_detail = mmltk::backend::models::rfdetr::detail;
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
 if (!same_shape(actual, expected)) {
  throw std::runtime_error(label + " shape mismatch: actual=" + shape_string(actual) + " expected=" + shape_string(expected));
 }
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
 if (actual.enc_outputs.has_value() != expected.enc_outputs.has_value()) {
  throw std::runtime_error(std::string(preset_name) + ".enc_outputs presence mismatch");
 }
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
 NativeRfDetrModel upstream_model(artifacts.config);
 NativeRfDetrModel native_model(artifacts.config);
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "load", fixture.preset_name);
 assert_clean_summary(mmltk::backend::models::rfdetr::load_model_weights(upstream_model, upstream_path, true), "upstream");
 assert_clean_summary(mmltk::backend::models::rfdetr::load_model_weights(native_model, native_path, true), "native");
 upstream_model.eval();
 native_model.eval();
 const auto image = make_fixture_image(fixture);
 log_fixture_phase("test_rfdetr_checkpoint_parity", index, total, "forward", fixture.preset_name);
 const auto upstream_outputs = upstream_model.forward(nested_tensor_from_tensor_list({image.clone()}), true);
 const auto native_outputs = native_model.forward(nested_tensor_from_tensor_list({image.clone()}), true);
 assert_outputs_bitwise_equal(upstream_outputs, native_outputs, fixture.preset_name);
}
}  // namespace
void test_checkpoint_parity_matches_for_all_registered_fixtures() {
 const auto& fixtures = parity_fixture_cases();
 for (size_t index = 0; index < fixtures.size(); ++index) { run_checkpoint_parity_case(fixtures[index], index + 1, fixtures.size()); }
}
TEST_CASE("test_checkpoint_parity_matches_for_all_registered_fixtures", "[model][rfdetr][checkpoint_parity][integration]") {
 test_checkpoint_parity_matches_for_all_registered_fixtures();
}
