#include "src/controller/services/training/train_command.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <cstddef>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <limits>
#include <string>
#include <type_traits>
namespace {
namespace r = mmltk::backend::models::rfdetr;
r::TrainRequest request() {
 r::TrainRequest value;
 value.train_compiled_path = "/tmp/train.bin";
 value.val_compiled_path = "/tmp/val.bin";
 value.weights_path = "/tmp/weights.pt";
 value.output_dir = "/tmp/output";
 return value;
}
r::TrainRequest child_request(const r::TrainRequest& value) {
 const auto arguments = mmltk::controller::services::build_train_command_arguments(value);
 REQUIRE(arguments.size() == 4);
 CHECK(arguments[0] == "rfdetr");
 CHECK(arguments[1] == "train");
 CHECK(arguments[2] == "--request-json");
 REQUIRE(arguments[3].size() <= r::kMaximumTrainRequestJsonBytes);
 return r::decode_train_request_json(arguments[3]);
}
}  // namespace
TEST_CASE("training child carries the complete canonical request including topology and independent recipes", "[gui][train_command]") {
 auto value = request();
 CHECK(child_request(value) == value);
 value.device_ids = {3, 1};
 value.numa_nodes = {2, -1};
 value.h2d_dataloader = false;
 value.test_compiled_path = "/independent/test.bin";
 value.progress_bar = false;
 value.recipe.optimizer = r::TrainOptimizerKind::SGD;
 r::reset_train_recipe(value.recipe);
 value.lanes = 2;
 value.lane_configuration.mode = r::TrainLaneMode::Independent;
 r::resize_training_models(value.lane_configuration, 2, value.recipe, value.seed);
 value.lane_configuration.models[1].seed = 9001;
 value.lane_configuration.models[1].recipe.lr = .012345678901234;
 using Relation = mmltk::frameworks::reflection::catalog_provider_relation<r::TrainRecipeCatalog>;
 Relation::set_override<mmltk::frameworks::reflection::member_path<&r::TrainRecipeSettings::lr>>(value.lane_configuration.models[1].recipe.overrides);
 value.data_policy.balancing = r::TrainBalancing::RareRepeatsStratified;
 value.validation_lanes = 3;
 value.unfreeze_encoder_last_epochs = 4;
 value.disable_augmentation_last_epochs = 7;
 CHECK(child_request(value) == value);
 value.weights_path.clear();
 value.resume_path = "/tmp/resume.pt";
 CHECK(child_request(value) == value);
 value.distributed_worker = true;
 value.distributed_rank = 1;
 value.distributed_world_size = 2;
 value.distributed_store_path = "/tmp/distributed-store";
 CHECK(child_request(value) == value);
}
TEST_CASE("every optimizer recipe retains complete JSON command and checkpoint configuration values", "[gui][train_command]") {
 namespace reflection = mmltk::frameworks::reflection;
 namespace serialization = mmltk::frameworks::serialization;
 using Relation = reflection::catalog_provider_relation<r::TrainRecipeCatalog>;
 constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<r::TrainRequest>();
 const serialization::wire::Limits limits{.max_bytes = capacity, .max_items = 4096U, .max_depth = 32U};
 std::vector<std::byte> encoded(capacity);
 for (const auto& catalog : r::kTrainRecipeCatalog) {
  auto value = request();
  value.recipe.optimizer = catalog.optimizer;
  r::reset_train_recipe(value.recipe);
  CHECK(static_cast<const r::TrainRecipeValues&>(value.recipe) == static_cast<const r::TrainRecipeValues&>(catalog));
  CHECK(r::train_recipe_valid(value.recipe));
  Relation::VisitMembers([&]<class Entry>() { Relation::set_override<Entry::destination>(value.recipe.overrides); });
  value.lanes = 2;
  value.lane_configuration.mode = r::TrainLaneMode::Independent;
  r::resize_training_models(value.lane_configuration, 2, value.recipe, value.seed);
  auto& detached = value.lane_configuration.models.back().recipe;
  detached.optimizer = r::TrainOptimizerKind::SGD;
  r::reset_train_recipe(detached);
  detached.lr = .003;
  Relation::set_override<reflection::member_path<&r::TrainRecipeSettings::lr>>(detached.overrides);
  CHECK(child_request(value) == value);
  const auto size = serialization::encode(value, std::span(encoded), limits);
  REQUIRE(size.has_value());
  const auto decoded = serialization::decode<r::TrainRequest>({std::span(encoded).first(*size), {}}, limits);
  REQUIRE(decoded.has_value());
  CHECK(*decoded == value);
  CHECK_NOTHROW(r::validate_train_request(*decoded));
 }
}
TEST_CASE("training command preserves supervision and independent perceptual switches exactly", "[gui][train_command][training_supervision]") {
 for (const auto assignment : {r::TrainAssignmentKind::Hungarian, r::TrainAssignmentKind::MatchFree})
  for (bool denoising : {false, true})
   for (bool augmentation : {false, true})
    for (bool perceptual : {false, true}) {
     auto value = request();
     value.training_supervision.assignment = assignment;
     value.training_supervision.match_free.rho = r::kSupervisionOpenUnitMaximum;
     value.training_supervision.match_free.correspondence_weight = .12345679F;
     value.training_supervision.match_free.query_weight = 1.25F;
     value.training_supervision.denoising.enabled = denoising;
     value.training_supervision.denoising.groups = 10;
     value.training_supervision.denoising.label_noise_ratio = .87654322F;
     value.training_supervision.denoising.center_noise_scale = r::kSupervisionOpenUnitMinimum;
     value.training_supervision.denoising.size_noise_scale = r::kSupervisionOpenUnitMaximum;
     value.gpu_augmentation.enabled = augmentation;
     value.gpu_augmentation.perceptual_downscale = perceptual;
     CHECK(child_request(value) == value);
    }
}
TEST_CASE("canonical training JSON is bounded and rejects invalid payloads before launch", "[gui][train_command]") {
 CHECK_THROWS(r::decode_train_request_json(""));
 CHECK_THROWS(r::decode_train_request_json(std::string(r::kMaximumTrainRequestJsonBytes + 1, ' ')));
 CHECK_THROWS(r::decode_train_request_json("{broken"));
 auto value = request();
 value.device_id = -1;
 CHECK_THROWS(child_request(value));
 value = request();
 value.recipe.lr = std::numeric_limits<double>::infinity();
 CHECK_THROWS(child_request(value));
 value = request();
 value.recipe.lr_scheduler = r::TrainLrSchedulerKind::UltralyticsLinear;
 CHECK_THROWS(child_request(value));
 value = request();
 value.lanes = 0;
 CHECK_THROWS(child_request(value));
}
TEST_CASE("recipe selection and reset preserve scope and derive every override bit", "[gui][train_command]") {
 using Relation = mmltk::frameworks::reflection::catalog_provider_relation<r::TrainRecipeCatalog>;
 auto value = request();
 value.recipe.lr = .009;
 Relation::set_override<mmltk::frameworks::reflection::member_path<&r::TrainRecipeSettings::lr>>(value.recipe.overrides);
 value.recipe.optimizer = r::TrainOptimizerKind::Muon;
 r::resolve_train_recipe(value.recipe);
 CHECK(value.recipe.lr == .009);
 CHECK(value.recipe.lr_encoder == .0003);
 CHECK(value.recipe.lr_scheduler == r::TrainLrSchedulerKind::Cosine);
 auto independent = value.recipe;
 r::reset_train_recipe(value.recipe);
 CHECK(value.recipe.optimizer == r::TrainOptimizerKind::Muon);
 CHECK(value.recipe.lr == .0002);
 CHECK(independent.lr == .009);
 std::size_t fields = 0;
 Relation::VisitMembers([&]<class Entry> {
  CHECK_FALSE(Relation::overridden<Entry::destination>(value.recipe.overrides));
  ++fields;
 });
 CHECK(fields == r::kTrainRecipeFieldCount);
 CHECK(r::cli_enum_spelling(r::TrainOptimizerKind::SGD) == "sgd");
 CHECK(r::train_lr_scheduler_from_spelling("ultralytics-linear") == r::TrainLrSchedulerKind::UltralyticsLinear);
 CHECK_FALSE(r::train_lr_scheduler_from_spelling("Cosine"));
}
TEST_CASE("Execution admission checks global products bounded stable models and finite recipe values", "[gui][train_command][execution]") {
 auto value = request();
 CHECK(value.lanes == 1);
 CHECK(value.validation_lanes == 1);
 CHECK(r::PredictRequest{}.lanes == 1);
 CHECK(r::ValidateRequest{}.lanes == 1);
 value.batch_size = 3;
 value.grad_accum_steps = 4;
 value.lanes = 2;
 const auto shared = r::derive_execution_facts(value, 17);
 CHECK(shared.settings_revision == 17);
 CHECK(shared.logical_models == 1);
 CHECK(shared.microbatches_per_attempt == 8);
 CHECK(shared.effective_batch_per_model == 24);
 CHECK(shared.aggregate_round_images == 24);
 for (const auto batch : {1U, 3U, 12U}) {
  auto distributed = value;
  distributed.batch_size = batch;
  distributed.device_ids = {1, 0};
  const auto configured = r::derive_training_batch_distribution(distributed);
  CHECK(configured.ranks == 2);
  const auto first = r::training_rank_slice(batch, 0, 2);
  const auto last = r::training_rank_slice(batch, 1, 2);
  CHECK(configured.minimum_rank_batch == last.count);
  CHECK(configured.maximum_rank_batch == first.count);
  CHECK(first.count + last.count == batch);
  CHECK(last.begin == first.count);
  distributed.device_ids.clear();
  CHECK(r::derive_training_batch_distribution(distributed).minimum_rank_batch == batch);
 }
 r::resize_training_models(value.lane_configuration, r::kMaximumTrainingModels, value.recipe, value.seed);
 CHECK(value.lane_configuration.models.size() == 16);
 CHECK(value.lane_configuration.models.front().seed != value.lane_configuration.models.back().seed);
 const auto retained = value.lane_configuration;
 CHECK_THROWS(r::resize_training_models(value.lane_configuration, 17, value.recipe, value.seed));
 CHECK(value.lane_configuration == retained);
 value.lane_configuration.mode = r::TrainLaneMode::PeriodicAveraging;
 value.lanes = 16;
 CHECK(r::derive_execution_facts(value, 18).effective_batch_per_model == 12);
 CHECK(r::effective_final_policy(value.lane_configuration) == r::TrainFinalPolicy::Off);
 value.lane_configuration.models.clear();
 value.lane_configuration.next_model_id = std::numeric_limits<std::uint64_t>::max();
 CHECK_THROWS(r::resize_training_models(value.lane_configuration, 1, value.recipe, value.seed));
 value = request();
 value.batch_size = std::numeric_limits<std::size_t>::max();
 value.lanes = 2;
 CHECK_THROWS(r::derive_execution_facts(value, 0));
 value = request();
 value.val_batch_size = std::numeric_limits<std::size_t>::max();
 value.validation_lanes = 2;
 CHECK_THROWS(r::validate_train_request(value));
 r::ValidateRequest validation;
 validation.batch_size = std::numeric_limits<std::size_t>::max();
 validation.lanes = 2;
 CHECK_THROWS(r::derive_execution_facts(validation, 0));
 r::PredictRequest prediction;
 prediction.batch_size = std::numeric_limits<std::size_t>::max();
 prediction.lanes = 2;
 CHECK_THROWS(r::derive_execution_facts(prediction, 0));
 value = request();
 value.recipe.optimizer = r::TrainOptimizerKind::SGD;
 r::reset_train_recipe(value.recipe);
 value.recipe.nesterov = true;
 value.recipe.momentum = 0;
 CHECK_FALSE(r::train_recipe_valid(value.recipe));
 value.recipe.momentum = .9;
 CHECK(r::train_recipe_valid(value.recipe));
 using Relation = mmltk::frameworks::reflection::catalog_provider_relation<r::TrainRecipeCatalog>;
 Relation::VisitMembers([&]<class Entry> {
  auto candidate = value.recipe;
  auto& field = mmltk::frameworks::reflection::access<r::TrainRecipeSettings, Entry::destination>(candidate);
  using Field = std::remove_cvref_t<decltype(field)>;
  constexpr auto constraint = mmltk::frameworks::reflection::accessor_policy<Entry::destination>();
  if constexpr (std::is_arithmetic_v<Field> && !std::same_as<Field, bool>) {
   if constexpr (constraint.has_minimum) {
    field = static_cast<Field>(constraint.minimum - 1);
    CHECK_FALSE(r::train_recipe_valid(candidate));
    auto invalid = value;
    invalid.recipe = candidate;
    CHECK_THROWS(child_request(invalid));
   }
   if constexpr (constraint.has_maximum) {
    field = static_cast<Field>(constraint.maximum + 1);
    CHECK_FALSE(r::train_recipe_valid(candidate));
   }
  }
  if constexpr (std::floating_point<Field>) {
   field = std::numeric_limits<double>::quiet_NaN();
   CHECK_FALSE(r::train_recipe_valid(candidate));
   field = std::numeric_limits<double>::infinity();
   CHECK_FALSE(r::train_recipe_valid(candidate));
  }
 });
}
