#include "src/backend/models/rfdetr/training/detail/training_artifact.h"
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/training/detail/model_merging.h"
#include "src/backend/models/rfdetr/training/detail/training_session_checkpoint.h"
#include "src/backend/models/rfdetr/training/detail/checkpoint_private.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
#include "src/backend/models/rfdetr/training/detail/training_ops_private.h"
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/common/io/file_digest.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "training_continuation_fixture.h"
#include <array>
#include <fstream>
#include <limits>
#include <memory>
namespace {
namespace r = mmltk::backend::models::rfdetr;
namespace io = mmltk::common::io;
r::EvalSummary evaluation(double metric, bool masks = false) {
 r::EvalSummary value;
 value.bbox.ap = metric;
 value.bbox.available = true;
 if (masks) {
  value.mask.emplace();
  value.mask->ap = metric;
  value.mask->available = true;
 }
 return value;
}
r::TrainingSelectionCandidate ingredient(const std::filesystem::path& directory, std::uint64_t id, double value, double metric, bool masks = false) {
 r::DecodedNativeModelState state;
 state.metadata = r::testsupport::synthetic_training_metadata();
 state.replace_entries({{"value", torch::tensor({value})}, {"counter", torch::tensor({3}, torch::kInt64)}});
 r::TrainingSelectionCandidate candidate;
 auto& artifact = candidate.artifact;
 artifact.path = directory / (std::to_string(id) + ".pt");
 r::save_native_checkpoint(artifact.path, state);
 artifact.model_id = id;
 artifact.session_id = "session";
 artifact.initialization = std::string(64, '1');
 artifact.configuration = std::string(64, '3');
 artifact.validation = std::string(64, '2');
 artifact.sha256 = io::sha256_hex(io::sha256_file(artifact.path));
 artifact.content = r::native_state_fingerprint(state.entries());
 artifact.selection_metric = metric;
 artifact.evaluation = evaluation(metric, masks);
 return candidate;
}
struct SessionFixture final {
 explicit SessionFixture(const std::filesystem::path& path) : checkpoint(path) {
  request.train_compiled_path = "train.bin";
  request.val_compiled_path = "val.bin";
  request.weights_path = "seed.pt";
  request.output_dir = path;
  request.epochs = 3;
  request.lanes = 3;
  request.recipe.warmup_epochs = 0;
  request.lane_configuration.mode = r::TrainLaneMode::Independent;
  r::resize_training_models(request.lane_configuration, 3, request.recipe, 42);
  request.lane_configuration.models[1].recipe.optimizer = r::TrainOptimizerKind::Muon;
  request.lane_configuration.models[2].recipe.optimizer = r::TrainOptimizerKind::SGD;
  manifest.session_id = "session";
  manifest.attempt_id = "attempt";
  manifest.initialization = std::string(64, '1');
  manifest.configuration = std::string(64, '2');
  manifest.validation = std::string(64, '3');
  manifest.epoch = 1;
  manifest.request = request;
  plan.plan_hash = 1;
  for (const auto& model : request.lane_configuration.models) {
   manifest.models.push_back({model.model_id, {}, {}, 0, {}});
   plan.shards.push_back({model.model_id, model.seed, {static_cast<std::uint32_t>(model.model_id - 1)}, {1}, {}});
  }
 }
 void publish(std::function_ref<void(r::TrainingPublicationStep)> observe = [](r::TrainingPublicationStep) {}) {
  checkpoint.publish(manifest, plan, [&](const std::filesystem::path& destination, std::size_t index) {
   torch::OrderedDict<std::string, torch::Tensor> parameters;
   parameters.insert("value", torch::ones({2}, torch::TensorOptions().requires_grad(true)));
   auto configuration = request;
   configuration.recipe = request.lane_configuration.models[index].recipe;
   auto optimizer = r::build_optimizer(parameters, configuration).optimizer;
   parameters["value"].mutable_grad() = torch::ones_like(parameters["value"]);
   optimizer.step();
   optimizer.zero_grad(true);
   auto values = r::testsupport::continuation_values(configuration, {.epoch = 0, .training_attempt_id = manifest.attempt_id});
   values.data.model_id = manifest.models[index].model_id;
   values.grad_scaler_scale = r::GradScaler::kInitialScale;
   optimizer.set_lrs(values.schedule.absolute_lrs, 1);
   optimizer.set_momentum(values.schedule.held_momentum);
   if (corrupt_model == index) ++values.data.model_id;
   torch::serialize::OutputArchive output;
   r::detail::write_native_checkpoint_metadata(output, r::testsupport::synthetic_training_metadata());
   r::detail::write_training_continuation(output, configuration, values);
   mmltk::backend::ml::cuda::TensorReadbackBuffers readback;
   readback.Begin();
   const std::vector<r::NormalizedModelStateEntry> state{{"value", parameters["value"]}};
   r::detail::reserve_state_archive(state, readback, 0);
   optimizer.reserve_checkpoint(readback, 1);
   r::detail::write_resume_state_archive(output, "state", state, readback, 0);
   torch::serialize::OutputArchive saved_optimizer;
   optimizer.save(saved_optimizer, readback, 1);
   output.write("optimizer", saved_optimizer);
   readback.Complete();
   r::detail::publish_native_checkpoint_archive(output, destination);
  }, admissions, observe);
 }
 std::vector<std::shared_ptr<const r::TrainingArtifactAdmission>> admissions;
 std::optional<std::size_t> corrupt_model;
 r::TrainRequest request;
 r::TrainingSessionManifest manifest;
 r::TrainingPlanState plan;
 r::TrainingSessionCheckpoint checkpoint;
};
}  // namespace
TEST_CASE("native averaging installs every receiver and admits complete values before mutation", "[model][rfdetr][training][merge]") {
 std::vector<r::NormalizedModelStateEntry> a{{"p", torch::tensor({1.F, 3.F})}, {"counter", torch::tensor(7, torch::kInt64)}};
 std::vector<r::NormalizedModelStateEntry> b{{"p", torch::tensor({5.F, 7.F})}, {"counter", torch::tensor(7, torch::kInt64)}};
 std::vector<r::NormalizedModelStateEntry> zero{{"p", torch::zeros({2})}, {"counter", torch::tensor(7, torch::kInt64)}};
 const std::array inputs{&a, &b, &zero};
 r::NativeModelAverage average;
 average.prepare(inputs, std::array{1., 3., 0.});
 average.install(inputs);
 CHECK(torch::equal(a[0].tensor, torch::tensor({4.F, 6.F})));
 CHECK(torch::equal(zero[0].tensor, a[0].tensor));
 auto pointer = average.values()[0].tensor.data_ptr();
 average.prepare(inputs, std::array{1., 1., 0.});
 CHECK(pointer == average.values()[0].tensor.data_ptr());
 b[1].tensor.fill_(8);
 REQUIRE_THROWS(average.prepare(inputs, std::array{1., 1., 0.}));
 CHECK(torch::equal(a[0].tensor, torch::tensor({4.F, 6.F})));
 b[1].tensor.fill_(7);
 b[0].tensor.fill_(std::numeric_limits<float>::quiet_NaN());
 REQUIRE_THROWS(average.prepare(inputs, std::array{1., 0., 0.}));
 REQUIRE_THROWS(average.prepare(inputs, std::array{0., 0., 0.}));
}
TEST_CASE("merge clocks preserve nonzero resumed rounds and retire zero and partial intervals", "[model][rfdetr][training][merge]") {
 r::TrainLaneConfiguration config;
 config.mode = r::TrainLaneMode::PeriodicAveraging;
 config.merge_cadence = r::TrainMergeCadence::Rounds;
 config.merge_rounds = 3;
 r::TrainingMergeSchedule schedule(config, {8, 4, 2});
 CHECK_FALSE(schedule.finish_round(false));
 CHECK(schedule.finish_round(true));
 schedule.retire_interval();
 CHECK(schedule.state().round == 9);
 CHECK(schedule.state().merge == 5);
 CHECK_FALSE(schedule.pending());
 CHECK_FALSE(schedule.finish_round(true));
 schedule.retire_interval();
 CHECK(schedule.state().merge == 6);
 schedule.retire_interval();
 CHECK(schedule.state().merge == 6);
 config.merge_rounds = 1;
 r::TrainingMergeSchedule each(config);
 CHECK(each.finish_round(true));
 each.retire_interval();
 CHECK(each.finish_round(true));
 config.merge_cadence = r::TrainMergeCadence::Epoch;
 r::TrainingMergeSchedule epoch(config);
 CHECK_FALSE(epoch.finish_round(true));
 CHECK(epoch.pending());
 epoch.retire_interval();
 CHECK(epoch.state().merge == 1);
}
TEST_CASE("final selection keeps explicit and uniform outcomes even when validation worsens", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-selection");
 std::array candidates{ingredient(temp.path(), 4, 1, .8), ingredient(temp.path(), 9, 5, .7)};
 candidates[0].coefficient = 0;
 candidates[1].coefficient = 2;
 const auto evaluate = [](const std::filesystem::path& path) {
  const auto state = r::decode_native_model_state(path);
  CHECK(state.entries().front().tensor.item<double>() > 0);
  return evaluation(.1);
 };
 const auto explicit_result = r::select_training_artifact(candidates, r::TrainFinalPolicy::Explicit, false, temp.path(), evaluate);
 CHECK(explicit_result.artifact.weights == r::EvaluatedWeights::Soup);
 CHECK(*explicit_result.artifact.selection_metric == .1);
 CHECK(explicit_result.ingredients.front().coefficient == 0);
 CHECK(explicit_result.ingredients.back().coefficient == 1);
 CHECK(r::decode_native_model_state(explicit_result.artifact.path).entries().front().tensor.item<double>() == 5);
 const auto uniform = r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), evaluate);
 CHECK(r::decode_native_model_state(uniform.artifact.path).entries().front().tensor.item<double>() == 3);
 CHECK(std::filesystem::exists(candidates[0].artifact.path));
 CHECK(std::filesystem::exists(explicit_result.artifact.path));
 CHECK(r::read_training_selection(temp.path() / "selected.json") == uniform);
 candidates[1].coefficient = 0;
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Explicit, false, temp.path(), evaluate));
 CHECK(r::read_training_selection(temp.path() / "selected.json") == uniform);
}
TEST_CASE("greedy uses strict improvement stable ties and identified duplicate ingredients", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-greedy");
 std::array candidates{ingredient(temp.path(), 9, 1, .8), ingredient(temp.path(), 4, 3, .8), ingredient(temp.path(), 12, 5, .7)};
 std::size_t evaluations = 0;
 const auto equal = [&](const auto&) {
  ++evaluations;
  return evaluation(.8);
 };
 const auto retained = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), equal);
 CHECK(retained.artifact.model_id == 4);
 REQUIRE(retained.ingredients.size() == 1);
 CHECK(evaluations == 2);
 candidates[0].artifact.path = candidates[1].artifact.path;
 candidates[0].artifact.sha256 = candidates[1].artifact.sha256;
 candidates[0].artifact.content = candidates[1].artifact.content;
 evaluations = 0;
 const auto improve = [&](const auto&) {
  ++evaluations;
  return evaluation(.9);
 };
 const auto accepted = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), improve);
 REQUIRE(accepted.ingredients.size() == 2);
 CHECK(evaluations == 1);
 CHECK(accepted.ingredients[0].model_id == 4);
 CHECK(accepted.ingredients[1].model_id == 12);
 CHECK(accepted.ingredients[0].coefficient == .5);
 evaluations = 0;
 const auto off = r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), improve);
 CHECK(off.artifact.model_id == 4);
 CHECK(evaluations == 0);
}
TEST_CASE("final ingredients require common initialization complete identity and actual finite mask AP", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-selection-admission");
 std::array candidates{ingredient(temp.path(), 1, 1, .8, true), ingredient(temp.path(), 2, 2, .7, true)};
 const auto evaluate = [](const auto&) { return evaluation(.9, true); };
 const auto saved = r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, true, temp.path(), evaluate);
 candidates[1].artifact.initialization = std::string(64, '4');
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, true, temp.path(), evaluate));
 candidates[1].artifact.initialization = candidates[0].artifact.initialization;
 candidates[1].artifact.evaluation->mask.reset();
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, true, temp.path(), evaluate));
 candidates[1].artifact.evaluation = evaluation(std::numeric_limits<double>::quiet_NaN(), true);
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, true, temp.path(), evaluate));
 CHECK(r::read_training_selection(temp.path() / "selected.json") == saved);
}
TEST_CASE("immutable session generations retain complete previous files through each publication fault", "[model][rfdetr][training][checkpoint]") {
 for (auto step :
  {r::TrainingPublicationStep::Created, r::TrainingPublicationStep::PlanWritten, r::TrainingPublicationStep::ModelWritten, r::TrainingPublicationStep::ModelValidated,
   r::TrainingPublicationStep::Serialized, r::TrainingPublicationStep::Validated, r::TrainingPublicationStep::Synced, r::TrainingPublicationStep::Published, r::TrainingPublicationStep::Retained}) {
  mmltk::testsupport::ScopedTempDir temp("session-publication");
  SessionFixture fixture(temp.path());
  fixture.publish();
  const auto previous = fixture.manifest;
  REQUIRE_THROWS(fixture.publish([&](auto observed) {
   if (observed == step) throw std::runtime_error("injected publication failure");
  }));
  r::TrainingSessionAdmission surviving(fixture.checkpoint.path());
  CHECK(std::filesystem::exists(temp.path() / previous.models.front().path));
  CHECK(surviving.model_count() == 3);
  CHECK(r::inspect_training_checkpoint(fixture.checkpoint.path()).checkpoint().resumable);
  CHECK_FALSE(r::inspect_training_checkpoint(temp.path() / previous.models.front().path).checkpoint().resumable);
 }
}
TEST_CASE("session leases retain old generations and reject missing replaced or mixed sets", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("session-leases");
 SessionFixture fixture(temp.path());
 fixture.publish();
 auto lease = std::make_unique<r::TrainingSessionAdmission>(fixture.checkpoint.path());
 lease->release_decoded_state();
 CHECK_NOTHROW(lease->require_unchanged());
 CHECK(lease->model_count() == 3);
 REQUIRE_THROWS(lease->model(0));
 REQUIRE_THROWS(lease->continuation(0));
 REQUIRE_THROWS(lease->plan());
 const auto old = fixture.manifest;
 fixture.publish();
 fixture.publish();
 CHECK(std::filesystem::exists(temp.path() / old.models.front().path));
 REQUIRE_THROWS(lease->require_unchanged());
 lease.reset();
 fixture.publish();
 CHECK_FALSE(std::filesystem::exists(temp.path() / old.models.front().path));
 auto invalid = fixture.manifest;
 invalid.models.front().path = std::filesystem::path("generations") / invalid.previous_generation / ("model-" + std::to_string(invalid.models.front().model_id) + ".pt");
 REQUIRE_THROWS(r::validate_training_session_manifest(invalid));
 std::filesystem::remove(temp.path() / fixture.manifest.models.front().path);
 REQUIRE_THROWS(r::TrainingSessionAdmission(fixture.checkpoint.path()));
}
TEST_CASE("final selection rejects incomplete coefficients and failures preserve the prior frozen output", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-selection-failures");
 std::array candidates{ingredient(temp.path(), 2, 1, .8), ingredient(temp.path(), 1, 2, .8)};
 candidates[0].artifact.epoch = 2;
 candidates[1].artifact.epoch = 3;
 const auto evaluate = [](const auto&) { return evaluation(.9); };
 const auto old = r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), evaluate);
 CHECK(old.artifact.model_id == 2);
 const auto reject = [&](double coefficient) {
  candidates[1].coefficient = coefficient;
  REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Explicit, false, temp.path(), evaluate));
  CHECK(r::read_training_selection(temp.path() / "selected.json") == old);
 };
 reject(-1);
 reject(std::numeric_limits<double>::quiet_NaN());
 reject(std::numeric_limits<double>::infinity());
 candidates[1].coefficient = 1;
 REQUIRE_THROWS(
  r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) -> r::EvalSummary { throw std::runtime_error("selection evaluation failed"); }));
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(std::numeric_limits<double>::quiet_NaN()); }));
 CHECK(r::read_training_selection(temp.path() / "selected.json") == old);
 candidates[1].artifact.weights = r::EvaluatedWeights::Ema;
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), evaluate));
 candidates[1].artifact.weights = r::EvaluatedWeights::Ordinary;
 candidates[1].artifact.model_id = candidates[0].artifact.model_id;
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), evaluate));
 CHECK(std::filesystem::exists(candidates[0].artifact.path));
 CHECK(std::filesystem::exists(candidates[1].artifact.path));
}
TEST_CASE("identical native content is one greedy ingredient even through separate saved files", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-content-identity");
 std::array candidates{ingredient(temp.path(), 1, 3, .5), ingredient(temp.path(), 2, 3, .5)};
 CHECK(candidates[0].artifact.content == candidates[1].artifact.content);
 std::size_t calls = 0;
 const auto selected = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), [&](const auto&) {
  ++calls;
  return evaluation(.9);
 });
 CHECK(calls == 0);
 CHECK(selected.ingredients.size() == 1);
 CHECK(selected.artifact.model_id == 1);
 for (auto& candidate : candidates) {
  auto proof = std::make_shared<r::TrainingArtifactAdmission>(candidate.artifact);
  proof->release_decoded_state();
  candidate.admission = std::move(proof);
 }
 const auto retained = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), [&](const auto&) {
  ++calls;
  return evaluation(.9);
 });
 CHECK(calls == 0);
 CHECK(retained == selected);
}
TEST_CASE("session admission rejects a partial or mixed model set before replacing its pointer", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("session-mixed-set");
 SessionFixture fixture(temp.path());
 fixture.publish();
 const auto previous = fixture.manifest;
 for (std::size_t model = 0; model < previous.models.size(); ++model) {
  fixture.corrupt_model = model;
  REQUIRE_THROWS(fixture.publish());
  r::TrainingSessionAdmission current(fixture.checkpoint.path());
  CHECK(current.manifest() == previous);
  for (const auto& file : previous.models) CHECK(io::sha256_hex(io::sha256_file(temp.path() / file.path)) == file.sha256);
 }
 fixture.corrupt_model.reset();
 for (const auto step : {r::TrainingPublicationStep::ModelWritten, r::TrainingPublicationStep::ModelValidated}) {
  for (std::size_t model = 0; model < previous.models.size(); ++model) {
   std::size_t visited = 0;
   REQUIRE_THROWS(fixture.publish([&](auto observed) {
    if (observed == step && visited++ == model) throw std::runtime_error("partial model set");
   }));
   CHECK(r::TrainingSessionAdmission(fixture.checkpoint.path()).manifest() == previous);
  }
 }
 std::vector<std::unique_ptr<r::TrainingSessionAdmission>> readers;
 for (int count = 0; count < 16; ++count) readers.push_back(std::make_unique<r::TrainingSessionAdmission>(fixture.checkpoint.path()));
 REQUIRE_THROWS(r::TrainingSessionAdmission(fixture.checkpoint.path()));
 readers.erase(readers.begin());
 CHECK_NOTHROW(r::TrainingSessionAdmission(fixture.checkpoint.path()));
}
TEST_CASE("native artifact metadata remains authoritative under misleading legacy directory names", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("output-seg-med");
 const auto candidate = ingredient(temp.path(), 1, 2, .5);
 CHECK(r::resolve_model_state(candidate.artifact.path, {}, 0).artifacts.config.preset_name == "rf-detr-nano");
 auto unnamed = r::decode_native_model_state(candidate.artifact.path);
 unnamed.metadata.preset_name.clear();
 const auto path = temp.path() / "checkpoint.pt";
 r::save_native_checkpoint(path, unnamed);
 CHECK_THROWS(r::resolve_model_state(path, {}, 0));
}
TEST_CASE("native averaging checks mixed storage and invalid inputs before receiver mutation", "[model][rfdetr][training][merge]") {
 std::vector<r::NormalizedModelStateEntry> a{{"half", torch::ones({2}, torch::kFloat16)}, {"double", torch::ones({2}, torch::kFloat64)}, {"integer", torch::ones({2}, torch::kInt64)}};
 auto b = a;
 for (auto& entry : b) entry.tensor = entry.tensor.clone();
 const std::array inputs{&a, &b};
 r::NativeModelAverage average;
 average.prepare(inputs, std::array{1., 1.});
 CHECK(average.values()[0].tensor.scalar_type() == torch::kFloat32);
 CHECK(average.values()[1].tensor.scalar_type() == torch::kFloat64);
 average.install(inputs);
 b[1].tensor.fill_(std::numeric_limits<double>::infinity());
 REQUIRE_THROWS(average.prepare(inputs, std::array{1., 0.}));
 REQUIRE_THROWS(average.prepare(inputs, std::array{1., 1.}));
 REQUIRE_THROWS(average.install(inputs));
 CHECK(torch::equal(a[1].tensor, torch::ones({2}, torch::kFloat64)));
 b[1].tensor.fill_(1);
 b[2].tensor.fill_(2);
 REQUIRE_THROWS(average.prepare(inputs, std::array{1., 1.}));
 CHECK(torch::equal(a[2].tensor, torch::ones({2}, torch::kInt64)));
}
TEST_CASE("selection retires unpublished trials and preserves every committed artifact", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-trial-custody");
 const std::array candidates{ingredient(temp.path(), 1, 1, .8), ingredient(temp.path(), 2, 2, .7), ingredient(temp.path(), 3, 3, .6)};
 const auto inventory = [&] {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(temp.path())) {
   const auto name = entry.path().filename().string();
   // Bundle lock inodes outlive retired artifacts so concurrent admissions
   // and publications continue to synchronize on the same lock.
   if (name.starts_with("selection-") && !name.ends_with(".classes.lock")) paths.push_back(entry.path());
  }
  return paths;
 };
 for (int repeat = 0; repeat < 3; ++repeat) {
  const auto selected = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), [](const auto&) { return evaluation(.1); });
  CHECK(inventory().empty());
  REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) -> r::EvalSummary { throw std::runtime_error("evaluation failure"); }));
  REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(std::numeric_limits<double>::quiet_NaN()); }));
  CHECK(inventory().empty());
  CHECK(r::read_training_selection(temp.path() / "selected.json") == selected);
 }
 double metric = .8;
 const auto first = r::select_training_artifact(candidates, r::TrainFinalPolicy::ValidationGreedy, false, temp.path(), [&](const auto&) {
  metric += .05;
  return evaluation(metric);
 });
 REQUIRE(inventory().size() == 1);
 CHECK(std::filesystem::exists(first.artifact.path));
 const auto second = r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(.5); });
 CHECK(inventory().size() == 2);
 CHECK(std::filesystem::exists(first.artifact.path));
 CHECK(std::filesystem::exists(second.artifact.path));
 std::filesystem::create_directory(temp.path() / "selected.json.staging");
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(.7); }));
 CHECK(inventory().size() == 2);
 CHECK(r::read_training_selection(temp.path() / "selected.json") == second);
 for (const auto& candidate : candidates) CHECK(std::filesystem::exists(candidate.artifact.path));
}
TEST_CASE("periodic duplicate admissions retain all logical ingredient identities", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("native-duplicate-admission");
 const auto first = ingredient(temp.path(), 1, 3, .5);
 std::vector<r::TrainingSelectionCandidate> candidates(r::kMaximumTrainingModels, first);
 for (std::size_t index = 0; index < candidates.size(); ++index) candidates[index].artifact.model_id = index + 1;
 const auto selected = r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(.5); });
 REQUIRE(selected.ingredients.size() == r::kMaximumTrainingModels);
 for (std::size_t index = 0; index < candidates.size(); ++index) CHECK(selected.ingredients[index].model_id == index + 1);
 candidates.back().artifact.sha256 = std::string(64, '0');
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), [](const auto&) { return evaluation(.5); }));
 candidates.back() = first;
 candidates.back().artifact.model_id = r::kMaximumTrainingModels;
 candidates.back().artifact.path = temp.path() / "distinct.pt";
 std::filesystem::copy_file(first.artifact.path, candidates.back().artifact.path);
 std::ofstream(candidates.back().artifact.path, std::ios::binary | std::ios::app) << "changed";
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), [](const auto&) { return evaluation(.5); }));
}
TEST_CASE("artifact evidence survives decoded readers and rejects identical named-file replacement", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("artifact-retained-evidence");
 const auto candidate = ingredient(temp.path(), 1, 3, .5);
 r::TrainingArtifactAdmission admission(candidate.artifact);
 auto reader = admission.decode();
 const std::weak_ptr<const r::DecodedNativeModelState> decoded = reader;
 CHECK(admission.evidence()->file()->sha256 == reader->class_artifact->file()->sha256);
 admission.release_decoded_state();
 CHECK_FALSE(decoded.expired());
 CHECK(reader->entries().front().tensor.item<double>() == 3);
 reader.reset();
 CHECK(decoded.expired());
 CHECK_NOTHROW(admission.require_matches(candidate.artifact));
 r::TrainingSelection selected;
 selected.artifact = candidate.artifact;
 selected.validation = *candidate.artifact.evaluation;
 selected.best_individual_metric = *candidate.artifact.selection_metric;
 selected.ingredients.push_back({candidate.artifact.model_id, candidate.artifact.sha256, 1});
 r::publish_training_selection(temp.path() / "selected.json", selected, admission);
 CHECK(decoded.expired());
 CHECK(r::read_training_selection(temp.path() / "selected.json") == selected);
 const auto replacement = temp.path() / "replacement.pt";
 std::filesystem::copy_file(candidate.artifact.path, replacement);
 std::filesystem::rename(replacement, candidate.artifact.path);
 REQUIRE_THROWS(admission.require_unchanged());
 REQUIRE_THROWS(admission.decode());
 REQUIRE_THROWS(r::publish_training_selection(temp.path() / "selected.json", selected, admission));
 CHECK(r::read_training_selection(temp.path() / "selected.json") == selected);
}
TEST_CASE("session publication retains only its unchanged current candidate evidence", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("session-candidate-evidence");
 SessionFixture fixture(temp.path());
 std::vector<std::weak_ptr<const r::TrainingArtifactAdmission>> proofs;
 for (auto& model : fixture.manifest.models) {
  auto candidate = ingredient(temp.path(), model.model_id, static_cast<double>(model.model_id), .5);
  candidate.artifact.configuration = fixture.manifest.configuration;
  candidate.artifact.validation = fixture.manifest.validation;
  model.best = candidate.artifact;
  auto admission = std::make_shared<r::TrainingArtifactAdmission>(candidate.artifact);
  admission->release_decoded_state();
  proofs.push_back(admission);
  fixture.admissions.push_back(std::move(admission));
 }
 fixture.publish();
 fixture.admissions.clear();
 fixture.publish();
 for (const auto& proof : proofs) CHECK_FALSE(proof.expired());
 // A replaced logical best retires the prior proof, while the individual file survives.
 const auto previous = fixture.manifest.models.front().best->path;
 auto replacement = ingredient(temp.path(), 100, 8, .7);
 replacement.artifact.model_id = fixture.manifest.models.front().model_id;
 replacement.artifact.configuration = fixture.manifest.configuration;
 replacement.artifact.validation = fixture.manifest.validation;
 fixture.manifest.models.front().best = replacement.artifact;
 fixture.publish();
 CHECK(proofs.front().expired());
 CHECK_FALSE(proofs.back().expired());
 CHECK(std::filesystem::exists(previous));
 r::TrainingSessionAdmission inspected(fixture.checkpoint.path());
 const auto best = inspected.best_admission(0);
 inspected.release_decoded_state();
 CHECK_NOTHROW(best->require_matches(replacement.artifact));
 CHECK_NOTHROW(inspected.require_unchanged());
}
TEST_CASE("session shared candidate paths preserve logical IDs and reject conflicting facts", "[model][rfdetr][training][checkpoint]") {
 mmltk::testsupport::ScopedTempDir temp("session-shared-candidate");
 SessionFixture fixture(temp.path());
 auto shared = ingredient(temp.path(), 1, 3, .5).artifact;
 shared.configuration = fixture.manifest.configuration;
 shared.validation = fixture.manifest.validation;
 for (auto& model : fixture.manifest.models) {
  model.best = shared;
  model.best->model_id = model.model_id;
  model.best->attempt = model.model_id;
 }
 fixture.publish();
 const auto previous = fixture.manifest;
 {
  r::TrainingSessionAdmission admitted(fixture.checkpoint.path());
  for (std::size_t index = 0; index < admitted.model_count(); ++index) {
   CHECK(admitted.manifest().models[index].best->model_id == fixture.manifest.models[index].model_id);
   CHECK(admitted.best_admission(index) == admitted.best_admission(0));
  }
 }
 ++fixture.manifest.models.back().best->epoch;
 REQUIRE_THROWS(fixture.publish());
 CHECK(r::TrainingSessionAdmission(fixture.checkpoint.path()).manifest() == previous);
}
TEST_CASE("each named equal-content artifact and late companion is checked before selection publication", "[model][rfdetr][training][merge]") {
 mmltk::testsupport::ScopedTempDir temp("selection-named-evidence");
 std::array candidates{ingredient(temp.path(), 1, 3, .5), ingredient(temp.path(), 2, 3, .5)};
 CHECK(candidates[0].artifact.content == candidates[1].artifact.content);
 const auto selected = r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [](const auto&) { return evaluation(.5); });
 REQUIRE(selected.ingredients.size() == 2);
 CHECK(selected.ingredients[0].model_id == 1);
 CHECK(selected.ingredients[1].model_id == 2);
 for (const auto& candidate : candidates) {
  const auto companion = std::filesystem::path(candidate.artifact.path.string() + ".classes.json");
  REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Uniform, false, temp.path(), [&](const auto&) {
   std::ofstream(companion) << "late companion";
   return evaluation(.6);
  }));
  std::filesystem::remove(companion);
  CHECK(r::read_training_selection(temp.path() / "selected.json") == selected);
 }
 // Equal tensor content never authorizes sharing the other path's file proof.
 candidates[1].admission = std::make_shared<r::TrainingArtifactAdmission>(candidates[0].artifact);
 REQUIRE_THROWS(r::select_training_artifact(candidates, r::TrainFinalPolicy::Off, false, temp.path(), [](const auto&) { return evaluation(.5); }));
}
TEST_CASE("last session publication checks include retained candidates and all generation files", "[model][rfdetr][training][checkpoint]") {
 for (const auto target : {0, 1, 2}) {
  mmltk::testsupport::ScopedTempDir temp("session-late-evidence");
  SessionFixture fixture(temp.path());
  auto candidate = ingredient(temp.path(), 1, 3, .5).artifact;
  candidate.configuration = fixture.manifest.configuration;
  candidate.validation = fixture.manifest.validation;
  fixture.manifest.models.front().best = candidate;
  fixture.publish();
  const auto previous = fixture.manifest;
  std::filesystem::path changed;
  const auto change_before_publication = [&](auto step) {
   if (step != r::TrainingPublicationStep::Synced) return;
   if (target == 0)
    changed = candidate.path.string() + ".classes.json";
   else {
    for (const auto& entry : std::filesystem::directory_iterator(temp.path() / "generations")) {
     if (entry.path().filename() == previous.generation || entry.path().filename() == previous.previous_generation) continue;
     changed = entry.path() / (target == 1 ? "model-1.pt.classes.json" : "plan.cbor");
    }
   }
   REQUIRE_FALSE(changed.empty());
   std::ofstream(changed, std::ios::binary | std::ios::app) << "late change";
  };
  REQUIRE_THROWS(fixture.publish(change_before_publication));
  if (target == 0) std::filesystem::remove(changed);
  CHECK(r::TrainingSessionAdmission(fixture.checkpoint.path()).manifest() == previous);
  for (const auto& model : previous.models) CHECK(std::filesystem::exists(temp.path() / model.path));
 }
}
