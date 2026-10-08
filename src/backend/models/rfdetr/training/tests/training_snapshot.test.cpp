#include "src/backend/ml/torch/tests/catch_support.h"
#include "detail/training_snapshot.h"
#include "detail/training_artifact.h"
#include "detail/training_session_checkpoint.h"
#include "detail/checkpoint_private.h"
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/checkpoint_fixture_support.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/common/system/execution_policy.h"
#include "src/backend/ml/torch/archive.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include "training_continuation_fixture.h"
#include "model_state_fixture.h"
#include <cuda_runtime_api.h>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mmltk::backend::models::rfdetr::testsupport {
struct TrainingSnapshotTestAccess final {
 static torch::Tensor view(TrainingSnapshot& snapshot, std::size_t slot) { return snapshot.readback_.Stage(slot); }
 static std::size_t capacity(const TrainingSnapshot& snapshot) { return snapshot.readback_.capacity_bytes(); }
};
}  // namespace mmltk::backend::models::rfdetr::testsupport
namespace {
namespace r = mmltk::backend::models::rfdetr;
TEST_CASE("Production transfer requires complete core while explicit partial and exact admission keep their contracts", "[rfdetr][checkpoint_parity][training_supervision]") {
 using Admission = r::detail::NormalizedModelStateAdmission;
 auto config = r::native_config_from_preset(r::model_presets().front());
 config.num_queries = config.num_select = 3;
 config.num_classes = 3;
 const auto source_layout = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"a", "b"}));
 const auto destination_layout = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"b", "new"}));
 r::NativeRfDetrModel source(config, source_layout), destination(config, destination_layout);
 auto complete = r::testsupport::clone_normalized_model_state(source);
 r::DecodedNativeModelState checkpoint(complete);
 checkpoint.metadata.class_layout = source_layout;
 const auto before = r::testsupport::clone_normalized_model_state(destination);
 const auto require_values = [](const r::NativeRfDetrModel& model, const std::vector<r::NormalizedModelStateEntry>& expected) {
  const auto actual = r::collect_module_state(model);
  REQUIRE(actual.size() == expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
   CAPTURE(expected[index].name);
   REQUIRE(actual[index].name == expected[index].name);
   CHECK(torch::equal(actual[index].tensor, expected[index].tensor));
  }
 };
 const r::ResolvedClassLayout semantic_source(source_layout);
 for (const auto malformed : {0, 1, 2, 3, 4, 5}) {
  CAPTURE(malformed);
  auto state = complete;
  if (malformed == 0) state.clear();
  if (malformed == 1) state.erase(std::ranges::find(state, std::string_view{"query_feat.weight"}, &r::NormalizedModelStateEntry::name));
  if (malformed == 2) std::ranges::find(state, std::string_view{"query_feat.weight"}, &r::NormalizedModelStateEntry::name)->tensor = torch::zeros({1, config.hidden_dim});
  if (malformed == 3) state.push_back({"_kp_active_mask", torch::zeros({1, 1}, torch::kBool)});
  if (malformed == 4) state.push_back(state.front());
  if (malformed == 5) std::ranges::find(state, std::string_view{"class_embed.weight"}, &r::NormalizedModelStateEntry::name)->tensor = torch::zeros({1, config.hidden_dim});
  checkpoint.replace_entries(std::move(state));
  REQUIRE_THROWS(r::load_training_model_weights(destination, checkpoint, r::TrainingSupervisionRoute::Hungarian));
  REQUIRE_THROWS(destination.stage_normalized_state(checkpoint.entries(), Admission::RequiredCore));
  if (malformed == 2 || malformed == 5) {
   try {
    static_cast<void>(destination.stage_normalized_state(checkpoint.entries(), Admission::RequiredCore));
    FAIL("damaged core tensor must fail admission");
   } catch (const r::ModelStateAdmissionError& error) { REQUIRE(error.summary().incompatible_names == std::vector<std::string>{malformed == 2 ? "query_feat.weight" : "class_embed.weight"}); }
  }
  require_values(destination, before);
 }
 checkpoint.replace_entries(complete);
 const auto summary = r::load_training_model_weights(destination, checkpoint, r::TrainingSupervisionRoute::Hungarian);
 REQUIRE(summary.missing_names.empty());
 REQUIRE(summary.incompatible_names.empty());
 REQUIRE(summary.initialized_names.empty());
 REQUIRE(summary.loaded_names.size() == complete.size());
 const auto parameters = destination.named_parameters();
 for (const auto& entry : complete) {
  const auto* actual = parameters.find(entry.name);
  if (!actual) continue;
  if (entry.name == "class_embed.weight" || entry.name == "class_embed.bias" || entry.name.starts_with("transformer.enc_out_class_embed.")) {
   const auto original = std::ranges::find(before, entry.name, &r::NormalizedModelStateEntry::name);
   CHECK(torch::equal(actual->select(0, 0), entry.tensor.select(0, 1)));
   CHECK(torch::equal(actual->select(0, 1), original->tensor.select(0, 1)));
   CHECK(torch::equal(actual->select(0, 2), original->tensor.select(0, 2)));
  } else
   CHECK(torch::equal(*actual, entry.tensor));
 }
 const std::vector partial{r::NormalizedModelStateEntry{"query_feat.weight", torch::full_like(parameters["query_feat.weight"], .75)}};
 for (const auto admission : {Admission::PartialExact, Admission::PartialFreshTransfer}) {
  auto candidate = destination.stage_normalized_state(partial, admission, &semantic_source);
  REQUIRE(candidate.summary.loaded_names == std::vector<std::string>{"query_feat.weight"});
  REQUIRE_FALSE(candidate.summary.missing_names.empty());
  destination.commit_normalized_state(std::move(candidate));
  CHECK(torch::equal(destination.named_parameters()["query_feat.weight"], partial.front().tensor));
 }
 auto external = complete;
 external.push_back({"_kp_active_mask", torch::zeros({0, 0}, torch::kBool)});
 external.push_back({"training_supervision.retained_training_state", torch::ones({1})});
 auto core = destination.stage_normalized_state(external, Admission::RequiredCore);
 REQUIRE(core.summary.unexpected_names.empty());
 REQUIRE(core.summary.missing_names.empty());
 destination.commit_normalized_state(std::move(core));
 REQUIRE_THROWS(destination.stage_normalized_state(external, Admission::Exact));
 config.training_supervision.assignment = r::TrainAssignmentKind::MatchFree;
 r::NativeRfDetrModel active(config, destination_layout);
 active.initialize_training_supervision(47);
 // Stock pretraining legitimately lacks the extension; Resume must restore it.
 REQUIRE_NOTHROW(active.stage_normalized_state(complete, Admission::FreshTransfer, &semantic_source));
 REQUIRE_THROWS(active.stage_normalized_state(complete, Admission::Exact));
 const auto full_resume = r::testsupport::clone_normalized_model_state(active);
 REQUIRE_NOTHROW(active.commit_normalized_state(active.stage_normalized_state(full_resume, Admission::Exact)));
 checkpoint.metadata.preset_name = config.preset_name;
 checkpoint.metadata.num_queries = source.config().num_queries;
 checkpoint.metadata.num_select = source.config().num_select;
 const r::FreshTransferQueryRetention retention{{source.config().group_detr, source.config().num_queries}, true};
 const std::vector<std::string> query_names{"query_feat.weight", "refpoint_embed.weight"};
 for (const int query_count : {2, 5}) {
  CAPTURE(query_count);
  auto resized_config = source.config();
  resized_config.num_queries = resized_config.num_select = query_count;
  if (query_count == 5) resized_config.preset_name = "custom-query-transfer-test";
  r::NativeRfDetrModel resized(resized_config, destination_layout);
  const auto fresh = r::testsupport::clone_normalized_model_state(resized);
  const auto retained = resized.named_parameters();
  const std::array query_storage{retained[query_names[0]].const_data_ptr(), retained[query_names[1]].const_data_ptr()};
  const std::array query_versions{retained[query_names[0]]._version(), retained[query_names[1]]._version()};
  const auto require_queries = [&] {
   const auto values = resized.named_parameters();
   for (std::size_t index = 0; index < query_names.size(); ++index) {
    const auto& name = query_names[index];
    const auto expected = std::ranges::find(fresh, name, &r::NormalizedModelStateEntry::name);
    CHECK(values[name].const_data_ptr() == query_storage[index]);
    CHECK(values[name]._version() == query_versions[index]);
    CHECK(torch::equal(values[name], expected->tensor));
   }
  };
  const auto require_transfer_refused = [&] {
   REQUIRE_THROWS(resized.stage_normalized_state(checkpoint.entries(), Admission::FreshTransfer, &semantic_source, retention));
   REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian));
   require_values(resized, fresh);
  };
  for (const auto& name : query_names) {
   for (const std::string_view defect : {"missing", "duplicate", "undefined", "rank", "rows", "width"}) {
    CAPTURE(name, defect);
    auto malformed = complete;
    const auto found = std::ranges::find(malformed, name, &r::NormalizedModelStateEntry::name);
    REQUIRE(found != malformed.end());
    if (defect == "missing")
     malformed.erase(found);
    else if (defect == "duplicate")
     malformed.push_back(*found);
    else if (defect == "undefined")
     found->tensor = torch::Tensor{};
    else if (defect == "rank")
     found->tensor = found->tensor.unsqueeze(0);
    else if (defect == "rows")
     found->tensor = found->tensor.narrow(0, 0, 1);
    else
     found->tensor = found->tensor.narrow(1, 0, 1);
    checkpoint.replace_entries(std::move(malformed));
    require_transfer_refused();
    require_queries();
   }
  }
  for (const bool missing : {false, true}) {
   auto malformed = complete;
   const std::string name = "backbone.0.encoder.encoder.embeddings.cls_token";
   const auto found = std::ranges::find(malformed, name, &r::NormalizedModelStateEntry::name);
   REQUIRE(found != malformed.end());
   if (missing)
    malformed.erase(found);
   else
    found->tensor = torch::Tensor{};
   checkpoint.replace_entries(std::move(malformed));
   try {
    static_cast<void>(resized.stage_normalized_state(checkpoint.entries(), Admission::FreshTransfer, &semantic_source, retention));
    FAIL("query retention must still require every compatible core tensor");
   } catch (const r::ModelStateAdmissionError& error) {
    CHECK(error.summary().initialized_names == query_names);
    CHECK((missing ? error.summary().missing_names : error.summary().incompatible_names) == std::vector<std::string>{name});
    CHECK(error.summary().loaded_names.size() + query_names.size() + 1 == complete.size());
   }
   REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian));
   require_values(resized, fresh);
  }
  checkpoint.replace_entries(complete);
  for (const auto& name : query_names) {
   auto weight = retained[name];
   const auto original = weight.detach();
   for (const auto& malformed : {original.unsqueeze(0), original.narrow(0, 0, 1), original.narrow(1, 0, 1)}) {
    weight.set_data(malformed);
    REQUIRE_THROWS(resized.stage_normalized_state(complete, Admission::FreshTransfer, &semantic_source, retention));
    REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian));
    weight.set_data(original);
    require_values(resized, fresh);
   }
  }
  checkpoint.replace_entries(fresh);
  require_transfer_refused();
  checkpoint.replace_entries(complete);
  for (const auto admission : {Admission::Exact, Admission::RequiredCore, Admission::PartialExact, Admission::PartialFreshTransfer})
   REQUIRE_THROWS(resized.stage_normalized_state(complete, admission, &semantic_source, retention));
  REQUIRE_THROWS(resized.stage_normalized_state(complete, Admission::FreshTransfer, &semantic_source));
  for (const std::int64_t source_count :
   {std::int64_t{0}, std::int64_t{-1}, std::int64_t{query_count}, retention.source_layout.queries_per_group + 1, std::numeric_limits<std::int64_t>::max() / retention.source_layout.groups + 1,
    std::numeric_limits<std::int64_t>::max()}) {
   auto conflicting = retention;
   conflicting.source_layout.queries_per_group = source_count;
   REQUIRE_THROWS(resized.stage_normalized_state(complete, Admission::FreshTransfer, &semantic_source, conflicting));
   require_values(resized, fresh);
  }
  for (const std::int64_t source_groups : {std::int64_t{0}, std::int64_t{-1}, retention.source_layout.groups + 1}) {
   auto conflicting = retention;
   conflicting.source_layout.groups = source_groups;
   REQUIRE_THROWS(resized.stage_normalized_state(complete, Admission::FreshTransfer, &semantic_source, conflicting));
   require_values(resized, fresh);
  }
  ++checkpoint.metadata.num_queries;
  REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian));
  require_values(resized, fresh);
  --checkpoint.metadata.num_queries;
  for (const auto route : {r::TrainingSupervisionRoute::MatchFree, r::TrainingSupervisionRoute::HungarianDenoising, r::TrainingSupervisionRoute::MatchFreeDenoising}) {
   REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, route));
   require_values(resized, fresh);
  }
  checkpoint.metadata.preset_name.clear();
  if (query_count == 5) {
   REQUIRE_THROWS(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian));
   require_values(resized, fresh);
   checkpoint.metadata.preset_name = source.config().preset_name;
  }
  auto reordered = complete;
  std::ranges::reverse(reordered);
  auto staged = resized.stage_normalized_state(reordered, Admission::FreshTransfer, &semantic_source, retention);
  REQUIRE(staged.summary.initialized_names == query_names);
  REQUIRE(staged.summary.loaded_names.size() + query_names.size() == complete.size());
  REQUIRE(staged.destinations.size() == staged.summary.loaded_names.size());
  REQUIRE(staged.tensors.size() == staged.destinations.size());
  REQUIRE(staged.summary.missing_names.empty());
  REQUIRE(staged.summary.incompatible_names.empty());
  REQUIRE(staged.summary.unexpected_names.empty());
  REQUIRE(std::ranges::is_sorted(staged.summary.loaded_names));
  for (const auto& value : staged.destinations)
   for (const auto* address : query_storage) CHECK(value.const_data_ptr() != address);
  for (const auto& name : query_names) CHECK(std::ranges::find(staged.summary.loaded_names, name) == staged.summary.loaded_names.end());
  require_values(resized, fresh);
  const auto staged_loaded = staged.summary.loaded_names;
  resized.commit_normalized_state(std::move(staged));
  require_queries();
  const auto transferred = r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian);
  REQUIRE(transferred.initialized_names == query_names);
  REQUIRE(transferred.loaded_names == staged_loaded);
  REQUIRE(transferred.missing_names.empty());
  REQUIRE(transferred.incompatible_names.empty());
  require_queries();
  for (std::size_t index = 0; index < complete.size(); ++index) {
   REQUIRE(checkpoint.entries()[index].name == complete[index].name);
   CHECK(checkpoint.entries()[index].tensor.const_data_ptr() == complete[index].tensor.const_data_ptr());
  }
  require_values(source, complete);
  // Raw dictionaries have no count metadata. The same preset default used by
  // artifact resolution is sufficient only when the complete query pair agrees.
  auto raw = complete;
  const auto* preset = r::find_preset_catalog_entry(source.config().preset_name);
  REQUIRE(preset);
  for (const auto& name : query_names) {
   auto entry = std::ranges::find(raw, name, &r::NormalizedModelStateEntry::name);
   entry->tensor = torch::zeros({static_cast<int64_t>(preset->query_count) * preset->group_count, entry->tensor.size(1)});
  }
  checkpoint.replace_entries(std::move(raw));
  checkpoint.metadata.num_queries = 0;
  const r::FreshTransferQueryRetention fallback{{preset->group_count, preset->query_count}, false};
  REQUIRE(resized.stage_normalized_state(checkpoint.entries(), Admission::FreshTransfer, &semantic_source, fallback).summary.initialized_names == query_names);
  REQUIRE(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian).initialized_names == transferred.initialized_names);
  require_queries();
  for (const auto& name : query_names) {
   const auto original = std::ranges::find(checkpoint.entries(), name, &r::NormalizedModelStateEntry::name);
   CHECK(original->tensor.size(0) == static_cast<int64_t>(preset->query_count) * preset->group_count);
   CHECK(torch::equal(original->tensor, torch::zeros_like(original->tensor)));
  }
  // Matching raw rows load normally, even when the preset fallback differs.
  checkpoint.replace_entries(fresh);
  auto matching = resized.stage_normalized_state(checkpoint.entries(), Admission::FreshTransfer, &semantic_source, fallback);
  REQUIRE(matching.summary.initialized_names.empty());
  REQUIRE(matching.summary.loaded_names.size() == fresh.size());
  REQUIRE(matching.destinations.size() == fresh.size());
  // An unknown preset need not provide group evidence when no retention occurs.
  checkpoint.metadata.preset_name.clear();
  REQUIRE(r::load_training_model_weights(resized, checkpoint, r::TrainingSupervisionRoute::Hungarian).initialized_names.empty());
  checkpoint.metadata.preset_name = source.config().preset_name;
  checkpoint.metadata.num_queries = source.config().num_queries;
 }
}
TEST_CASE("Training dataset identity uses bounded metadata without opening image payloads", "[rfdetr][training][session]") {
 mmltk::backend::data::FileHeader header{};
 header.num_images = 1'000'000;
 header.image_width = header.image_height = 312;
 header.channels = 3;
 header.total_file_size = 1ULL << 40;
 mmltk::common::io::FileSnapshot generation{.device = 1, .inode = 2, .bytes = header.total_file_size, .modified_seconds = 100, .modified_nanoseconds = 200};
 // No file exists: even a terabyte source identity uses these metadata alone.
 const auto identity = r::training_dataset_identity(header, generation, "resolved-model");
 CHECK(identity.size() == 64);
 CHECK(r::training_dataset_identity(header, generation, "resolved-model") == identity);
 auto moved = generation;
 moved.device = 3;
 moved.inode = 4;
 moved.changed_seconds = 5;
 CHECK(r::training_dataset_identity(header, moved, "resolved-model") == identity);
 ++generation.modified_nanoseconds;
 CHECK(r::training_dataset_identity(header, generation, "resolved-model") != identity);
 --generation.modified_nanoseconds;
 CHECK(r::training_dataset_identity(header, generation, "different-model") != identity);
 ++header.data_offset;
 CHECK(r::training_dataset_identity(header, generation, "resolved-model") != identity);
}
using Access = r::testsupport::TrainingSnapshotTestAccess;
struct CopyAccounting final {
 std::map<const void*, std::size_t> reads;
 std::size_t bytes = 0;
 [[nodiscard]] std::size_t count() const {
  std::size_t count = 0;
  for (const auto& [source, copies] : reads) count += copies;
  return count;
 }
};
thread_local CopyAccounting* active_copies = nullptr;
class CountCopies final {
public:
 CountCopies() { active_copies = &copies; }
 ~CountCopies() { active_copies = nullptr; }
 CopyAccounting copies;
};
[[nodiscard]] std::unique_ptr<mmltk::common::system::ScopedExecutionPolicy> snapshot_policy(bool cuda) {
 if (!cuda) return {};
 int count = 0;
 if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) SKIP("CUDA unavailable");
 CUDA_ASSERT_OK(cudaSetDevice(0));
 return std::make_unique<mmltk::common::system::ScopedExecutionPolicy>(mmltk::frameworks::gpu::test_support::selected_test_execution_policy(0));
}
r::TrainRequest snapshot_request(const std::filesystem::path& directory) {
 r::TrainRequest request;
 request.train_compiled_path = "train.bin";
 request.val_compiled_path = "val.bin";
 request.weights_path = "seed.pt";
 request.output_dir = directory;
 request.epochs = 3;
 request.use_ema = true;
 request.ema_decay = .5;
 request.ema_tau = 0;
 request.recipe.optimizer = r::TrainOptimizerKind::AdamW;
 request.recipe.warmup_epochs = 0;
 return request;
}
torch::OrderedDict<std::string, torch::Tensor> snapshot_parameters(const torch::Device& device) {
 torch::OrderedDict<std::string, torch::Tensor> values;
 values.insert("value", torch::tensor({2.F, 4.F}).to(device).set_requires_grad(true));
 values.insert("training_supervision.value", torch::tensor({7.F}).to(device));
 return values;
}
struct SnapshotFixture final {
 SnapshotFixture(const std::filesystem::path& directory, bool cuda)
     : request(snapshot_request(directory)),
       parameters(snapshot_parameters(torch::Device(cuda ? torch::kCUDA : torch::kCPU))),
       built(r::build_optimizer(parameters, request)),
       ema(built.optimizer.eligible_parameters(), request.ema_decay, request.ema_tau),
       ordinary{
        {"value", parameters["value"]}, {"training_supervision.value", parameters["training_supervision.value"]}, {"counter", torch::tensor({3}, torch::kInt64).to(parameters["value"].device())}
       },
       values(r::testsupport::continuation_values(request, {.epoch = 0})) {
  parameters["value"].mutable_grad() = torch::full_like(parameters["value"], .25);
  built.optimizer.step();
  built.optimizer.zero_grad(true);
  ema.update();
  built.optimizer.set_lrs(values.schedule.absolute_lrs, 1);
  built.optimizer.set_momentum(values.schedule.held_momentum);
  scaler.load_state(128, 0);
  torch::NoGradGuard guard;
  parameters["value"].add_(2);
 }
 [[nodiscard]] r::TrainingSnapshotPublication begin() { return snapshot.begin(ordinary, built.optimizer.eligible_parameter_names(), &ema); }
 void resume(const std::filesystem::path& path) {
  snapshot.save_resume(path, r::testsupport::synthetic_training_metadata(), built.optimizer, scaler, request, 0, ema.completed_updates(), "attempt", {}, values);
 }
 r::TrainRequest request;
 torch::OrderedDict<std::string, torch::Tensor> parameters;
 r::OptimizerBuildResult built;
 r::ModelEma ema;
 r::GradScaler scaler{true};
 std::vector<r::NormalizedModelStateEntry> ordinary;
 r::detail::TrainingContinuationValues values;
 r::TrainingSnapshot snapshot;
};
const torch::Tensor& entry(const r::DecodedNativeModelState& state, std::string_view name) {
 const auto found = std::ranges::find(state.entries(), name, &r::NormalizedModelStateEntry::name);
 REQUIRE(found != state.entries().end());
 return found->tensor;
}
void require_archive_equal(torch::serialize::InputArchive& left, torch::serialize::InputArchive& right) {
 REQUIRE(left.keys() == right.keys());
 for (const auto& key : left.keys()) {
  torch::serialize::InputArchive child;
  if (left.try_read(key, child)) {
   torch::serialize::InputArchive other;
   REQUIRE(right.try_read(key, other));
   require_archive_equal(child, other);
  } else {
   c10::IValue value, other;
   left.read(key, value);
   right.read(key, other);
   if (value.isTensor()) {
    REQUIRE(other.isTensor());
    REQUIRE(torch::equal(value.toTensor(), other.toTensor()));
   } else
    REQUIRE(value == other);
  }
 }
}
}  // namespace
extern "C" cudaError_t __real_cudaMemcpyAsync(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t);
extern "C" cudaError_t __wrap_cudaMemcpyAsync(void* destination, const void* source, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream) {
 const auto status = __real_cudaMemcpyAsync(destination, source, bytes, kind, stream);
 if (status == cudaSuccess && active_copies && kind == cudaMemcpyDeviceToHost) {
  ++active_copies->reads[source];
  active_copies->bytes += bytes;
 }
 return status;
}
TEST_CASE("one publication reuses ordinary EMA and Resume values and appends only optimizer slots", "[rfdetr][training][snapshot][cuda]") {
 const bool cuda = GENERATE(false, true);
 const auto policy = snapshot_policy(cuda);
 mmltk::testsupport::ScopedTempDir directory("training-snapshot");
 SnapshotFixture fixture(directory.path(), cuda);
 const auto expected_ordinary = fixture.parameters["value"].detach().cpu().clone();
 const auto expected_ema = fixture.ema.shadow_params().front().cpu().clone();
 CountCopies accounting;
 auto publication = fixture.begin();
 // Three ordinary tensors and two EMA tensors, including full Resume-only state.
 REQUIRE(accounting.copies.count() == (cuda ? 5U : 0U));
 REQUIRE(accounting.copies.bytes == (cuda ? 32U : 0U));
 REQUIRE_THROWS(fixture.begin());
 const auto ordinary_path = directory.path() / "ordinary.pt", ema_path = directory.path() / "ema.pt";
 fixture.snapshot.save_weights(ordinary_path, r::testsupport::synthetic_training_metadata(), false, {});
 fixture.snapshot.save_weights(ema_path, r::testsupport::synthetic_training_metadata(), true, {});
 fixture.snapshot.save_weights(directory.path() / "ordinary-again.pt", r::testsupport::synthetic_training_metadata(), false, {});
 REQUIRE(accounting.copies.count() == (cuda ? 5U : 0U));
 fixture.resume(directory.path() / "resume.pt");
 // AdamW's one initialized parameter adds step, first and second moments once.
 REQUIRE(accounting.copies.count() == (cuda ? 8U : 0U));
 REQUIRE(accounting.copies.bytes == (cuda ? 52U : 0U));
 fixture.resume(directory.path() / "resume-again.pt");
 REQUIRE(accounting.copies.count() == (cuda ? 8U : 0U));
 for (const auto& [source, count] : accounting.copies.reads) REQUIRE(count == 1);
 const auto* address = Access::view(fixture.snapshot, 0).const_data_ptr();
 const auto capacity = Access::capacity(fixture.snapshot);
 {
  auto held = Access::view(fixture.snapshot, 0).detach();
  REQUIRE_THROWS(publication.finish());
  REQUIRE_THROWS(fixture.snapshot.require_inactive());
 }
 publication.finish();
 REQUIRE_NOTHROW(fixture.snapshot.require_inactive());
 auto ordinary = r::decode_native_model_state(ordinary_path), selected = r::decode_native_model_state(ema_path);
 auto resume = r::decode_native_model_state(directory.path() / "resume.pt"), repeated = r::decode_native_model_state(directory.path() / "resume-again.pt");
 REQUIRE(ordinary.entries().size() == 2);
 REQUIRE(selected.entries().size() == 2);
 REQUIRE(resume.entries().size() == 3);
 REQUIRE(torch::equal(entry(ordinary, "value"), expected_ordinary));
 REQUIRE(torch::equal(entry(selected, "value"), expected_ema));
 REQUIRE(torch::equal(entry(resume, "value"), expected_ordinary));
 REQUIRE(torch::equal(entry(ordinary, "counter"), entry(selected, "counter")));
 REQUIRE(torch::equal(entry(ordinary, "counter"), entry(resume, "counter")));
 require_archive_equal(*resume.admitted_archive(), *repeated.admitted_archive());
 const auto continuation = r::detail::read_training_continuation(*resume.admitted_archive());
 REQUIRE(continuation);
 REQUIRE(continuation->values.schedule == fixture.values.schedule);
 REQUIRE(continuation->values.grad_scaler_scale == 128);
 REQUIRE(continuation->values.ema_completed_updates == 1);
 torch::serialize::InputArchive shadows;
 resume.admitted_archive()->read("ema_state", shadows);
 const auto restored_ema = r::detail::read_ema_shadow_archive(shadows, fixture.built.optimizer.eligible_parameter_names());
 REQUIRE(torch::equal(restored_ema.front(), expected_ema));
 // Retained core file evidence remains usable independently of a heavy decode.
 r::TrainingArtifactAdmission admission(ema_path);
 const auto retained = admission.decode();
 admission.release_decoded_state();
 REQUIRE_NOTHROW(admission.require_unchanged());
 REQUIRE(torch::equal(entry(*retained, "value"), expected_ema));
 {
  torch::NoGradGuard guard;
  fixture.parameters["value"].add_(5);
 }
 auto changed = fixture.begin();
 REQUIRE(Access::view(fixture.snapshot, 0).const_data_ptr() == address);
 REQUIRE(Access::capacity(fixture.snapshot) == capacity);
 REQUIRE(torch::equal(Access::view(fixture.snapshot, 0), expected_ordinary + 5));
 REQUIRE(accounting.copies.count() == (cuda ? 13U : 0U));
 fixture.snapshot.save_weights(directory.path() / "changed.pt", r::testsupport::synthetic_training_metadata(), false, {});
 changed.finish();
 REQUIRE(torch::equal(entry(r::decode_native_model_state(directory.path() / "changed.pt"), "value"), expected_ordinary + 5));
}
TEST_CASE("publication scope releases serializer failures and preserves the complete session generation", "[rfdetr][training][snapshot]") {
 mmltk::testsupport::ScopedTempDir directory("training-snapshot-failure");
 SnapshotFixture fixture(directory.path(), false);
 r::TrainingSessionCheckpoint checkpoint(directory.path());
 r::TrainingSessionManifest manifest;
 manifest.session_id = "session";
 manifest.attempt_id = "attempt";
 manifest.epoch = 1;
 manifest.initialization = std::string(64, '1');
 manifest.configuration = std::string(64, '2');
 manifest.validation = std::string(64, '3');
 manifest.precision = r::TrainingPrecisionKind::Float16;
 manifest.request = fixture.request;
 manifest.models.push_back({0, {}, {}, 0, {}});
 const r::TrainingPlanState plan{1, {{0, 42, {0}, {1}, {}}}};
 {
  auto publication = fixture.begin();
  fixture.snapshot.save_weights(directory.path() / "individual.pt", r::testsupport::synthetic_training_metadata(), false, {});
  checkpoint.publish(manifest, plan, [&](const auto& path, std::size_t) { fixture.resume(path); });
  publication.finish();
 }
 r::TrainingArtifactAdmission individual(directory.path() / "individual.pt");
 const auto generation = manifest.generation;
 REQUIRE_THROWS([&] {
  auto publication = fixture.begin();
  checkpoint.publish(manifest, plan, [&](const auto& path, std::size_t) {
   fixture.resume(path);
   torch::serialize::OutputArchive failing;
   failing.write("retained", Access::view(fixture.snapshot, 0));
   // OutputArchive owns the frozen view when opening its output fails.
   failing.save_to((directory.path() / "missing" / "unwritable.pt").string());
  });
 }());
 REQUIRE_NOTHROW(fixture.snapshot.require_inactive());
 REQUIRE_NOTHROW(individual.require_unchanged());
 r::TrainingSessionAdmission saved(checkpoint.path());
 REQUIRE(saved.manifest().generation == generation);
 REQUIRE(manifest.generation == generation);
 saved.release_decoded_state();
 REQUIRE_NOTHROW(saved.require_unchanged());
 auto next = fixture.begin();
 {
  torch::serialize::OutputArchive retained;
  retained.write("held", Access::view(fixture.snapshot, 0));
  REQUIRE_THROWS(next.finish());
 }
 next.finish();
 REQUIRE_NOTHROW(fixture.snapshot.require_inactive());
}
TEST_CASE("ordinary publication excludes stale EMA after released storage is reused", "[rfdetr][training][snapshot]") {
 mmltk::testsupport::ScopedTempDir directory("training-snapshot-ordinary");
 SnapshotFixture fixture(directory.path(), false);
 const std::array invalid{r::NormalizedModelStateEntry{"undefined", {}}};
 REQUIRE_THROWS(fixture.snapshot.begin(invalid, {}, nullptr));
 REQUIRE_NOTHROW(fixture.snapshot.require_inactive());
 {
  auto publication = fixture.begin();
  publication.finish();
 }
 fixture.request.use_ema = false;
 auto publication = fixture.snapshot.begin(fixture.ordinary, {}, nullptr);
 REQUIRE_THROWS(fixture.snapshot.save_weights(directory.path() / "unadmitted-ema.pt", r::testsupport::synthetic_training_metadata(), true, {}));
 fixture.snapshot.save_resume(
  directory.path() / "ordinary-resume.pt", r::testsupport::synthetic_training_metadata(), fixture.built.optimizer, fixture.scaler, fixture.request, 0, 0, "attempt", {}, fixture.values);
 publication.finish();
 auto saved = r::decode_native_model_state(directory.path() / "ordinary-resume.pt");
 torch::serialize::InputArchive unwanted;
 REQUIRE_FALSE(saved.admitted_archive()->try_read("ema_state", unwanted));
 const auto continuation = r::detail::read_training_continuation(*saved.admitted_archive());
 REQUIRE(continuation);
 REQUIRE(continuation->values.ema_completed_updates == 0);
}
TEST_CASE("test_ema_selection_restores_identity_and_mode", "[model][rfdetr][training][ema][snapshot]") {
 r::NativeRfDetrConfig config;
 config.num_classes = 3;
 config.num_queries = 2;
 config.num_select = 2;
 config.dec_layers = 1;
 config.hidden_dim = 8;
 config.ca_nheads = 2;
 config.training_supervision = {};
 r::NativeRfDetrModel module(config, r::testsupport::synthetic_training_layout(config.num_classes - 1));
 module.train();
 const auto named = module.named_parameters();
 std::vector<torch::Tensor> parameters{named["class_embed.weight"], named["class_embed.bias"]};
 r::ModelEma ema(parameters, 0.5, 0.0);
 auto original = parameters.front().detach().clone();
 auto* identity = parameters.front().unsafeGetTensorImpl();
 {
  torch::NoGradGuard guard;
  parameters.front().add_(2.0);
 }
 ema.update();
 REQUIRE(ema.completed_updates() == 1);
 {
  torch::NoGradGuard guard;
  parameters.front().add_(3);
 }
 const auto ordinary = parameters.front().detach().clone();
 const std::vector<r::NormalizedModelStateEntry> state{{"class_embed.weight", parameters.front()}, {"class_embed.bias", parameters.back()}};
 const std::vector<std::string> names{"class_embed.weight", "class_embed.bias"};
 mmltk::testsupport::ScopedTempDir directory("snapshot-ema-selection");
 auto metadata = r::testsupport::synthetic_training_metadata();
 metadata.class_layout = r::testsupport::synthetic_training_layout(2);
 metadata.num_classes = 3;
 r::TrainingSnapshot snapshot;
 auto publication = snapshot.begin(state, names, &ema);
 try {
  r::ModelEma::Selection selection(ema, module);
  module.eval();
  REQUIRE(parameters.front().unsafeGetTensorImpl() == identity);
  REQUIRE(torch::allclose(parameters.front(), original + 2.0));
  REQUIRE_THROWS(ema.update());
  REQUIRE(ema.completed_updates() == 1);
  snapshot.save_weights(directory.path() / "ordinary.pt", metadata, false, {});
  snapshot.save_weights(directory.path() / "ema.pt", metadata, true, {});
  REQUIRE(torch::equal(Access::view(snapshot, 0), ordinary));
  throw std::runtime_error("selected evaluation failed");
 } catch (const std::runtime_error&) {}
 REQUIRE(module.is_training());
 REQUIRE(parameters.front().unsafeGetTensorImpl() == identity);
 REQUIRE(torch::equal(parameters.front(), ordinary));
 publication.finish();
 REQUIRE(torch::equal(entry(r::decode_native_model_state(directory.path() / "ordinary.pt"), "class_embed.weight"), ordinary));
 REQUIRE(torch::equal(entry(r::decode_native_model_state(directory.path() / "ema.pt"), "class_embed.weight"), original + 2));
 auto adopted = r::ModelEma::from_cpu_shadow(parameters, ema.shadow_params(), 0.5, 0.0, ema.completed_updates());
 REQUIRE(adopted.completed_updates() == ema.completed_updates());
 REQUIRE(torch::equal(adopted.shadow_params().front(), ema.shadow_params().front()));
 auto malformed = ema.shadow_params();
 malformed.back() = torch::full_like(malformed.back(), std::numeric_limits<float>::quiet_NaN());
 REQUIRE_THROWS(r::ModelEma::from_cpu_shadow(parameters, malformed, 0.5, 0.0, ema.completed_updates()));
}
