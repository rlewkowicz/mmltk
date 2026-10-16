#include "src/backend/ml/torch/tests/catch_support.h"
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <ATen/Context.h>
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "detail/training_session_checkpoint.h"
#include "src/common/io/file_digest.h"
#include <array>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "src/backend/data/tests/test_fixture.h"
#include <limits>
#include <map>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include "src/backend/ml/torch/archive.h"
#include "src/backend/ml/torch/archive_cbor.h"
#include "detail/checkpoint_private.h"
#include "detail/model_ema.h"
#include "detail/training_continuation.h"
#include "detail/training_data_plan.h"
#include "src/backend/data/loading/dataset_loader.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
#include "src/backend/models/rfdetr/augmentation/tests/gpu_augment_test_support.h"
#include "detail/training_snapshot.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "training_continuation_fixture.h"
#include "encoder_training_fixture.h"
#include "src/backend/models/rfdetr/core/encoder_pretraining.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "model_state_fixture.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
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
torch::serialize::InputArchive weights_only_fixture() {
 torch::serialize::OutputArchive weights;
 mmltk::backend::ml::serialization::write_string(weights, "source_kind", "weights-only");
 return r::testsupport::checkpoint_input(weights);
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
 auto input = weights_only_fixture();
 REQUIRE_FALSE(r::detail::read_training_continuation(input).has_value());
}
template <class Value>
auto corrupted_continuation(const std::string& key, Value value) {
 auto source = continuation_fixture(saved_request());
 torch::serialize::OutputArchive output;
 r::testsupport::copy_checkpoint_archive(source, output, key);
 output.write(key, std::move(value));
 return r::testsupport::checkpoint_input(output);
}
void test_current_continuation_scalar_boundaries() {
 const auto invalid = std::to_array<std::pair<std::string, c10::IValue>>(
  {{"epoch", int64_t{-1}}, {"epoch", int64_t{std::numeric_limits<int>::max()}}, {"grad_scaler_scale", 0.0}, {"grad_scaler_scale", std::numeric_limits<double>::infinity()},
   {"grad_scaler_growth_tracker", int64_t{-1}}, {"grad_scaler_growth_tracker", int64_t{std::numeric_limits<int>::max()} + 1}, {"ema_completed_updates", int64_t{-1}},
   {"ema_completed_updates", std::numeric_limits<int64_t>::max()}, {"ema_completed_updates", int64_t{1}}, {"training_attempt_id", std::string{}}, {"training_attempt_id", std::string(65, 'a')},
   {"training_original_descriptor", std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'a')}});
 for (const auto& [key, value] : invalid) {
  auto input = corrupted_continuation(key, value);
  INFO(key);
  REQUIRE_THROWS(r::detail::read_training_continuation(input));
 }
 for (const auto* key : {"training_configuration_cbor", "training_supervision_config_cbor"}) {
  auto input = corrupted_continuation(key, torch::zeros({1024 * 1024}, torch::kUInt8));
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
TEST_CASE("continuation CBOR adapter enforces exact byte item and depth budgets", "[model][rfdetr][training][continuation]") {
 namespace archive = mmltk::backend::ml::serialization;
 using Record = r::TrainingEpochPolicyState;
 const Record value{true, false};
 // One map, two keys and two bool values; both fields have fixed byte size.
 constexpr archive::CborArchiveBudget budget{.max_items = 5, .max_depth = 1};
 torch::serialize::OutputArchive output;
 archive::write_cbor_record(output, "epoch_policy", value, budget, "encode");
 auto input = r::testsupport::checkpoint_input(output);
 auto tensor = archive::require_tensor(input, "epoch_policy");
 constexpr auto capacity = mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<Record>();
 REQUIRE(static_cast<std::size_t>(tensor.numel()) == capacity);
 CHECK(archive::read_cbor_record<Record>(tensor, budget, "extent", "decode") == value);
 for (const auto limit : {archive::CborArchiveBudget{4, 1}, archive::CborArchiveBudget{5, 0}}) {
  torch::serialize::OutputArchive rejected;
  REQUIRE_THROWS_WITH(archive::write_cbor_record(rejected, "epoch_policy", value, limit, "encode"), "encode");
  REQUIRE_THROWS_WITH(archive::read_cbor_record<Record>(tensor, limit, "extent", "decode"), "decode");
 }
 for (const auto& invalid :
  {torch::Tensor{}, tensor.to(torch::kInt32), tensor.unsqueeze(0), torch::empty({0}, torch::kUInt8), torch::zeros({}, torch::kUInt8),
   torch::zeros({static_cast<int64_t>(capacity + 1)}, torch::kUInt8)}) {
  REQUIRE_THROWS_WITH(archive::read_cbor_record<Record>(invalid, budget, "extent", "decode"), "extent");
 }
 REQUIRE_THROWS_WITH(archive::read_cbor_record<Record>(torch::zeros_like(tensor), budget, "extent", "decode"), "decode");
 // The admitted tensor retains the normalized allocation after the
 // noncontiguous backing leaves scope.
 auto retained = [&] {
  auto strided = r::testsupport::strided_checkpoint_bytes(tensor);
  REQUIRE_FALSE(strided.is_contiguous());
  REQUIRE_THROWS_WITH(archive::read_cbor_record<Record>(strided, budget, "extent", "decode", {.require_contiguous = true}), "extent");
  return archive::admit_cbor_tensor<Record>(strided, "extent");
 }();
 CHECK(retained.is_contiguous());
 CHECK(archive::read_cbor_record<Record>(retained, budget, "extent", "decode") == value);
}
TEST_CASE("saved requests normalize strided bytes while nested continuation records require contiguous storage", "[model][rfdetr][training][continuation]") {
 auto request = saved_request();
 request.train_compiled_path = "training//fixture.bin";
 auto source = continuation_fixture(request);
 const auto expected = r::detail::read_training_continuation(source);
 REQUIRE(expected);
 CHECK(expected->configuration.train_compiled_path.native() == "training/fixture.bin");
 for (const auto* key : {"training_configuration_cbor", "schedule", "epoch_policy", "data", "epoch_metrics", "execution"}) {
  INFO(key);
  const auto original = mmltk::backend::ml::serialization::require_tensor(source, key);
  auto strided = r::testsupport::strided_checkpoint_bytes(original);
  auto input = corrupted_continuation(key, strided);
  REQUIRE_FALSE(mmltk::backend::ml::serialization::require_tensor(input, key).is_contiguous());
  if (std::string_view(key) == "training_configuration_cbor") {
   const auto restored = r::detail::read_training_continuation(input);
   REQUIRE(restored);
   CHECK(restored->configuration == expected->configuration);
  } else {
   REQUIRE_THROWS_WITH(r::detail::read_training_continuation(input), "invalid continuation structure");
   auto empty = corrupted_continuation(key, torch::empty({0}, torch::kUInt8));
   REQUIRE_THROWS_WITH(r::detail::read_training_continuation(empty), "invalid continuation values");
  }
 }
 constexpr auto capacity = mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<r::TrainRequest>();
 for (const auto& invalid :
  {torch::empty({0}, torch::kUInt8), torch::zeros({1, 1}, torch::kUInt8), torch::zeros({1}, torch::kFloat32), torch::zeros({static_cast<int64_t>(capacity + 1)}, torch::kUInt8)}) {
  auto input = corrupted_continuation("training_configuration_cbor", invalid);
  REQUIRE_THROWS_WITH(r::detail::read_training_configuration(input), "invalid checkpoint training configuration");
 }
 auto malformed = corrupted_continuation("training_configuration_cbor", torch::full({1}, 255, torch::kUInt8));
 REQUIRE_THROWS(r::detail::read_training_configuration(malformed));
}
TEST_CASE("checkpoint CBOR readers reject device tensors before decoding", "[model][rfdetr][training][continuation][gpu]") {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA unavailable; checkpoint device admission unexecuted");
 auto request = saved_request();
 request.training_supervision.denoising.enabled = true;
 auto source = continuation_fixture(request);
 for (const auto* key : {"training_configuration_cbor", "epoch_policy", "training_supervision_config_cbor"}) {
  INFO(key);
  torch::serialize::OutputArchive output;
  r::testsupport::copy_checkpoint_archive(source, output, key);
  output.write(key, mmltk::backend::ml::serialization::require_tensor(source, key).to(torch::kCUDA));
  auto input = r::testsupport::checkpoint_input(output, std::nullopt);
  REQUIRE(mmltk::backend::ml::serialization::require_tensor(input, key).is_cuda());
  const auto* error = std::string_view(key) == "training_configuration_cbor" ? "invalid checkpoint training configuration"
                      : std::string_view(key) == "epoch_policy"              ? "invalid continuation structure"
                                                                             : "invalid RF-DETR training supervision CBOR blob";
  REQUIRE_THROWS_WITH(r::detail::read_training_continuation(input), error);
 }
}
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
 // CLEANUP-IGNORE: Schedule and continuation tests independently exercise the first epoch/microbatch boundary before different progression and restore checks.
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
 auto input = weights_only_fixture();
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
 // CLEANUP-IGNORE: Schedule and continuation tests copy a borrowed donor plan for different later mutation and restore assertions.
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
 CHECK((history.state() == accepted));
 // Archives produced while a future plan exists contain only admitted slots.
 values.data.donors = history.state();
 auto pending_archive = continuation_fixture(request, values);
 const auto pending_saved = r::detail::read_training_continuation(pending_archive);
 REQUIRE(pending_saved);
 resumed.restore(loader, pending_saved->values.data.donors);
 CHECK(std::ranges::equal(resumed.prepare(loader, 0, keys, images, augmentation), future_donors));
 history.commit();
 resumed.commit();
 CHECK((history.state() == resumed.state()));
 history.restore(loader, accepted);
 (void)history.prepare(loader, 0, keys, images, augmentation);
 history.discard();
 CHECK_THROWS(history.commit());
 CHECK((history.state() == accepted));
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
TEST_CASE("Logical donor admission preserves original RLE support and empty-mask meaning", "[model][rfdetr][training][continuation][data_plan]") {
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
 r::TrainRequest request;
 request.batch_size = 1;
 mmltk::common::concurrency::WorkerPool readers(2);
 const r::TrainingDataPlan serial_plan(loader, request);
 const r::TrainingDataPlan parallel_plan(loader, request, &readers);
 CHECK(parallel_plan.hash() == serial_plan.hash());
 CHECK((parallel_plan.shards() == serial_plan.shards()));
 const auto serial_epoch = serial_plan.epoch(0, 0);
 const auto parallel_epoch = parallel_plan.epoch(0, 0);
 CHECK(parallel_epoch.schedule->image_indices == serial_epoch.schedule->image_indices);
 CHECK(parallel_epoch.schedule->draw_keys == serial_epoch.schedule->draw_keys);
 CHECK(parallel_epoch.repeated_exposure == serial_epoch.repeated_exposure);
 const r::TrainingDonorDescriptor masked{0, 0, true}, box{1, 0, true}, empty{2, 0, true};
 const auto source = r::resolve_training_donor(loader, masked);
 const auto image_annotations = loader.annotations(0);
 const auto& instance = image_annotations.labels.front();
 CHECK(instance.raw_ignore());
 CHECK(source.support.data() == image_annotations.instance_rle(instance).data());
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
  CHECK((resumed.state() == history.state()));
  history.commit();
  resumed.commit();
  CHECK((resumed.state() == history.state()));
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
namespace {
enum class SessionTensorComparison { Exact, ContinuedFloat32 };
constexpr double kContinuedFloat32Rtol = 1.3e-6, kContinuedFloat32Atol = 1e-5;
bool session_tensor_equal(const torch::Tensor& actual, const torch::Tensor& expected, SessionTensorComparison comparison) {
 REQUIRE(actual.defined() == expected.defined());
 if (!expected.defined()) return true;
 REQUIRE(actual.scalar_type() == expected.scalar_type());
 REQUIRE(actual.sizes() == expected.sizes());
 if (comparison == SessionTensorComparison::ContinuedFloat32 && expected.scalar_type() == torch::kFloat32) {
  // torch.testing.assert_close's standard FP32 bounds apply to independently
  // recomputed trajectories. CUDA atomic backward accumulation rounds separately
  // across runs; checkpoint restoration itself still requires exact values.
  REQUIRE(torch::isfinite(actual).all().item<bool>());
  REQUIRE(torch::isfinite(expected).all().item<bool>());
  return torch::allclose(actual, expected, kContinuedFloat32Rtol, kContinuedFloat32Atol);
 }
 return torch::equal(actual, expected);
}
void trace_session_tensor_difference(std::string_view name, const torch::Tensor& actual, const torch::Tensor& expected) {
 const auto* diagnostic = std::getenv("MMLTK_TEST_SESSION_CONTINUATION_TRACE");
 if (!diagnostic || std::string_view(diagnostic) != "1" || !actual.defined() || !expected.defined() || actual.sizes() != expected.sizes() || !actual.numel() || torch::equal(actual, expected)) return;
 const auto a = actual.detach().cpu().to(torch::kFloat64), b = expected.detach().cpu().to(torch::kFloat64);
 const auto difference = a - b;
 const nlohmann::json record{
  {"event", "test.training_session_difference"}, {"tensor", name}, {"maximum_difference", difference.abs().max().item<double>()}, {"reference_maximum", b.abs().max().item<double>()},
  {"l2_difference", difference.norm().item<double>()}, {"reference_l2", b.norm().item<double>()},
  {"maximum_bound_ratio", (difference.abs() / (kContinuedFloat32Atol + kContinuedFloat32Rtol * b.abs())).max().item<double>()}
 };
 std::fprintf(stderr, "%s\n", record.dump().c_str());
}
void require_archive_values_equal(torch::serialize::InputArchive& actual, torch::serialize::InputArchive& expected, SessionTensorComparison comparison) {
 auto actual_keys = actual.keys(), expected_keys = expected.keys();
 std::ranges::sort(actual_keys);
 std::ranges::sort(expected_keys);
 REQUIRE(actual_keys == expected_keys);
 for (const auto& key : expected_keys) {
  INFO(key);
  torch::serialize::InputArchive a, b;
  if (expected.try_read(key, b)) {
   REQUIRE(actual.try_read(key, a));
   require_archive_values_equal(a, b, comparison);
  } else {
   c10::IValue left, right;
   actual.read(key, left);
   expected.read(key, right);
   if (right.isTensor()) {
    REQUIRE(left.isTensor());
    const bool equal = session_tensor_equal(left.toTensor(), right.toTensor(), comparison);
    trace_session_tensor_difference(key, left.toTensor(), right.toTensor());
    CHECK(equal);
   } else
    CHECK(left == right);
  }
 }
}
void require_session_trajectory_equal(const r::TrainingSessionAdmission& actual, const r::TrainingSessionAdmission& expected, SessionTensorComparison comparison = SessionTensorComparison::Exact) {
 REQUIRE(actual.model_count() == expected.model_count());
 for (std::size_t model = 0; model < actual.model_count(); ++model) {
  const auto& left = actual.model(model).entries();
  const auto& right = expected.model(model).entries();
  REQUIRE(left.size() == right.size());
  for (std::size_t entry = 0; entry < left.size(); ++entry) {
   INFO(left[entry].name);
   REQUIRE(left[entry].name == right[entry].name);
   const bool equal = session_tensor_equal(left[entry].tensor, right[entry].tensor, comparison);
   trace_session_tensor_difference(left[entry].name, left[entry].tensor, right[entry].tensor);
   CHECK(equal);
  }
  const auto& a = actual.continuation(model).values;
  const auto& b = expected.continuation(model).values;
  CHECK(a.schedule == b.schedule);
  CHECK(a.data == b.data);
  CHECK(a.grad_scaler_scale == b.grad_scaler_scale);
  CHECK(a.grad_scaler_growth_tracker == b.grad_scaler_growth_tracker);
  CHECK(a.ema_completed_updates == b.ema_completed_updates);
  for (const auto name : {"optimizer", "ema_state"}) {
   torch::serialize::InputArchive archive_a, archive_b;
   const bool present = expected.model(model).admitted_archive()->try_read(name, archive_b);
   REQUIRE(actual.model(model).admitted_archive()->try_read(name, archive_a) == present);
   if (present) require_archive_values_equal(archive_a, archive_b, comparison);
  }
 }
}
}  // namespace
TEST_CASE("Original application sessions resume with immutable format ten identities and retained candidates", "[rfdetr][training][continuation][baseline][gpu]") {
 namespace io = mmltk::common::io;
 namespace data = mmltk::backend::data;
 const auto* configured = std::getenv("MMLTK_RFDETR_BASELINE_SESSION");
 if (!configured) SKIP("historical-session validation is opt-in: use --with-upstream or --env MMLTK_RFDETR_BASELINE_SESSION=PATH");
 INFO("The wrapper must prepare the provenance-verified 1c13fec8 session fixture before running this acceptance case");
 REQUIRE(configured != nullptr);
 const auto root = std::filesystem::canonical(configured);
 std::ifstream evidence_file(root / "baseline.json");
 REQUIRE(evidence_file.good());
 const auto evidence = nlohmann::json::parse(evidence_file);
 REQUIRE(evidence.at("format_version") == 1);
 REQUIRE(evidence.at("application_revision") == "1c13fec8b04036661a4028b8807d0e1cff42407d");
 REQUIRE(evidence.at("source_tree") == "ab06f768a4ad225f0d3fdd0985ea414b726402df");
 REQUIRE(evidence.at("source_tree_clean") == true);
 REQUIRE(evidence.at("build_sha256").get<std::string>().size() == 64);
 REQUIRE_FALSE(evidence.at("inputs").empty());
 REQUIRE_FALSE(evidence.at("files").empty());
 const auto admitted_path = [&](const std::filesystem::path& path) {
  const auto absolute = mmltk::testsupport::canonical_fixture_path(root, path);
  REQUIRE(absolute);
  return *absolute;
 };
 std::map<std::filesystem::path, std::string> hashes;
 for (const auto& record : evidence.at("files")) {
  const auto path = admitted_path(record.at("path").get<std::string>());
  REQUIRE(hashes.emplace(path, record.at("sha256").get<std::string>()).second);
 }
 const auto require_evidence = [&](const std::filesystem::path& path) {
  const auto absolute = admitted_path(path);
  REQUIRE(hashes.contains(absolute));
  return absolute;
 };
 const auto verify_files = [&] {
  for (const auto& record : evidence.at("files")) {
   const auto path = admitted_path(record.at("path").get<std::string>());
   INFO(path.string());
   CHECK(io::sha256_hex(io::sha256_file(path)) == record.at("sha256").get<std::string>());
   CHECK(std::filesystem::file_size(path) == record.at("bytes").get<std::uint64_t>());
  }
 };
 verify_files();
 const auto binary = require_evidence(evidence.at("build_path").get<std::string>());
 REQUIRE(hashes.at(binary) == evidence.at("build_sha256").get<std::string>());
 require_evidence(evidence.at("build_transcript").get<std::string>());
 require_evidence(evidence.at("producer_transcript").get<std::string>());
 for (const auto* role : {"source", "include", "identity_declarations", "link_options"}) require_evidence(evidence.at("harness").at(role).get<std::string>());
 REQUIRE(evidence.at("harness").size() == 4);
 for (const auto& input : evidence.at("inputs")) {
  const auto path = require_evidence(input.at("path").get<std::string>());
  CHECK(hashes.at(path) == input.at("sha256").get<std::string>());
 }
 const auto original = require_evidence(evidence.at("session").get<std::string>());
 const auto continued = require_evidence(evidence.at("continued_session").get<std::string>());
 std::ifstream manifest_file(original);
 const auto original_manifest = nlohmann::json::parse(manifest_file);
 REQUIRE_FALSE(original_manifest.contains("identity_schema"));
 std::ifstream continued_file(continued);
 REQUIRE_FALSE(nlohmann::json::parse(continued_file).contains("identity_schema"));
 r::TrainingSessionAdmission saved(original), expected(continued);
 REQUIRE(saved.manifest().identity_schema == 1);
 REQUIRE(expected.manifest().identity_schema == 1);
 for (const auto& [session, path] : std::array{std::pair{&saved, original}, std::pair{&expected, continued}}) {
  require_evidence(path.parent_path() / "generations" / session->manifest().generation / "plan.cbor");
  for (const auto& model : session->manifest().models) {
   require_evidence(path.parent_path() / model.path);
   if (model.best) require_evidence(model.best->path);
  }
  for (const auto& dataset : {session->manifest().request.train_compiled_path, session->manifest().request.val_compiled_path, session->manifest().request.test_compiled_path}) {
   REQUIRE_FALSE(dataset.empty());
   require_evidence(dataset);
  }
 }
 const auto selected_path = require_evidence(evidence.at("continued_selection").get<std::string>());
 const auto expected_selection = r::read_training_selection(selected_path);
 require_evidence(expected_selection.artifact.path);
 REQUIRE(std::ranges::any_of(saved.manifest().models, [](const auto& model) { return model.best.has_value(); }));
 for (std::size_t model = 0; model < saved.model_count(); ++model) {
  CHECK(mmltk::backend::ml::serialization::require_int(*saved.model(model).admitted_archive(), "format_version") == 4);
  if (const auto candidate = saved.best_admission(model)) {
   CHECK(mmltk::backend::ml::serialization::require_int(*candidate->decode()->admitted_archive(), "format_version") == 4);
   candidate->require_matches(*saved.manifest().models[model].best);
  }
 }
 const auto source_dataset = data::CompiledDataset::open(saved.manifest().request.train_compiled_path);
 const auto config = r::resolve_admitted_model_artifacts(saved.model(0), original.parent_path() / saved.manifest().models.front().path, saved.manifest().request.preset_name,
  static_cast<int>(source_dataset.header().image_width), saved.manifest().request.class_layout_path)
                      .config;
 const auto signature = r::training_configuration_identity(config, saved.model(0).metadata.class_layout, 1);
 CHECK(signature == evidence.at("configuration_signature").get<std::string>());
 for (const auto& [name, path] : std::array{std::pair{"configuration", saved.manifest().request.train_compiled_path}, std::pair{"validation", saved.manifest().request.val_compiled_path}}) {
  const auto source = data::CompiledDataset::open(path);
  CHECK(source.source_header().version == 10);
  const auto header_file = require_evidence(evidence.at("source_headers").at(name).get<std::string>());
  REQUIRE(std::filesystem::file_size(header_file) == sizeof(data::FileHeader));
  std::array<char, sizeof(data::FileHeader)> original_header{};
  std::ifstream header_input(header_file, std::ios::binary);
  header_input.read(original_header.data(), original_header.size());
  REQUIRE(header_input.good());
  CHECK(std::memcmp(original_header.data(), &source.source_header(), original_header.size()) == 0);
  const auto digest = r::training_dataset_identity(source.source_header(), io::FileSnapshot::Read(path), signature);
  CHECK(digest == original_manifest.at(name).get<std::string>());
 }
 const mmltk::testsupport::ScopedTempDir output("baseline-session-resume");
 auto request = saved.manifest().request;
 request.resume_path = original;
 request.weights_path.clear();
 request.output_dir = output.path() / "resumed";
 request.epochs = expected.manifest().request.epochs;
 const auto result = r::run_training(request);
 r::TrainingSessionAdmission actual(result.checkpoint_path);
 REQUIRE(actual.manifest().identity_schema == 2);
 require_session_trajectory_equal(actual, expected);
 REQUIRE(result.selected);
 CHECK(result.selected->artifact.content == expected_selection.artifact.content);
 CHECK(result.selected->artifact.selection_metric == expected_selection.artifact.selection_metric);
 CHECK(result.selected->method == expected_selection.method);
 REQUIRE(result.test_summary);
 auto changed = request;
 changed.output_dir = output.path() / "changed-recipe";
 changed.recipe.lr *= 2;
 REQUIRE_THROWS(r::run_training(changed));
 const auto changed_data = output.path() / "changed-train.bin";
 std::filesystem::copy_file(request.train_compiled_path, changed_data);
 std::filesystem::last_write_time(changed_data, std::filesystem::last_write_time(request.train_compiled_path) + std::chrono::seconds(1));
 changed = request;
 changed.output_dir = output.path() / "changed-data";
 changed.train_compiled_path = changed_data;
 REQUIRE_THROWS(r::run_training(changed));
 verify_files();
 saved.require_unchanged();
 expected.require_unchanged();
}
TEST_CASE("PE Base and keypoint sessions retain shared independent and averaged trajectories through final selection", "[rfdetr][training][continuation][lanes][gpu]") {
 namespace data = mmltk::backend::data;
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 const bool deterministic = at::globalContext().deterministicCuDNN(), benchmark = at::globalContext().benchmarkCuDNN();
 const mmltk::testsupport::ScopedTestCleanup restore_convolution([=] {
  at::globalContext().setDeterministicCuDNN(deterministic);
  at::globalContext().setBenchmarkCuDNN(benchmark);
 });
 at::globalContext().setDeterministicCuDNN(true);
 at::globalContext().setBenchmarkCuDNN(false);
 for (const auto preset : {"rf-detr-femto", "rf-detr-xlarge", "rf-detr-keypoint-preview"}) DYNAMIC_SECTION(preset) {
   CAPTURE(preset);
   const mmltk::testsupport::ScopedTempDir root("family-session");
   auto config = r::native_config_from_preset(*r::find_preset_catalog_entry(preset));
   config.resolution = config.patch_size * config.num_windows * 2;
   config.num_queries = config.num_select = 8;
   config.group_detr = 2;
   data::testsupport::FixtureSpec spec{.root_dir = root.path().string(), .width = config.resolution, .height = config.resolution, .num_images = 4, .background_images = 0};
   data::testsupport::create_compiled_fixture(spec, config.has_keypoints());
   auto source = data::CompiledDataset::open(data::testsupport::compiled_bin_path(spec));
   config.num_classes = static_cast<int>(source.class_catalog()->size()) + 1;
   if (config.has_keypoints()) config.keypoints_per_class = r::native_training_keypoint_counts(*source.class_catalog());
   const auto seed_path = root.path() / "seed.pt";
   {
    torch::manual_seed(123);
    r::NativeRfDetrModel model(config, r::native_training_class_layout(*source.class_catalog()));
    r::DecodedNativeModelState seed;
    seed.metadata = r::testsupport::model_checkpoint_metadata(model, "native-fixture", "seed.pt");
    seed.replace_entries(r::testsupport::clone_normalized_model_state(model));
    r::save_native_checkpoint(seed_path, seed);
   }
   for (const auto mode : {r::TrainLaneMode::SharedGradients, r::TrainLaneMode::Independent, r::TrainLaneMode::PeriodicAveraging}) DYNAMIC_SECTION("mode=" << static_cast<int>(mode)) {
     CAPTURE(mode);
     r::TrainRequest request;
     request.preset_name = preset;
     request.weights_path = seed_path;
     request.train_compiled_path = request.val_compiled_path = request.test_compiled_path = data::testsupport::compiled_bin_path(spec);
     request.output_dir = root.path() / std::to_string(static_cast<int>(mode)) / "initial";
     request.num_queries = config.num_queries;
     request.lanes = 2;
     request.workers = 4;
     request.batch_size = 1;
     request.val_batch_size = 3;
     request.compilation_mode = r::CompilationMode::kNone;
     request.amp = request.fused_optimizer = request.progress_bar = false;
     request.use_ema = true;
     request.validation_loss = true;
     request.freeze_encoder = true;
     request.lane_configuration.mode = mode;
     r::resize_training_models(request.lane_configuration, mode == r::TrainLaneMode::SharedGradients ? 1 : 2, request.recipe, request.seed);
     request.lane_configuration.final_policy = mode == r::TrainLaneMode::Independent ? r::TrainFinalPolicy::Uniform : r::TrainFinalPolicy::Off;
     const auto initial = r::run_training(request);
     REQUIRE(initial.selected);
     REQUIRE(initial.test_summary);
     request.resume_path = initial.checkpoint_path;
     request.weights_path.clear();
     request.epochs = 2;
     request.output_dir = request.output_dir.parent_path() / "resumed-a";
     const auto a = r::run_training(request);
     request.output_dir = request.output_dir.parent_path() / "resumed-b";
     const auto b = r::run_training(request);
     r::TrainingSessionAdmission left(a.checkpoint_path), right(b.checkpoint_path);
     require_session_trajectory_equal(left, right, SessionTensorComparison::ContinuedFloat32);
     REQUIRE(a.selected);
     REQUIRE(a.test_summary);
     CHECK(a.selected->artifact.selection_metric == r::training_selection_metric(a.selected->validation, config.task));
     if (config.has_keypoints()) {
      REQUIRE(a.test_summary->keypoint);
      REQUIRE(a.test_summary->keypoint->available);
      CHECK(r::checkpoint_configuration(left.model(0).metadata).keypoints_per_class == config.keypoints_per_class);
     }
    }
  }
}
TEST_CASE("legacy checkpoint requests migrate only absent encoder launch fields", "[rfdetr][training][continuation][encoder_pretraining]") {
 namespace serial = mmltk::frameworks::serialization;
 auto request = saved_request();
 auto legacy = nlohmann::json::parse(r::encode_train_request_json(request));
 legacy.erase("from_scratch");
 legacy.erase("encoder_weights_path");
 const auto read = [&](const nlohmann::json& document) {
  const auto bytes = serial::json_to_cbor(document.dump(), {.max_bytes = r::kMaximumTrainRequestJsonBytes, .max_items = 4096, .max_depth = 32});
  auto tensor = torch::empty({static_cast<int64_t>(bytes.size())}, torch::kUInt8);
  std::memcpy(tensor.data_ptr(), bytes.data(), bytes.size());
  torch::serialize::OutputArchive output;
  output.write("training_configuration_cbor", tensor);
  auto input = r::testsupport::checkpoint_input(output);
  return r::detail::read_training_configuration(input);
 };
 CHECK(read(legacy) == request);
 for (const auto* name : {"from_scratch", "encoder_weights_path"}) {
  auto partially_current = nlohmann::json::parse(r::encode_train_request_json(request));
  partially_current.erase(name);
  CHECK(read(partially_current) == request);
  auto invalid = legacy;
  invalid[name] = nullptr;
  REQUIRE_THROWS(read(invalid));
  invalid[name] = 1;
  REQUIRE_THROWS(read(invalid));
 }
 auto invalid = legacy;
 invalid.erase("epochs");
 REQUIRE_THROWS(read(invalid));
 invalid = legacy;
 invalid["new_source"] = true;
 REQUIRE_THROWS(read(invalid));
 request.weights_path.clear();
 request.from_scratch = true;
 request.preset_name = "rf-detr-atto";
 request.encoder_weights_path = "/launch-only/base.safetensors";
 auto source = continuation_fixture(request);
 const auto restored = r::detail::read_training_continuation(source);
 REQUIRE(restored);
 CHECK(restored->configuration.from_scratch);
 CHECK(restored->configuration.encoder_weights_path.empty());
 auto active = restored->configuration;
 r::normalize_train_resume_request(active, "/immutable/session.json");
 CHECK_NOTHROW(r::detail::require_active_training_continuation(*restored, active));
}
TEST_CASE("verified scratch sessions preserve fresh task state lineage and source independent Resume", "[rfdetr][training][continuation][encoder_pretraining][gpu]") {
 const auto evidence = r::testsupport::encoder_training_evidence();
 if (!evidence) SKIP("real encoder lifecycle requires explicit MMLTK_RFDETR_ENCODER_PRETRAINING_DIR");
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 for (const auto preset : {"rf-detr-atto", "rf-detr-seg-nano", "rf-detr-keypoint-preview", "rf-detr-xlarge"}) DYNAMIC_SECTION(preset) {
   const mmltk::testsupport::ScopedTempDir root("scratch-session");
   auto request = r::testsupport::encoder_training_request(root.path(), *evidence, preset);
   if (std::string_view(preset) == "rf-detr-atto") {
    request.num_queries = 0;
    request.lane_configuration.mode = r::TrainLaneMode::Independent;
    r::resize_training_models(request.lane_configuration, 2, request.recipe, request.seed);
    request.lane_configuration.final_policy = r::TrainFinalPolicy::Uniform;
   }
   request.freeze_encoder = std::string_view(preset) != "rf-detr-seg-nano";
   const auto result = r::run_training(request);
   REQUIRE(result.selected);
   REQUIRE(result.test_summary);
   const auto& artifacts = result.artifacts;
   const auto& provenance = artifacts.encoder_pretraining;
   REQUIRE(provenance);
   CHECK(*provenance == r::encoder_pretraining_provenance(*r::find_encoder_pretraining_asset(artifacts.config.backbone.kind)));
   CHECK(artifacts.input_path.empty());
   const auto source = mmltk::backend::data::CompiledDataset::open(request.train_compiled_path);
   CHECK(artifacts.class_layout == r::native_training_class_layout(*source.class_catalog()));
   CHECK(artifacts.config.num_classes == static_cast<int>(source.class_catalog()->size()) + 1);
   CHECK(artifacts.source_num_queries == r::native_config_from_preset(*r::find_preset_catalog_entry(preset)).num_queries);
   CHECK(artifacts.automatic_num_queries_cap == artifacts.source_num_queries);
   if (request.num_queries == 0) CHECK(std::cmp_equal(artifacts.config.num_queries, std::min<std::uint32_t>(source.header().max_instances_per_image * 2U + 1U, artifacts.automatic_num_queries_cap)));
   if (artifacts.config.has_keypoints()) CHECK(artifacts.config.keypoints_per_class == r::native_training_keypoint_counts(*source.class_catalog()));
   if (std::string_view(preset) == "rf-detr-atto") CHECK(artifacts.config.dec_layers == 0);
   // Reconstruct the seeded common model before any update. Its complete hash,
   // encoder state and fresh task heads are the session's original identity.
   torch::manual_seed(static_cast<std::uint64_t>(request.seed));
   r::NativeRfDetrModel initial(artifacts.config, artifacts.class_layout);
   initial.initialize_training_supervision(static_cast<std::uint64_t>(request.seed));
   const auto fresh_heads = initial.named_parameters(true)["class_embed.weight"].detach().clone();
   const auto loaded = r::initialize_encoder_pretraining(initial, request.encoder_weights_path);
   CHECK(torch::equal(initial.named_parameters(true)["class_embed.weight"], fresh_heads));
   CHECK(loaded.provenance == provenance);
   r::TrainingSessionAdmission session(result.checkpoint_path);
   CHECK(session.manifest().initialization == r::native_state_fingerprint(r::collect_module_state(initial)));
   r::testsupport::require_reusable_encoder_request(session.manifest().request);
   const auto parameters = initial.named_parameters(true);
   const auto buffers = initial.named_buffers(true);
   for (std::size_t model = 0; model < session.model_count(); ++model) {
    const auto& state = session.model(model);
    CHECK(state.metadata.encoder_pretraining == provenance);
    CHECK(state.metadata.source_path.empty());
    CHECK(session.continuation(model).configuration.from_scratch);
    CHECK(session.continuation(model).configuration.encoder_weights_path.empty());
    CHECK(session.continuation(model).values.schedule.consumed_attempts > 0);
    std::size_t changed_encoder_tensors = 0;
    for (const auto& value : state.entries())
     if (value.name.starts_with("backbone.0.encoder.")) {
      const auto* expected = parameters.find(value.name);
      if (!expected) expected = buffers.find(value.name);
      REQUIRE(expected);
      const bool equal = torch::equal(value.tensor, *expected);
      if (request.freeze_encoder) CHECK(equal);
      changed_encoder_tensors += !equal;
     }
    if (!request.freeze_encoder) CHECK(changed_encoder_tensors > 0);
   }
   if (session.model_count() > 1) {
    CHECK(session.plan().shards[0].seed != session.plan().shards[1].seed);
    CHECK(r::native_state_fingerprint(session.model(0).entries()) != r::native_state_fingerprint(session.model(1).entries()));
   }
   for (const auto& entry : std::filesystem::directory_iterator(request.output_dir))
    if (entry.path().extension() == ".pt") CHECK(r::decode_native_model_state(entry.path()).metadata.encoder_pretraining == provenance);
   CHECK(r::decode_native_model_state(result.selected->artifact.path).metadata.encoder_pretraining == provenance);
   const auto inspection = r::inspect_training_checkpoint(result.checkpoint_path).checkpoint();
   CHECK(inspection.encoder_pretraining == provenance);
   const auto initialization = session.manifest().initialization;
   std::filesystem::remove(request.encoder_weights_path);
   REQUIRE_FALSE(std::filesystem::exists(request.encoder_weights_path));
   r::normalize_train_resume_request(request, result.checkpoint_path);
   request.epochs = 2;
   request.output_dir = root.path() / "resumed";
   const auto continued = r::run_training(request);
   REQUIRE(continued.selected);
   r::TrainingSessionAdmission resumed(continued.checkpoint_path);
   CHECK(resumed.manifest().initialization == initialization);
   CHECK(resumed.manifest().request.from_scratch);
   CHECK(resumed.manifest().request.encoder_weights_path.empty());
   CHECK(resumed.model(0).metadata.encoder_pretraining == provenance);
   CHECK(resumed.continuation(0).values.schedule.consumed_attempts > session.continuation(0).values.schedule.consumed_attempts);
   // Ordinary Transfer records the emitted detector as input while retaining lineage.
   request.resume_path.clear();
   request.weights_path = continued.selected->artifact.path;
   request.epochs = 1;
   request.output_dir = root.path() / "transfer";
   const auto transferred = r::run_training(request);
   CHECK(transferred.artifacts.encoder_pretraining == provenance);
   r::TrainingSessionAdmission transfer(transferred.checkpoint_path);
   CHECK(transfer.model(0).metadata.source_path == std::filesystem::canonical(request.weights_path).string());
   CHECK_FALSE(transfer.manifest().request.from_scratch);
  }
}
