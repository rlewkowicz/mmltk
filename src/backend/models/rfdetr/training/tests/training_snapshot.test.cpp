#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "detail/training_snapshot.h"
#include "detail/training_artifact.h"
#include "detail/training_session_checkpoint.h"
#include "detail/training_identity_v1.h"
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
#include <concepts>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

namespace {
namespace identity_v1 = r::detail::training_identity_v1;
namespace reflection = mmltk::frameworks::reflection;
template <class Owner, auto Member, class Type>
consteval bool historical_field(std::size_t index) {
 return std::same_as<std::remove_cvref_t<decltype(std::declval<Owner>().*Member)>, Type> && reflection::member_index<Member>(reflection::field_declarations<Owner>()) == index;
}
// Independent persisted shape assertions, transcribed from 1c13fec8. These do
// not derive expected bytes from the converter or today's configuration.
using HistoricalConfig = identity_v1::Configuration;
static_assert(reflection::field_declarations<HistoricalConfig>().size() == 35U);
static_assert(
 historical_field<HistoricalConfig, &HistoricalConfig::preset_name, std::string>(0) &&
 historical_field<HistoricalConfig, &HistoricalConfig::resolution, int>(1) &&
 historical_field<HistoricalConfig, &HistoricalConfig::segmentation, bool>(2) &&
 historical_field<HistoricalConfig, &HistoricalConfig::num_classes, int>(3) &&
 historical_field<HistoricalConfig, &HistoricalConfig::num_queries, int>(4) &&
 historical_field<HistoricalConfig, &HistoricalConfig::num_select, int>(5) &&
 historical_field<HistoricalConfig, &HistoricalConfig::dec_layers, int>(6) &&
 historical_field<HistoricalConfig, &HistoricalConfig::group_detr, int>(7) &&
 historical_field<HistoricalConfig, &HistoricalConfig::two_stage, bool>(8) &&
 historical_field<HistoricalConfig, &HistoricalConfig::hidden_dim, int>(9) &&
 historical_field<HistoricalConfig, &HistoricalConfig::patch_size, int>(10) &&
 historical_field<HistoricalConfig, &HistoricalConfig::num_windows, int>(11) &&
 historical_field<HistoricalConfig, &HistoricalConfig::positional_encoding_size, int>(12) &&
 historical_field<HistoricalConfig, &HistoricalConfig::sa_nheads, int>(13) &&
 historical_field<HistoricalConfig, &HistoricalConfig::ca_nheads, int>(14) &&
 historical_field<HistoricalConfig, &HistoricalConfig::dec_n_points, int>(15) &&
 historical_field<HistoricalConfig, &HistoricalConfig::dim_feedforward, int>(16) &&
 historical_field<HistoricalConfig, &HistoricalConfig::bbox_reparam, bool>(17) &&
 historical_field<HistoricalConfig, &HistoricalConfig::lite_refpoint_refine, bool>(18) &&
 historical_field<HistoricalConfig, &HistoricalConfig::cls_loss_coef, double>(19) &&
 historical_field<HistoricalConfig, &HistoricalConfig::bbox_loss_coef, double>(20) &&
 historical_field<HistoricalConfig, &HistoricalConfig::giou_loss_coef, double>(21) &&
 historical_field<HistoricalConfig, &HistoricalConfig::mask_ce_loss_coef, double>(22) &&
 historical_field<HistoricalConfig, &HistoricalConfig::mask_dice_loss_coef, double>(23) &&
 historical_field<HistoricalConfig, &HistoricalConfig::sum_group_losses, bool>(24) &&
 historical_field<HistoricalConfig, &HistoricalConfig::use_varifocal_loss, bool>(25) &&
 historical_field<HistoricalConfig, &HistoricalConfig::use_position_supervised_loss, bool>(26) &&
 historical_field<HistoricalConfig, &HistoricalConfig::ia_bce_loss, bool>(27) &&
 historical_field<HistoricalConfig, &HistoricalConfig::aux_loss, bool>(28) &&
 historical_field<HistoricalConfig, &HistoricalConfig::mask_point_sample_ratio, std::int64_t>(29) &&
 historical_field<HistoricalConfig, &HistoricalConfig::focal_alpha, double>(30) &&
 historical_field<HistoricalConfig, &HistoricalConfig::set_cost_class, double>(31) &&
 historical_field<HistoricalConfig, &HistoricalConfig::set_cost_bbox, double>(32) &&
 historical_field<HistoricalConfig, &HistoricalConfig::set_cost_giou, double>(33) &&
 historical_field<HistoricalConfig, &HistoricalConfig::training_supervision, identity_v1::TrainingSupervisionConfig>(34));
using HistoricalMatchFree = identity_v1::MatchFreeSupervisionConfig;
using HistoricalDenoising = identity_v1::DenoisingSupervisionConfig;
using HistoricalSupervision = identity_v1::TrainingSupervisionConfig;
static_assert(reflection::field_declarations<HistoricalMatchFree>().size() == 3U &&
 historical_field<HistoricalMatchFree, &HistoricalMatchFree::rho, float>(0) &&
 historical_field<HistoricalMatchFree, &HistoricalMatchFree::correspondence_weight, float>(1) &&
 historical_field<HistoricalMatchFree, &HistoricalMatchFree::query_weight, float>(2));
static_assert(reflection::field_declarations<HistoricalDenoising>().size() == 5U &&
 historical_field<HistoricalDenoising, &HistoricalDenoising::enabled, bool>(0) &&
 historical_field<HistoricalDenoising, &HistoricalDenoising::groups, std::uint16_t>(1) &&
 historical_field<HistoricalDenoising, &HistoricalDenoising::label_noise_ratio, float>(2) &&
 historical_field<HistoricalDenoising, &HistoricalDenoising::center_noise_scale, float>(3) &&
 historical_field<HistoricalDenoising, &HistoricalDenoising::size_noise_scale, float>(4));
static_assert(reflection::field_declarations<HistoricalSupervision>().size() == 3U &&
 historical_field<HistoricalSupervision, &HistoricalSupervision::assignment, identity_v1::TrainAssignmentKind>(0) &&
 historical_field<HistoricalSupervision, &HistoricalSupervision::match_free, HistoricalMatchFree>(1) &&
 historical_field<HistoricalSupervision, &HistoricalSupervision::denoising, HistoricalDenoising>(2));
using HistoricalName = identity_v1::ClassName;
using HistoricalCatalog = identity_v1::OrderedClassCatalog;
using HistoricalSlot = identity_v1::ModelClassSlot;
using HistoricalProvenance = identity_v1::ClassLayoutProvenance;
using HistoricalLayout = identity_v1::ModelClassLayout;
static_assert(reflection::field_declarations<HistoricalName>().size() == 1U && historical_field<HistoricalName, &HistoricalName::value, std::string>(0));
static_assert(reflection::field_declarations<HistoricalCatalog>().size() == 1U && historical_field<HistoricalCatalog, &HistoricalCatalog::names, std::vector<HistoricalName>>(0));
static_assert(reflection::field_declarations<HistoricalSlot>().size() == 2U &&
 historical_field<HistoricalSlot, &HistoricalSlot::role, identity_v1::ClassSlotRole>(0) &&
 historical_field<HistoricalSlot, &HistoricalSlot::foreground_index, std::optional<std::uint32_t>>(1));
static_assert(reflection::field_declarations<HistoricalProvenance>().size() == 3U &&
 historical_field<HistoricalProvenance, &HistoricalProvenance::origin, identity_v1::ClassLayoutOrigin>(0) &&
 historical_field<HistoricalProvenance, &HistoricalProvenance::producer, std::string>(1) &&
 historical_field<HistoricalProvenance, &HistoricalProvenance::artifact_sha256, std::string>(2));
static_assert(reflection::field_declarations<HistoricalLayout>().size() == 8U &&
 historical_field<HistoricalLayout, &HistoricalLayout::version, std::uint32_t>(0) &&
 historical_field<HistoricalLayout, &HistoricalLayout::foreground, HistoricalCatalog>(1) &&
 historical_field<HistoricalLayout, &HistoricalLayout::class_name_evidence, HistoricalCatalog>(2) &&
 historical_field<HistoricalLayout, &HistoricalLayout::slots, std::vector<HistoricalSlot>>(3) &&
 historical_field<HistoricalLayout, &HistoricalLayout::scores, identity_v1::ClassScoreEncoding>(4) &&
 historical_field<HistoricalLayout, &HistoricalLayout::no_object, identity_v1::NoObjectEncoding>(5) &&
 historical_field<HistoricalLayout, &HistoricalLayout::provenance, HistoricalProvenance>(6) &&
 historical_field<HistoricalLayout, &HistoricalLayout::supervision_in_foreground_order, bool>(7));
static_assert(identity_v1::kSupervisionLimits.max_bytes == 4096U && identity_v1::kSupervisionLimits.max_items == 128U && identity_v1::kSupervisionLimits.max_depth == 16U);
static_assert(identity_v1::kClassLayoutLimits.max_bytes == 1048576U && identity_v1::kClassLayoutLimits.max_items == 1048576U && identity_v1::kClassLayoutLimits.max_depth == 64U);
static_assert(reflection::policy_of_member<&HistoricalName::value>().maximum_bytes == 256U);
static_assert(reflection::policy_of_member<&HistoricalCatalog::names>().maximum_items == 256U);
static_assert(reflection::policy_of_member<&HistoricalLayout::slots>().maximum_items == 4096U);
static_assert(reflection::policy_of_member<&HistoricalProvenance::producer>().maximum_bytes == 1024U);
static_assert(reflection::policy_of_member<&HistoricalProvenance::artifact_sha256>().maximum_bytes == 64U);
template <class Enum, std::size_t Count>
consteval bool historical_enum(const std::array<std::string_view, Count>& spellings) {
 if constexpr (!std::same_as<std::underlying_type_t<Enum>, std::uint8_t> || reflection::enum_entries<Enum>().size() != Count) return false;
 const auto entries = reflection::enum_entries<Enum>();
 for (std::size_t index = 0; index < Count; ++index)
  if (entries[index].name != spellings[index] || static_cast<std::uint8_t>(entries[index].value) != index) return false;
 return true;
}
static_assert(historical_enum<identity_v1::TrainAssignmentKind>(std::array<std::string_view, 2>{"Hungarian", "MatchFree"}));
static_assert(historical_enum<identity_v1::ClassSlotRole>(std::array<std::string_view, 4>{"Foreground", "Background", "Unused", "Unresolved"}));
static_assert(historical_enum<identity_v1::ClassScoreEncoding>(std::array<std::string_view, 2>{"SigmoidLogits", "SoftmaxLogits"}));
static_assert(historical_enum<identity_v1::NoObjectEncoding>(std::array<std::string_view, 3>{"AllNegative", "ExplicitBackground", "Unspecified"}));
static_assert(historical_enum<identity_v1::ClassLayoutOrigin>(std::array<std::string_view, 5>{"Unresolved", "NativeTraining", "Embedded", "DigestDescriptor", "VerifiedAsset"}));

r::NativeRfDetrConfig historical_identity_configuration() {
 return {
  .preset_name = "identity-fixture", .resolution = 384, .task = r::ModelTask::Detection,
  .num_classes = 3, .num_queries = 7, .num_select = 5, .dec_layers = 3, .group_detr = 2, .two_stage = true,
  .hidden_dim = 256, .patch_size = 16, .num_windows = 2, .positional_encoding_size = 24, .sa_nheads = 8, .ca_nheads = 16,
  .dec_n_points = 4, .dim_feedforward = 1024, .bbox_reparam = false, .lite_refpoint_refine = true,
  .cls_loss_coef = 1.25, .bbox_loss_coef = 6.5, .giou_loss_coef = 2.25, .mask_ce_loss_coef = 0.75, .mask_dice_loss_coef = 1.5,
  .sum_group_losses = true, .use_varifocal_loss = true, .use_position_supervised_loss = true, .ia_bce_loss = false, .aux_loss = false,
  .mask_point_sample_ratio = 32, .focal_alpha = 0.125, .set_cost_class = 2.5, .set_cost_bbox = 5.5, .set_cost_giou = 3.0,
  .training_supervision = {
   .assignment = r::TrainAssignmentKind::Hungarian, .match_free = {.rho = 0.5F, .correspondence_weight = 1.0F, .query_weight = 2.0F},
   .denoising = {.enabled = false, .groups = 5U, .label_noise_ratio = 0.2F, .center_noise_scale = 0.4F, .size_noise_scale = 0.4F}}
 };
}
}  // namespace

