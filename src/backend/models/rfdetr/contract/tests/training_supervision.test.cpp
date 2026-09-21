#include <array>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include <cmath>
#include <cstddef>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace {
using namespace mmltk::backend::models::rfdetr;
namespace serialization = mmltk::frameworks::serialization;
void test_defaults_and_cli_spellings_are_stable() {
 const TrainingSupervisionConfig config;
 CHECK(config.assignment == TrainAssignmentKind::Hungarian);
 CHECK_FALSE(config.denoising.enabled);
 CHECK(config.match_free.rho == 0.5F);
 CHECK(config.match_free.correspondence_weight == 1.0F);
 CHECK(config.match_free.query_weight == 1.0F);
 CHECK(config.denoising.groups == 5U);
 CHECK(config.denoising.label_noise_ratio == 0.2F);
 CHECK(config.denoising.center_noise_scale == 0.4F);
 CHECK(config.denoising.size_noise_scale == 0.4F);
 CHECK(training_supervision_config_valid(config));
 CHECK(cli_enum_spelling(TrainAssignmentKind::Hungarian) == "hungarian");
 CHECK(cli_enum_spelling(TrainAssignmentKind::MatchFree) == "match-free");
 CHECK(train_assignment_from_spelling("hungarian") == TrainAssignmentKind::Hungarian);
 CHECK(train_assignment_from_spelling("match-free") == TrainAssignmentKind::MatchFree);
 CHECK_FALSE(train_assignment_from_spelling("MatchFree"));
}
void test_open_and_closed_ranges_and_relation_are_enforced() {
 TrainingSupervisionConfig config;
 config.assignment = TrainAssignmentKind::MatchFree;
 CHECK(training_supervision_config_valid(config));
 config.assignment = static_cast<TrainAssignmentKind>(std::numeric_limits<std::uint8_t>::max());
 CHECK_FALSE(training_supervision_config_valid(config));
 config.assignment = TrainAssignmentKind::MatchFree;
 config.match_free.correspondence_weight = 0.0F;
 config.match_free.query_weight = 0.0F;
 CHECK_FALSE(training_supervision_config_valid(config));
 config.match_free.query_weight = 1.0F;
 for (const float endpoint : {0.0F, 1.0F}) {
  config.match_free.rho = endpoint;
  CHECK_FALSE(training_supervision_config_valid(config));
  config.match_free.rho = 0.5F;
  config.denoising.center_noise_scale = endpoint;
  CHECK_FALSE(training_supervision_config_valid(config));
  config.denoising.center_noise_scale = 0.4F;
  config.denoising.size_noise_scale = endpoint;
  CHECK_FALSE(training_supervision_config_valid(config));
  config.denoising.size_noise_scale = 0.4F;
 }
 config.denoising.label_noise_ratio = 0.0F;
 CHECK(training_supervision_config_valid(config));
 config.denoising.label_noise_ratio = 1.0F;
 CHECK(training_supervision_config_valid(config));
 config.denoising.groups = 0U;
 CHECK_FALSE(training_supervision_config_valid(config));
 config.denoising.groups = static_cast<std::uint16_t>(kMaximumDenoisingGroups + 1U);
 CHECK_FALSE(training_supervision_config_valid(config));
 config.denoising.groups = 5U;
 config.match_free.rho = std::numeric_limits<float>::quiet_NaN();
 CHECK_FALSE(training_supervision_config_valid(config));
 CHECK(kSupervisionOpenUnitMinimum == std::nextafter(0.0F, 1.0F));
 CHECK(kSupervisionOpenUnitMaximum == std::nextafter(1.0F, 0.0F));
}
void test_reflected_cbor_round_trip_preserves_complete_nested_value() {
 TrainingSupervisionConfig source;
 source.assignment = TrainAssignmentKind::MatchFree;
 source.match_free = {.rho = 0.625F, .correspondence_weight = 0.75F, .query_weight = 1.25F};
 source.denoising = {
  .enabled = true,
  .groups = 10U,
  .label_noise_ratio = 0.3F,
  .center_noise_scale = 0.45F,
  .size_noise_scale = 0.35F,
 };
 constexpr std::size_t capacity = serialization::reflected_maximum_cbor_bytes<TrainingSupervisionConfig>();
 STATIC_REQUIRE(capacity > 0U);
 std::array<std::byte, capacity> encoded{};
 const serialization::wire::Limits limits{.max_bytes = capacity, .max_items = 64U, .max_depth = 8U};
 const auto size = serialization::encode(source, encoded, limits);
 REQUIRE(size.has_value());
 const auto decoded = serialization::decode<TrainingSupervisionConfig>({std::span(encoded).first(*size), {}}, limits);
 REQUIRE(decoded.has_value());
 CHECK(*decoded == source);
}
TrainRequest valid_train_request() {
 TrainRequest request;
 request.train_compiled_path = "/tmp/train.bin";
 request.val_compiled_path = "/tmp/val.bin";
 request.weights_path = "/tmp/weights.pth";
 request.output_dir = "/tmp/output";
 request.preset_name = "rf-detr-nano";
 return request;
}
void test_request_and_model_boundaries_reject_unsupported_feature_combinations() {
 TrainRequest request = valid_train_request();
 validate_train_request(request);
 request.training_supervision.assignment = TrainAssignmentKind::MatchFree;
 validate_train_request(request);
 request.preset_name = "rf-detr-seg-nano";
 CHECK_THROWS(validate_train_request(request));
 request = valid_train_request();
 request.training_supervision.denoising.enabled = true;
 request.compilation_mode = CompilationMode::kFullTrace;
 CHECK_THROWS(validate_train_request(request));
 NativeRfDetrConfig model = native_config_from_preset(*find_preset_catalog_entry("rf-detr-nano"));
 model.training_supervision.assignment = TrainAssignmentKind::MatchFree;
 CHECK(training_supervision_model_config_valid(model));
 model.set_cost_class = 0.0;
 model.set_cost_bbox = 0.0;
 model.set_cost_giou = 0.0;
 CHECK_FALSE(training_supervision_model_config_valid(model));
 model = native_config_from_preset(*find_preset_catalog_entry("rf-detr-nano"));
 model.training_supervision.denoising.enabled = true;
 model.cls_loss_coef = 0.0;
 model.bbox_loss_coef = 0.0;
 model.giou_loss_coef = 0.0;
 CHECK_FALSE(training_supervision_model_config_valid(model));
 model.cls_loss_coef = 1.0;
 model.focal_alpha = std::numeric_limits<double>::infinity();
 CHECK_FALSE(training_supervision_model_config_valid(model));
}
void test_query_layout_capacity_checks_each_product_and_sum() {
 TrainingSupervisionConfig config;
 config.denoising.enabled = true;
 CHECK(training_supervision_query_layout_valid(config, 300U, 13U, 100U));
 CHECK_FALSE(training_supervision_query_layout_valid(config, std::numeric_limits<std::size_t>::max(), 2U, 0U));
 CHECK_FALSE(training_supervision_query_layout_valid(config, 0U, 1U, std::numeric_limits<std::size_t>::max()));
 CHECK_FALSE(training_supervision_query_layout_valid(config, std::numeric_limits<std::size_t>::max() - 1U, 1U, 1U));
}
}  // namespace
TEST_CASE("test_defaults_and_cli_spellings_are_stable", "[model][rfdetr][training_supervision]") { test_defaults_and_cli_spellings_are_stable(); }
TEST_CASE("test_open_and_closed_ranges_and_relation_are_enforced", "[model][rfdetr][training_supervision]") {
 test_open_and_closed_ranges_and_relation_are_enforced();
}
TEST_CASE("test_reflected_cbor_round_trip_preserves_complete_nested_value", "[model][rfdetr][training_supervision]") {
 test_reflected_cbor_round_trip_preserves_complete_nested_value();
}
TEST_CASE("test_request_and_model_boundaries_reject_unsupported_feature_combinations", "[model][rfdetr][training_supervision]") {
 test_request_and_model_boundaries_reject_unsupported_feature_combinations();
}
TEST_CASE("test_query_layout_capacity_checks_each_product_and_sum", "[model][rfdetr][training_supervision]") {
 test_query_layout_capacity_checks_each_product_and_sum();
}
