#include "src/backend/ml/torch/tests/catch_support.h"
#include <array>
#include <limits>
#include <string>
#include <vector>
#include <utility>
#include "src/backend/ml/torch/archive.h"
#include "detail/checkpoint_private.h"
#include "detail/model_ema.h"
#include "detail/training_continuation.h"
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
torch::serialize::InputArchive continuation_fixture(const r::TrainRequest& request) {
 torch::serialize::OutputArchive output;
 r::detail::write_training_continuation(output, request,
  {.epoch = 0,
   .best_regular_metric = -std::numeric_limits<double>::infinity(),
   .best_ema_metric = -std::numeric_limits<double>::infinity(),
   .grad_scaler_scale = 1024.0,
   .grad_scaler_growth_tracker = 17,
   .ema_completed_updates = request.use_ema ? 37 : 0,
   .training_attempt_id = "attempt",
   .training_original_descriptor = "original.json"});
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
 const std::array<std::pair<std::string, c10::IValue>, 22> invalid{{{"epoch", int64_t{-1}}, {"epoch", int64_t{std::numeric_limits<int>::max()}}, {"grad_scaler_scale", 0.0},
  {"grad_scaler_scale", std::numeric_limits<double>::infinity()}, {"grad_scaler_growth_tracker", int64_t{-1}}, {"grad_scaler_growth_tracker", int64_t{std::numeric_limits<int>::max()} + 1},
  {"ema_completed_updates", int64_t{-1}}, {"ema_completed_updates", std::numeric_limits<int64_t>::max()}, {"ema_completed_updates", int64_t{1}},
  {"best_regular_metric", std::numeric_limits<double>::quiet_NaN()}, {"best_ema_metric", std::numeric_limits<double>::quiet_NaN()}, {"training_attempt_id", std::string{}},
  {"training_attempt_id", std::string(65, 'a')}, {"training_original_descriptor", std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'a')}, {"warmup_epochs", 0.25},
  {"warmup_momentum", 0.25}, {"lr_min_factor", 0.25}, {"lr_drop", int64_t{7}}, {"lr_scheduler", std::string("cosine")}, {"gpu_augment_perceptual_downscale", false},
  {"optimizer_kind", std::string("invalid")}, {"gpu_augment_geometry_probability", 0.125}}};
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
 for (auto optimizer : {r::TrainOptimizerKind::AdamW, r::TrainOptimizerKind::Muon}) {
  for (bool ema : {false, true}) {
   auto request = saved_request();
   request.optimizer = optimizer;
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
   active.optimizer = optimizer == r::TrainOptimizerKind::AdamW ? r::TrainOptimizerKind::Muon : r::TrainOptimizerKind::AdamW;
   REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
   active = request;
   active.lr_drop += 1;
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
 state.replace_entries({{"query_feat.weight", torch::zeros({26, 256})}, {"refpoint_embed.weight", torch::zeros({26, 4})}, {"class_embed.weight", torch::zeros({3, 256})},
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
 request.warmup_epochs = 1.5;
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
  {.epoch = 0,
   .best_regular_metric = 0.2,
   .best_ema_metric = 0.3,
   .grad_scaler_scale = 128,
   .grad_scaler_growth_tracker = 7,
   .ema_completed_updates = 3,
   .training_attempt_id = "stock-admission",
   .training_original_descriptor = "original.json"});
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
 const auto continuation = r::detail::admit_training_configuration(admitted.artifacts.config, admitted.model_state.admitted_archive(), active);
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
 auto admitted_resume = r::load_resume_checkpoint_state(path, admitted.model_state, *continuation, resumed.optimizer, active, {name}, {resumed_weight}, true);
 REQUIRE(admitted_resume.start_epoch == 1);
 REQUIRE(admitted_resume.scaler_scale == 128);
 REQUIRE(admitted_resume.scaler_growth_tracker == 7);
 REQUIRE(admitted_resume.restored_ema.has_value());
 REQUIRE(admitted_resume.restored_ema->completed_updates() == 3);
 REQUIRE(torch::equal(admitted_resume.restored_ema->shadow_params()[0], saved_shadow));
 REQUIRE(torch::equal(resumed_weight, saved_weight));
 resumed.optimizer.commit(std::move(*admitted_resume.optimizer_candidate));
 r::LrScheduleConfig schedule;
 schedule.warmup_epochs = continuation->configuration.warmup_epochs;
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
 admitted_resume.restored_ema->update();
 REQUIRE(torch::equal(weight, resumed_weight));
 REQUIRE(torch::equal(ema.shadow_params()[0], admitted_resume.restored_ema->shadow_params()[0]));
 auto invalid = active;
 invalid.warmup_epochs = 2.0;
 REQUIRE_THROWS(r::detail::admit_training_configuration(admitted.artifacts.config, admitted.model_state.admitted_archive(), invalid));
 REQUIRE(admitted.artifacts.config.cls_loss_coef == 7.3);
 REQUIRE_THROWS(r::detail::admit_training_configuration(admitted.artifacts.config, nullptr, active));
 torch::serialize::OutputArchive weights_only;
 mmltk::backend::ml::serialization::write_string(weights_only, "source_kind", "weights-only");
 auto input = r::testsupport::checkpoint_input(weights_only);
 REQUIRE_THROWS(r::detail::admit_training_configuration(admitted.artifacts.config, &input, active));
 REQUIRE(admitted.artifacts.config.mask_ce_loss_coef == 8.2);
 // Starting a new run from this full archive deliberately ignores continuation.
 REQUIRE_FALSE(r::detail::admit_training_configuration(admitted.artifacts.config, admitted.model_state.admitted_archive(), request).has_value());
 REQUIRE(admitted.artifacts.config.cls_loss_coef == 1.0);
 REQUIRE(admitted.model_state.metadata.cls_loss_coef == 7.3);
}