TEST_CASE("Session identities bind joint metadata while baseline identities preserve original vocabulary", "[rfdetr][training][session][keypoints]") {
 // Fixed independent bytes follow the original train.cpp hexfloat stream and
 // sorted JSON encoding. The genuine old-application session case separately
 // compares this conversion against its retained producer signature.
 const bool segmentation = GENERATE(false, true);
 const unsigned supervision = GENERATE(0U, 1U, 2U, 3U);
 CAPTURE(segmentation, supervision);
 auto config = historical_identity_configuration();
 config.task = segmentation ? r::ModelTask::Segmentation : r::ModelTask::Detection;
 auto layout = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog(segmentation ? std::vector<std::string>{"ant", "zebra"} : std::vector<std::string>{"zebra", "ant"}));
 layout.class_name_evidence.names = {{""}, {"raw\"name"}, {"raw\"name"}};
 layout.slots = {{r::ClassSlotRole::Foreground, 1U}, {r::ClassSlotRole::Unused, std::nullopt}, {r::ClassSlotRole::Foreground, 0U}};
 layout.provenance = {.origin = r::ClassLayoutOrigin::VerifiedAsset, .producer = "original/\"asset\"\nprovider", .artifact_sha256 = std::string(64, 'a')};
 layout.supervision_in_foreground_order = false;
 std::string expected_supervision =
  R"({"assignment":"Hungarian","denoising":{"center_noise_scale":0.4000000059604645,"enabled":false,"groups":5,"label_noise_ratio":0.20000000298023224,"size_noise_scale":0.4000000059604645},"match_free":{"correspondence_weight":1.0,"query_weight":2.0,"rho":0.5}})";
 if (supervision == 1U || supervision == 3U) {
  config.training_supervision.assignment = r::TrainAssignmentKind::MatchFree;
  config.training_supervision.match_free = {.rho = 0.25F, .correspondence_weight = 0.5F, .query_weight = 1.5F};
  config.training_supervision.denoising = {.enabled = supervision == 3U, .groups = 7U, .label_noise_ratio = 0.125F, .center_noise_scale = 0.25F, .size_noise_scale = 0.75F};
  expected_supervision = supervision == 1U
   ? R"({"assignment":"MatchFree","denoising":{"center_noise_scale":0.25,"enabled":false,"groups":7,"label_noise_ratio":0.125,"size_noise_scale":0.75},"match_free":{"correspondence_weight":0.5,"query_weight":1.5,"rho":0.25}})"
   : R"({"assignment":"MatchFree","denoising":{"center_noise_scale":0.25,"enabled":true,"groups":7,"label_noise_ratio":0.125,"size_noise_scale":0.75},"match_free":{"correspondence_weight":0.5,"query_weight":1.5,"rho":0.25}})";
 } else if (supervision == 2U) {
  config.training_supervision.denoising = {.enabled = true, .groups = 3U, .label_noise_ratio = 0.5F, .center_noise_scale = 0.25F, .size_noise_scale = 0.75F};
  expected_supervision =
   R"({"assignment":"Hungarian","denoising":{"center_noise_scale":0.25,"enabled":true,"groups":3,"label_noise_ratio":0.5,"size_noise_scale":0.75},"match_free":{"correspondence_weight":1.0,"query_weight":2.0,"rho":0.5}})";
 }
 std::string expected_layout =
  R"({"class_name_evidence":{"names":[{"value":""},{"value":"raw\"name"},{"value":"raw\"name"}]},"foreground":{"names":[{"value":"zebra"},{"value":"ant"}]},"no_object":"AllNegative","provenance":{"artifact_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","origin":"VerifiedAsset","producer":"original/\"asset\"\nprovider"},"scores":"SigmoidLogits","slots":[{"foreground_index":1,"role":"Foreground"},{"role":"Unused"},{"foreground_index":0,"role":"Foreground"}],"supervision_in_foreground_order":false,"version":1})";
 if (segmentation) {
  layout.slots = {{r::ClassSlotRole::Background, std::nullopt}, {r::ClassSlotRole::Foreground, 1U}, {r::ClassSlotRole::Foreground, 0U}};
  layout.scores = r::ClassScoreEncoding::SoftmaxLogits;
  layout.no_object = r::NoObjectEncoding::ExplicitBackground;
  layout.provenance = {.origin = r::ClassLayoutOrigin::DigestDescriptor, .producer = "original\\descriptor", .artifact_sha256 = std::string(64, 'b')};
  layout.supervision_in_foreground_order = true;
  expected_layout =
   R"({"class_name_evidence":{"names":[{"value":""},{"value":"raw\"name"},{"value":"raw\"name"}]},"foreground":{"names":[{"value":"ant"},{"value":"zebra"}]},"no_object":"ExplicitBackground","provenance":{"artifact_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","origin":"DigestDescriptor","producer":"original\\descriptor"},"scores":"SoftmaxLogits","slots":[{"role":"Background"},{"foreground_index":1,"role":"Foreground"},{"foreground_index":0,"role":"Foreground"}],"supervision_in_foreground_order":true,"version":1})";
 }
 const std::string expected = std::string(segmentation ? "preset_name=identity-fixture;resolution=384;segmentation=1;" : "preset_name=identity-fixture;resolution=384;segmentation=0;") +
  "num_classes=3;num_queries=7;num_select=5;dec_layers=3;group_detr=2;two_stage=1;hidden_dim=256;patch_size=16;num_windows=2;"
  "positional_encoding_size=24;sa_nheads=8;ca_nheads=16;dec_n_points=4;dim_feedforward=1024;bbox_reparam=0;lite_refpoint_refine=1;"
  "cls_loss_coef=0x1.4p+0;bbox_loss_coef=0x1.ap+2;giou_loss_coef=0x1.2p+1;mask_ce_loss_coef=0x1.8p-1;mask_dice_loss_coef=0x1.8p+0;"
  "sum_group_losses=1;use_varifocal_loss=1;use_position_supervised_loss=1;ia_bce_loss=0;aux_loss=0;mask_point_sample_ratio=32;"
  "focal_alpha=0x1p-3;set_cost_class=0x1.4p+1;set_cost_bbox=0x1.6p+2;set_cost_giou=0x1.8p+1;" + expected_supervision + expected_layout;
 const auto baseline = r::training_configuration_identity(config, layout, 1);
 CHECK(baseline == expected);
 const auto unresolved = r::unresolved_class_layout(3);
 CHECK(r::training_configuration_identity(config, unresolved, 1) == expected.substr(0, expected.size() - expected_layout.size()) +
  R"({"class_name_evidence":{"names":[]},"foreground":{"names":[]},"no_object":"Unspecified","provenance":{"artifact_sha256":"","origin":"Unresolved","producer":""},"scores":"SigmoidLogits","slots":[{"role":"Unresolved"},{"role":"Unresolved"},{"role":"Unresolved"}],"supervision_in_foreground_order":false,"version":1})");
 const auto current = r::training_configuration_identity(config, layout, 2);
 CHECK(current == r::encode_checkpoint_configuration(config) + r::encode_class_layout(layout));
 CHECK(current != baseline);

 std::array current_only_changes{config, config, config};
 current_only_changes[0].keypoint_l1_loss_coef = 0.5;
 current_only_changes[1].backbone.dimension = 768U;
 current_only_changes[2].backbone.features = {2U, 5U, 8U, 11U};
 for (const auto& changed : current_only_changes) {
  CHECK(r::training_configuration_identity(changed, layout, 1) == baseline);
  CHECK(r::training_configuration_identity(changed, layout, 2) != current);
 }
 auto changed = config;
 auto changed_layout = layout;
 std::swap(changed_layout.foreground.names[0], changed_layout.foreground.names[1]);
 CHECK(r::training_configuration_identity(config, changed_layout, 1) != baseline);
 changed_layout = layout;
 changed_layout.provenance.producer += "-changed";
 CHECK(r::training_configuration_identity(config, changed_layout, 1) != baseline);

 mmltk::backend::data::FileHeader header{};
 mmltk::common::io::FileSnapshot file{.bytes = 1024, .modified_seconds = 7};
 std::array<std::byte, 4> metadata{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
 const auto identity = r::training_dataset_identity(header, file, baseline, metadata);
 metadata.back() = std::byte{5};
 CHECK(r::training_dataset_identity(header, file, baseline, metadata) != identity);
 REQUIRE_THROWS(r::training_configuration_identity(config, layout, 0));
 REQUIRE_THROWS(r::training_configuration_identity(config, layout, 3));
 for (const auto task : {r::ModelTask::Keypoints, static_cast<r::ModelTask>(255)}) {
  changed = config;
  changed.task = task;
  REQUIRE_THROWS(r::training_configuration_identity(changed, layout, 1));
 }
 for (const auto backbone : {r::BackboneKind::DinoV2Base, r::BackboneKind::PeCoreTiny}) {
  changed = config;
  changed.backbone.kind = backbone;
  REQUIRE_THROWS(r::training_configuration_identity(changed, layout, 1));
 }
 using KeypointSchema = mmltk::backend::data::catalog::ClassKeypointSchema;
 for (const bool evidence : {false, true}) {
  changed_layout = layout;
  auto& catalog = evidence ? changed_layout.class_name_evidence : changed_layout.foreground;
  catalog.keypoints = std::vector<KeypointSchema>(catalog.names.size());
  catalog.keypoints->front() = {.names = {"joint"}, .horizontal_flip = {0U}, .oks_sigmas = {0.05}};
  REQUIRE_THROWS(r::training_configuration_identity(config, changed_layout, 1));
  const auto with_joints = r::training_configuration_identity(config, changed_layout, 2);
  CHECK(with_joints != current);
  catalog.keypoints->front().names.front() = "renamed-joint";
  CHECK(r::training_configuration_identity(config, changed_layout, 2) != with_joints);
 }

 changed = config;
 changed.training_supervision.denoising.groups = 65U;
 REQUIRE_THROWS(r::training_configuration_identity(changed, layout, 1));
 changed = config;
 changed.training_supervision.match_free.rho = 1.0F;
 REQUIRE_THROWS(r::training_configuration_identity(changed, layout, 1));
 changed = config;
 changed.training_supervision.assignment = static_cast<r::TrainAssignmentKind>(255);
 REQUIRE_THROWS(r::training_configuration_identity(changed, layout, 1));
 changed_layout = layout;
 changed_layout.provenance.producer.assign(1025U, 'x');
 REQUIRE_THROWS(r::training_configuration_identity(config, changed_layout, 1));
 changed_layout = layout;
 changed_layout.slots.front().role = static_cast<r::ClassSlotRole>(255);
 REQUIRE_THROWS(r::training_configuration_identity(config, changed_layout, 1));
}
