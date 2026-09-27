#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "src/backend/data/tests/test_fixture.h"
#include <limits>
#include <string>
#include <vector>
#include <utility>
#include "src/backend/ml/torch/archive.h"
#include "detail/checkpoint_private.h"
#include "detail/model_ema.h"
#include "detail/training_continuation.h"
#include "detail/training_data_plan.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
#include "src/backend/models/rfdetr/augmentation/tests/gpu_augment_test_support.h"
#include "detail/training_snapshot.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "training_continuation_fixture.h"
namespace {
namespace r = mmltk::backend::models::rfdetr;
r::TrainRequest saved_request() {
 r::TrainRequest request;
 request.train_compiled_path = "train.bin";
 request.val_compiled_path = "val.bin";
 request.weights_path = "seed.pt";
 request.output_dir = "run";
 request.gpu_augmentation.perceptual_downscale = true;
 return request;
}
torch::serialize::InputArchive continuation_fixture(const r::TrainRequest& request, const r::detail::TrainingContinuationValues& values) {
 torch::serialize::OutputArchive output;
 r::detail::write_training_continuation(output, request, values);
 torch::serialize::OutputArchive optimizer;
 mmltk::backend::ml::serialization::write_int(optimizer, "fixture", 1);
 output.write("optimizer", optimizer);
 if (request.use_ema) {
  torch::serialize::OutputArchive ema;
  mmltk::backend::ml::serialization::write_int(ema, "entry_count", 0);
  output.write("ema_state", ema);
 }
 return r::testsupport::checkpoint_input(output);
}
torch::serialize::InputArchive continuation_fixture(const r::TrainRequest& request) {
 return continuation_fixture(request, r::testsupport::continuation_values(request, {.epoch = 0,
                                                                                    .grad_scaler_scale = 1024.0,
                                                                                    .grad_scaler_growth_tracker = 17,
                                                                                    .ema_completed_updates = request.use_ema ? 37 : 0,
                                                                                    .training_attempt_id = "attempt",
                                                                                    .training_original_descriptor = "original.json"}));
}
void test_current_continuation_required_fields() {
 auto source = continuation_fixture(saved_request());
 const auto loaded = r::detail::read_training_continuation(source);
 REQUIRE(loaded.has_value());
 CHECK(loaded->configuration.gpu_augmentation.perceptual_downscale);
 CHECK_FALSE(loaded->configuration.gpu_augmentation.enabled);
 // Every written continuation fact is mandatory except the default supervision
 // blob, whose current format deliberately omits the default value.
 for (const auto& key : source.keys()) {
  torch::serialize::OutputArchive incomplete;
  r::testsupport::copy_checkpoint_archive(source, incomplete, key);
  auto input = r::testsupport::checkpoint_input(incomplete);
  INFO(key);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 for (const auto& key : source.keys()) {
  torch::serialize::OutputArchive malformed;
  r::testsupport::copy_checkpoint_archive(source, malformed, key);
  malformed.write(key, c10::IValue(c10::List<int64_t>{}));
  auto input = r::testsupport::checkpoint_input(malformed);
  INFO(key);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 for (const auto* key : {"ema_state", "training_supervision_config_cbor"}) {
  torch::serialize::OutputArchive malformed;
  r::testsupport::copy_checkpoint_archive(source, malformed);
  malformed.write(key, c10::IValue(int64_t{1}));
  auto input = r::testsupport::checkpoint_input(malformed);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 torch::serialize::OutputArchive weights;
 mmltk::backend::ml::serialization::write_string(weights, "source_kind", "weights-only");
 auto input = r::testsupport::checkpoint_input(weights);
 REQUIRE_FALSE(r::detail::read_training_continuation(input).has_value());
}
void test_current_continuation_scalar_boundaries() {
 const auto invalid = std::to_array<std::pair<std::string, c10::IValue>>(
  {{"epoch", int64_t{-1}}, {"epoch", int64_t{std::numeric_limits<int>::max()}}, {"grad_scaler_scale", 0.0}, {"grad_scaler_scale", std::numeric_limits<double>::infinity()},
   {"grad_scaler_growth_tracker", int64_t{-1}}, {"grad_scaler_growth_tracker", int64_t{std::numeric_limits<int>::max()} + 1}, {"ema_completed_updates", int64_t{-1}},
   {"ema_completed_updates", std::numeric_limits<int64_t>::max()}, {"ema_completed_updates", int64_t{1}}, {"training_attempt_id", std::string{}}, {"training_attempt_id", std::string(65, 'a')},
   {"training_original_descriptor", std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'a')}});
 for (const auto& [key, value] : invalid) {
  auto source = continuation_fixture(saved_request());
  torch::serialize::OutputArchive output;
  r::testsupport::copy_checkpoint_archive(source, output, key);
  output.write(key, value);
  auto input = r::testsupport::checkpoint_input(output);
  INFO(key);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 for (const auto* key : {"training_configuration_cbor", "training_supervision_config_cbor"}) {
  auto source = continuation_fixture(saved_request());
  torch::serialize::OutputArchive output;
  r::testsupport::copy_checkpoint_archive(source, output, key);
  output.write(key, torch::zeros({1024 * 1024}, torch::kUInt8));
  auto input = r::testsupport::checkpoint_input(output);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 auto supervised = saved_request();
 supervised.training_supervision.denoising.enabled = true;
 auto supervised_source = continuation_fixture(supervised);
 REQUIRE(r::detail::read_training_continuation(supervised_source).has_value());
 torch::serialize::OutputArchive missing_supervision;
 r::testsupport::copy_checkpoint_archive(supervised_source, missing_supervision, "training_supervision_config_cbor");
 auto missing_input = r::testsupport::checkpoint_input(missing_supervision);
 REQUIRE_THROWS(r::detail::read_training_continuation(missing_input));
 for (auto optimizer : {r::TrainOptimizerKind::AdamW, r::TrainOptimizerKind::Muon, r::TrainOptimizerKind::SGD}) {
  for (bool ema : {false, true}) {
   auto request = saved_request();
   request.recipe.optimizer = optimizer;
   request.use_ema = ema;
   auto source = continuation_fixture(request);
   const auto admitted = r::detail::read_training_continuation(source);
   REQUIRE(admitted.has_value());
   REQUIRE(admitted->configuration == request);
   REQUIRE(admitted->values.ema_completed_updates == (ema ? 37 : 0));
   auto active = request;
   active.output_dir = "another-run";
   active.epochs += 10;
   REQUIRE_NOTHROW(r::detail::require_active_training_continuation(*admitted, active));
   active.recipe.optimizer = optimizer == r::TrainOptimizerKind::AdamW ? r::TrainOptimizerKind::Muon : r::TrainOptimizerKind::AdamW;
   REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
   active = request;
   active.recipe.lr_drop += 1;
   REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
   active = request;
   active.training_supervision.denoising.enabled = true;
   REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
   active = request;
   active.use_ema = !ema;
   REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
  }
 }
}
void test_ordered_cpu_ema_admission() {
 const std::vector<std::string> names{"first", "second"};
 const std::vector<torch::Tensor> parameters{torch::ones({2, 3}), torch::zeros({4})};
 const auto pointer = parameters.front().data_ptr();
 REQUIRE_NOTHROW(r::ModelEma::validate_cpu_shadow(parameters, parameters));
 for (int fault = 0; fault != 6; ++fault) {
  auto shadow = parameters;
  switch (fault) {
   case 0: shadow.pop_back(); break;
   case 1: shadow[0] = torch::Tensor{}; break;
   case 2: shadow[0] = torch::zeros({3, 2}); break;
   case 3: shadow[0] = shadow[0].to(torch::kFloat64); break;
   case 4: shadow[0] = torch::full({2, 3}, std::numeric_limits<float>::infinity()); break;
   case 5: shadow[0] = shadow[0].to_sparse(); break;
  }
  REQUIRE_THROWS(r::ModelEma::from_cpu_shadow(parameters, shadow, 0.9, 100, 37));
  REQUIRE(parameters.front().data_ptr() == pointer);
  REQUIRE(torch::equal(parameters.front(), torch::ones({2, 3})));
 }
 for (const auto count : {int64_t{-1}, std::numeric_limits<int64_t>::max()}) {
  REQUIRE_THROWS(r::ModelEma::from_cpu_shadow(parameters, parameters, 0.9, 100, count));
  REQUIRE(parameters.front().data_ptr() == pointer);
  REQUIRE(torch::equal(parameters.front(), torch::ones({2, 3})));
 }
 for (int fault = 0; fault != 5; ++fault) {
  torch::serialize::OutputArchive output;
  mmltk::backend::ml::serialization::write_int(output, "entry_count", fault == 1 ? 1 : 2);
  for (std::size_t i = 0; i != names.size(); ++i) {
   torch::serialize::OutputArchive entry;
   mmltk::backend::ml::serialization::write_string(entry, "name", names[fault == 2 ? 1 - i : fault == 4 ? 0 : i]);
   if (fault != 3) entry.write("tensor", parameters[i]);
   output.write(mmltk::backend::ml::serialization::archive_entry_name(i), entry);
  }
  auto input = r::testsupport::checkpoint_input(output);
  if (fault == 0) {
   const auto shadow = r::detail::read_ema_shadow_archive(input, names);
   REQUIRE_NOTHROW(r::ModelEma::validate_cpu_shadow(parameters, shadow));
  } else
   REQUIRE_THROWS(r::detail::read_ema_shadow_archive(input, names));
 }
}
}  // namespace
TEST_CASE("test_current_continuation_required_fields", "[model][rfdetr][training][continuation]") { test_current_continuation_required_fields(); }
TEST_CASE("test_current_continuation_scalar_boundaries", "[model][rfdetr][training][continuation]") { test_current_continuation_scalar_boundaries(); }
TEST_CASE("test_ordered_cpu_ema_admission", "[model][rfdetr][training][continuation][ema]") { test_ordered_cpu_ema_admission(); }
TEST_CASE("Continuation preserves held SGD values logical offsets donor identities and applied epoch latches", "[model][rfdetr][training][continuation]") {
 auto request = saved_request();
 request.recipe.optimizer = r::TrainOptimizerKind::SGD;
 r::reset_train_recipe(request.recipe);
 request.recipe.warmup_epochs = .375;
 request.epochs = 4;
 request.batch_size = 2;
 request.grad_accum_steps = 2;
 request.freeze_encoder = true;
 request.unfreeze_encoder_last_epochs = 4;
 request.disable_augmentation_last_epochs = 4;
 auto base = continuation_fixture(request);
 auto values = r::detail::read_training_continuation(base)->values;
 r::TrainingSchedule schedule(request.recipe, {.01, .01}, {r::TrainingGroupRole::Bias, r::TrainingGroupRole::Ordinary}, 4, 4, 2);
 schedule.begin_epoch(0);
 schedule.consume_microbatch();
 schedule.consume_microbatch();
 schedule.prepare_attempt();
 schedule.finish_attempt(false);
 values.schedule = schedule.state();
 values.epoch_policy = {true, true};
 values.data.epoch = 0;
 values.data.next_microbatch = 2;
 values.epoch_metrics = {2, 3.0, 1.0, 2.0, {}};
 values.data.donors = {{7, 3, true}, {2, 9, true}};
 auto input = continuation_fixture(request, values);
 const auto saved = r::detail::read_training_continuation(input);
 REQUIRE(saved);
 CHECK(saved->configuration == request);
 CHECK(saved->values.schedule == values.schedule);
 CHECK(saved->values.data == values.data);
 CHECK(saved->values.epoch_metrics.microbatches == 2);
 CHECK(saved->values.epoch_metrics.loss_sum == 3.0);
 CHECK(saved->values.epoch_policy == values.epoch_policy);
 auto extended = request;
 extended.epochs = 12;
 REQUIRE_NOTHROW(r::detail::require_active_training_continuation(*saved, extended));
 r::TrainingSchedule restored(extended.recipe, {.01, .01}, {r::TrainingGroupRole::Bias, r::TrainingGroupRole::Ordinary}, extended.epochs, 8, 2);
 restored.restore(saved->values.schedule);
 restored.begin_epoch(0);
 CHECK(restored.state() == schedule.state());
 r::TrainingEpochPolicy policy(extended.unfreeze_encoder_last_epochs, extended.disable_augmentation_last_epochs, saved->values.epoch_policy);
 CHECK(policy.enter(1, extended.epochs) == values.epoch_policy);
 extended.disable_augmentation_last_epochs = 3;
 REQUIRE_THROWS(r::detail::require_active_training_continuation(*saved, extended));
 for (int fault = 0; fault < 6; ++fault) {
  auto invalid = values;
  switch (fault) {
   case 0: ++invalid.data.next_microbatch; break;
   case 1: ++invalid.schedule.consumed_attempts; break;
   case 2: invalid.schedule.successful_updates = 2; break;
   case 3: ++invalid.schedule.warmup_microbatches; break;
   case 4: invalid.data.donors.pop_back(); break;
   case 5: ++invalid.epoch_metrics.microbatches; break;
  }
  INFO(fault);
  REQUIRE_THROWS(continuation_fixture(request, invalid));
 }
}
TEST_CASE("Resolved current Resume preserves recipe optimizer EMA and an unfinished stock warmup", "[model][rfdetr][training][continuation][parity]") {
 const mmltk::testsupport::ScopedTempDir root("stock-resume-admission");
 const auto path = root.path() / "resume.pt";
 r::DecodedNativeModelState state;
 state.metadata.preset_name = "rf-detr-seg-medium";
 state.metadata.source_kind = "stock-admission-fixture";
 state.metadata.source_path = path.string();
 state.metadata.num_classes = 3;
 state.metadata.num_queries = 2;
 state.metadata.num_select = 2;
 state.metadata.class_layout = r::testsupport::synthetic_training_layout(2);
 state.replace_entries(
  {{"query_feat.weight", torch::zeros({26, 256})}, {"refpoint_embed.weight", torch::zeros({26, 4})}, {"class_embed.weight", torch::zeros({3, 256})},
   {"class_embed.bias", torch::tensor({0.3F, -0.2F, 0.7F})}});
 state.metadata.cls_loss_coef = 7.3;
 state.metadata.bbox_loss_coef = 9.1;
 state.metadata.giou_loss_coef = 3.2;
 state.metadata.mask_ce_loss_coef = 8.2;
 state.metadata.mask_dice_loss_coef = 6.4;
 const std::string name = "class_embed.bias";
 torch::Tensor weight;
 for (const auto& entry : state.entries())
  if (entry.name == name) weight = entry.tensor;
 REQUIRE(weight.defined());
 weight.set_requires_grad(true);
 auto request = saved_request();
 request.use_ema = true;
 request.recipe.warmup_epochs = 1.5;
 request.ema_decay = 0.9;
 request.ema_tau = 3;
 torch::OrderedDict<std::string, torch::Tensor> inventory;
 inventory.insert(name, weight);
 auto built = r::build_optimizer(inventory, request);
 r::ModelEma ema({weight}, request.ema_decay, request.ema_tau);
 for (int attempt = 0; attempt < 3; ++attempt) {
  weight.mutable_grad() = torch::full_like(weight, 0.2 + attempt * 0.1);
  built.optimizer.step();
  built.optimizer.zero_grad(true);
  ema.update();
 }
 const auto saved_weight = weight.detach().clone(), saved_shadow = ema.shadow_params()[0].clone();
 const std::vector<r::NormalizedModelStateEntry> shadow{{name, saved_shadow}};
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
 readback.Begin();
 r::detail::reserve_state_archive(state.entries(), readback, 0);
 r::detail::reserve_state_archive(shadow, readback, state.entries().size());
 built.optimizer.reserve_checkpoint(readback, state.entries().size() + 1);
 torch::serialize::OutputArchive archive;
 r::detail::write_native_checkpoint_metadata(archive, state.metadata);
 r::detail::write_training_continuation(archive, request,
  r::testsupport::continuation_values(request,
   {.epoch = 0, .grad_scaler_scale = 128, .grad_scaler_growth_tracker = 7, .ema_completed_updates = 3, .training_attempt_id = "stock-admission", .training_original_descriptor = "original.json"}));
 r::detail::write_resume_state_archive(archive, "state", state.entries(), readback, 0);
 r::detail::write_resume_state_archive(archive, "ema_state", shadow, readback, state.entries().size());
 torch::serialize::OutputArchive optimizer_archive;
 built.optimizer.save(optimizer_archive, readback, state.entries().size() + 1);
 archive.write("optimizer", optimizer_archive);
 readback.Complete();
 r::detail::publish_native_checkpoint_archive(archive, path);
 auto admitted = r::resolve_model_state(path, {}, 0);
 auto active = request;
 active.resume_path = path;
 const auto continuation = r::detail::read_training_continuation(*admitted.model_state.admitted_archive());
 REQUIRE(continuation);
 r::detail::require_active_training_continuation(*continuation, active);
 REQUIRE(continuation.has_value());
 REQUIRE(admitted.artifacts.config.cls_loss_coef == 7.3);
 REQUIRE(admitted.artifacts.config.bbox_loss_coef == 9.1);
 REQUIRE(admitted.artifacts.config.giou_loss_coef == 3.2);
 REQUIRE(admitted.artifacts.config.mask_ce_loss_coef == 8.2);
 REQUIRE(admitted.artifacts.config.mask_dice_loss_coef == 6.4);
 auto resumed_weight = saved_weight.clone().set_requires_grad(true);
 torch::OrderedDict<std::string, torch::Tensor> resumed_inventory;
 resumed_inventory.insert(name, resumed_weight);
 auto resumed = r::build_optimizer(resumed_inventory, active);
 auto admitted_resume = r::load_resume_checkpoint_state(path, admitted.model_state, *continuation, resumed.optimizer, {name}, {resumed_weight});
 REQUIRE(admitted_resume.start_epoch == 1);
 REQUIRE(admitted_resume.scaler_scale == 128);
 REQUIRE(admitted_resume.scaler_growth_tracker == 7);
 REQUIRE(admitted_resume.ema_cpu_shadow.has_value());
 auto restored_ema = r::ModelEma::from_cpu_shadow({resumed_weight}, *admitted_resume.ema_cpu_shadow, active.ema_decay, active.ema_tau, continuation->values.ema_completed_updates);
 REQUIRE(restored_ema.completed_updates() == 3);
 REQUIRE(torch::equal(restored_ema.shadow_params()[0], saved_shadow));
 REQUIRE(torch::equal(resumed_weight, saved_weight));
 resumed.optimizer.commit(std::move(*admitted_resume.optimizer_candidate));
 r::TrainRecipeSettings schedule;
 schedule.warmup_epochs = continuation->configuration.recipe.warmup_epochs;
 schedule.lr_scheduler = r::TrainLrSchedulerKind::Step;
 schedule.lr_drop = 10;
 const auto resumed_step = admitted_resume.start_epoch * 3;
 REQUIRE(r::compute_lr_scale(schedule, resumed_step, 3, 12) == 0.75);  // floor(3*1.5)=4.
 built.optimizer.set_lrs(built.base_lrs, 0.75);
 resumed.optimizer.set_lrs(resumed.base_lrs, r::compute_lr_scale(schedule, resumed_step, 3, 12));
 weight.mutable_grad() = torch::full_like(weight, 0.43);
 resumed_weight.mutable_grad() = torch::full_like(resumed_weight, 0.43);
 built.optimizer.step();
 resumed.optimizer.step();
 ema.update();
 restored_ema.update();
 REQUIRE(torch::equal(weight, resumed_weight));
 REQUIRE(torch::equal(ema.shadow_params()[0], restored_ema.shadow_params()[0]));
 auto invalid = active;
 invalid.recipe.warmup_epochs = 2.0;
 REQUIRE_THROWS(r::detail::require_active_training_continuation(*continuation, invalid));
 REQUIRE(admitted.artifacts.config.cls_loss_coef == 7.3);
 torch::serialize::OutputArchive weights_only;
 mmltk::backend::ml::serialization::write_string(weights_only, "source_kind", "weights-only");
 auto input = r::testsupport::checkpoint_input(weights_only);
 REQUIRE_FALSE(r::detail::read_training_continuation(input));
 REQUIRE(admitted.artifacts.config.mask_ce_loss_coef == 8.2);
 // Starting a new run from this full archive deliberately ignores continuation.
 r::apply_stock_training_coefficients(admitted.artifacts.config);
 REQUIRE(admitted.artifacts.config.cls_loss_coef == 1.0);
 REQUIRE(admitted.model_state.metadata.cls_loss_coef == 7.3);
}
TEST_CASE("Continuation donor admission validates every descriptor before changing history", "[model][rfdetr][training][continuation]") {
 namespace data = mmltk::backend::data;
 namespace fixture = data::testsupport;
 mmltk::testsupport::ScopedTempDir root("continuation-donors");
 fixture::FixtureSpec spec;
 spec.root_dir = root.path().string();
 spec.num_images = 3;
 spec.background_images = 0;
 fixture::create_synthetic_dataset(spec);
 const auto crowd_path = std::filesystem::path(fixture::dataset_dir(spec)) / spec.split / "000003.jsonl";
 nlohmann::json crowd;
 {
  std::ifstream input(crowd_path);
  input >> crowd;
 }
 crowd["iscrowd"] = 1;
 {
  std::ofstream output(crowd_path);
  output << crowd.dump() << '\n';
 }
 fixture::compile_existing_fixture(spec);
 data::DatasetLoader::Config config;
 config.compiled_path = fixture::compiled_bin_path(spec);
 config.batch_size = 2;
 data::DatasetLoader loader(config);
 auto request = saved_request();
 request.batch_size = 2;
 auto base = continuation_fixture(request);
 auto values = r::detail::read_training_continuation(base)->values;
 values.data.donors = {{0, 0, true}, {1, 0, true}};
 r::TrainingDonorHistory history(1, 2);
 history.restore(loader, values.data.donors);
 const auto accepted = history.state();
 const std::array<std::uint64_t, 2> keys{41, 42};
 const std::array<std::uint32_t, 2> images{0, 1};
 const auto planned = history.plan(0, keys, images);
 const std::vector<r::TrainingDonorDescriptor> expected(planned.begin(), planned.end());
 auto valid_archive = continuation_fixture(request, values);
 const auto valid = r::detail::read_training_continuation(valid_archive);
 REQUIRE(valid);
 CHECK(valid->values.data == values.data);
 r::TrainingDonorHistory resumed(1, 2);
 resumed.restore(loader, valid->values.data.donors);
 CHECK(std::ranges::equal(resumed.plan(0, keys, images), expected));
 const auto augmentation = r::test_support::isolated_augmentation_config(1.F);
 const auto future = history.prepare(loader, 0, keys, images, augmentation);
 const std::vector<r::TrainingDonorDescriptor> future_donors(future.begin(), future.end());
 CHECK(history.state() == accepted);
 // Archives produced while a future plan exists contain only admitted slots.
 values.data.donors = history.state();
 auto pending_archive = continuation_fixture(request, values);
 const auto pending_saved = r::detail::read_training_continuation(pending_archive);
 REQUIRE(pending_saved);
 resumed.restore(loader, pending_saved->values.data.donors);
 CHECK(std::ranges::equal(resumed.prepare(loader, 0, keys, images, augmentation), future_donors));
 history.commit();
 resumed.commit();
 CHECK(history.state() == resumed.state());
 history.restore(loader, accepted);
 (void)history.prepare(loader, 0, keys, images, augmentation);
 history.discard();
 CHECK_THROWS(history.commit());
 CHECK(history.state() == accepted);
 for (const auto invalid : {r::TrainingDonorDescriptor{3, 0, true}, r::TrainingDonorDescriptor{1, 1, true}, r::TrainingDonorDescriptor{2, 0, true}}) {
  values.data.donors = {{1, 0, true}, invalid};
  auto archive = continuation_fixture(request, values);
  const auto restored = r::detail::read_training_continuation(archive);
  REQUIRE(restored);
  REQUIRE_THROWS(history.restore(loader, restored->values.data.donors));
  CHECK((history.state() == accepted));
 }
 values.data.donors = {{std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), false}, {1, 0, true}};
 history.restore(loader, values.data.donors);
 CHECK((history.state() == values.data.donors));
 REQUIRE_THROWS(history.restore(loader, {}));
 CHECK((history.state() == values.data.donors));
}
TEST_CASE("Logical donor admission preserves original RLE support and empty-mask meaning", "[model][rfdetr][training][continuation]") {
 namespace data = mmltk::backend::data;
 namespace fixture = data::testsupport;
 mmltk::testsupport::ScopedTempDir root("logical-donor-support");
 fixture::FixtureSpec spec;
 spec.root_dir = root.path().string();
 spec.num_images = 4;
 spec.background_images = 0;
 spec.width = spec.height = 8;
 fixture::create_synthetic_dataset(spec);
 const std::array annotations{
  R"({"class":"person","bbox_xyxy":[3,3,4,4],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:2 8:1","ignore":true})", R"({"class":"person","bbox_xyxy":[1,1,7,7]})",
  R"({"class":"person","bbox_xyxy":[1,1,7,7],"mask_rle_encoding":"row_major_start_length","mask_rle":""})", R"({"class":"person","bbox_xyxy":[1,1,7,7],"iscrowd":true})"
 };
 for (std::size_t image = 0; image < annotations.size(); ++image) {
  std::ofstream output(std::filesystem::path(fixture::dataset_dir(spec)) / spec.split / ("00000" + std::to_string(image + 1) + ".jsonl"));
  output << annotations[image] << '\n';
 }
 fixture::compile_existing_fixture(spec);
 data::DatasetLoader::Config loader_config;
 loader_config.compiled_path = fixture::compiled_bin_path(spec);
 loader_config.batch_size = 1;
 data::DatasetLoader loader(loader_config);
 const r::TrainingDonorDescriptor masked{0, 0, true}, box{1, 0, true}, empty{2, 0, true};
 const auto source = r::resolve_training_donor(loader, masked);
 const auto& instance = loader.label_data()[loader.label_index()[0].label_begin];
 CHECK(instance.raw_ignore());
 CHECK(source.support.data() == loader.rle_data() + instance.mask_rle_offset / sizeof(data::RLEPair));
 const std::array expected_support{data::RLEPair{0, 2}, data::RLEPair{8, 1}};
 CHECK(std::ranges::equal(source.support, expected_support, [](const auto& a, const auto& b) { return a.start == b.start && a.length == b.length; }));
 CHECK(source.metadata.box == std::array<float, 4>{.375F, .375F, .5F, .5F});
 CHECK(source.metadata.area == 3.F);
 CHECK(source.metadata.has_mask);
 const auto box_source = r::resolve_training_donor(loader, box), empty_source = r::resolve_training_donor(loader, empty);
 CHECK_FALSE(box_source.metadata.has_mask);
 CHECK(box_source.metadata.area == 36.F);
 CHECK(empty_source.metadata.has_mask);
 CHECK(empty_source.metadata.area == 0.F);
 CHECK(empty_source.support.empty());
 CHECK(box_source.metadata.box == empty_source.metadata.box);
 REQUIRE_THROWS_WITH(r::resolve_training_donor(loader, {3, 0, true}), "crowd annotation cannot enter logical donor history");
 r::TrainingDonorHistory history(1, 1);
 auto config = r::test_support::spatial_occlusion_config(true);
 config.resize = {1, 1, 1};
 // Keep admission enabled while isolating source visibility from pasted pixels.
 config.copy_paste_probability = std::numeric_limits<float>::min();
 std::size_t visible = 0, hidden = 0, differs_from_box = 0;
 for (std::uint64_t key = 0; key < 256; ++key) {
  REQUIRE_FALSE(r::augmentation_paste_admitted(config, key));
  const auto plan = r::plan_augmentation_image(config, key, 0, nullptr, -1);
  const auto original = r::map_augmentation_instance(instance, 8, 8, &plan, source.support);
  auto box_only = instance;
  box_only.flags &= ~data::kAnnotationMask;
  box_only.mask_rle_pairs = 0;
  differs_from_box += original.visible != r::map_augmentation_instance(box_only, 8, 8, &plan).visible;
  history.replace(0, std::array{box});
  const auto donors = history.prepare(loader, 0, std::array{key}, std::array<std::uint32_t, 1>{0}, config);
  CHECK(donors[0] == box);
  CHECK(history.state()[0] == box);
  history.commit();
  CHECK_THROWS(history.commit());
  CHECK(history.state()[0] == (original.visible ? masked : box));
  visible += original.visible;
  hidden += !original.visible;
 }
 // A resumed history makes exactly the same subsequent admission decisions;
 // physical cache contents never enter checkpoint vocabulary or logical draws.
 r::TrainingDonorHistory resumed(1, 1);
 resumed.restore(loader, history.state());
 for (std::uint64_t key = 256; key < 288; ++key) {
  const auto index = static_cast<std::uint32_t>(key % 3);
  const auto expected = history.prepare(loader, 0, std::array{key}, std::array{index}, config)[0];
  const auto actual = resumed.prepare(loader, 0, std::array{key}, std::array{index}, config)[0];
  CHECK(actual == expected);
  CHECK(resumed.state() == history.state());
  history.commit();
  resumed.commit();
  CHECK(resumed.state() == history.state());
 }
 CHECK(visible > 0);
 CHECK(hidden > 0);
 CHECK(differs_from_box > 0);
 config = r::test_support::isolated_augmentation_config(std::numeric_limits<float>::min());
 for (const auto descriptor : {box, empty}) {
  (void)history.prepare(loader, 0, std::array<std::uint64_t, 1>{42}, std::array{descriptor.image_index}, config);
  history.commit();
  CHECK(history.state()[0] == descriptor);
 }
 const auto retained = history.state();
 (void)history.prepare(loader, 0, std::array<std::uint64_t, 1>{42}, std::array<std::uint32_t, 1>{3}, config);
 history.commit();
 CHECK((history.state() == retained));
 REQUIRE_THROWS_WITH(history.prepare(loader, 0, std::array<std::uint64_t, 1>{42}, std::array<std::uint32_t, 1>{4}, config), "logical donor source image is outside dataset");
 CHECK((history.state() == retained));
 REQUIRE_THROWS(history.commit());
}
