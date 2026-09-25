#include <algorithm>
#include "src/backend/models/rfdetr/training/telemetry_writer.h"
#include "src/frameworks/serialization/reflected_json.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <array>
#include <cerrno>
#include <iterator>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
namespace r = mmltk::backend::models::rfdetr;
void test_telemetry_persistence_failure_is_nonfatal() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-failure"};
 r::TrainingRun run;
 run.configuration.output_dir = temp.path() / "missing" / "directory";
 r::TrainingTelemetryWriter writer(run);
 r::TrainingMetricProgress progress;
 progress.phase = r::TrainingPhase::Completed;
 writer.Submit(progress, r::TrainingRecordRole::Epoch);
 writer.Finish(progress, {});
 writer.Close();
 REQUIRE(writer.persistence().degraded);
 REQUIRE_FALSE(writer.persistence().error.empty());
}
void test_telemetry_pressure_preserves_a_terminal_boundary() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-pressure"};
 r::TrainingRun run;
 run.configuration.output_dir = temp.path();
 r::TrainingTelemetryWriter writer(run);
 r::TrainingMetricProgress progress;
 progress.phase = r::TrainingPhase::Train;
 progress.total_images = 4096U * 7U;
 for (int sample = 0; sample < 4096; ++sample) {
  progress.optimizer_steps = sample;
  progress.completed_images = static_cast<std::uint64_t>(sample + 1) * 7U;
  writer.Submit(progress, r::TrainingRecordRole::Live);
 }
 progress.phase = r::TrainingPhase::Completed;
 writer.Finish(progress, {});
 writer.Close();
 std::ifstream stream(temp.path() / "metrics.jsonl");
 std::string line;
 std::optional<r::TrainingRecord> previous;
 while (std::getline(stream, line)) {
  auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
  if (previous) REQUIRE(record.sequence > previous->sequence);
  previous = std::move(record);
 }
 REQUIRE(previous.has_value());
 REQUIRE(previous->role == r::TrainingRecordRole::Terminal);
 REQUIRE(previous->sequence == 4096);
 REQUIRE(previous->format_version == 3);
 REQUIRE(previous->progress.completed_images == 4096U * 7U);
 REQUIRE(previous->progress.total_images == previous->progress.completed_images);
 REQUIRE(previous->dropped_before == writer.persistence().dropped_records);
}
}  // namespace
TEST_CASE("test_telemetry_persistence_failure_is_nonfatal", "[model][rfdetr][training][telemetry]") { test_telemetry_persistence_failure_is_nonfatal(); }
TEST_CASE("test_telemetry_pressure_preserves_a_terminal_boundary", "[model][rfdetr][training][telemetry]") { test_telemetry_pressure_preserves_a_terminal_boundary(); }
namespace {
// The writer's actual Initialize open blocks on this FIFO until Release. No
// sleeps, scheduler assumptions or production-only persistence hooks are used.
class HeldHistory final {
public:
 explicit HeldHistory(const std::filesystem::path& directory, r::TrainingRun run = {}) : path_(directory / "metrics.jsonl") {
  if (::mkfifo(path_.c_str(), 0600) != 0) throw std::runtime_error("create telemetry history FIFO");
  run.configuration.output_dir = directory;
  writer = std::make_unique<r::TrainingTelemetryWriter>(std::move(run));
 }
 ~HeldHistory() { Drain(); }
 void Drain() {
  if (drained_) return;
  const int stream = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
  if (stream < 0) std::terminate();  // fixture cannot leave its writer blocked
  std::jthread reader([&] {
   std::array<char, 4096> bytes{};
   for (;;) {
    const auto count = ::read(stream, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return;
    for (ssize_t index = 0; index < count; ++index) {
     if (bytes[index] == '\0') return;
     history.push_back(bytes[index]);
    }
   }
  });
  writer->Close();
  const char end = '\0';
  while (::write(stream, &end, 1) < 0 && errno == EINTR) {}
  reader.join();
  ::close(stream);
  drained_ = true;
 }
 std::unique_ptr<r::TrainingTelemetryWriter> writer;
 std::string history;

private:
 std::filesystem::path path_;
 bool drained_ = false;
};
class PersistenceNotice final {
public:
 explicit PersistenceNotice(const std::filesystem::path& directory) : path_(directory / "stderr.txt") {
  saved_ = ::dup(STDERR_FILENO);
  const int output = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (saved_ < 0 || output < 0 || ::dup2(output, STDERR_FILENO) < 0) std::terminate();
  ::close(output);
 }
 ~PersistenceNotice() {
  ::dup2(saved_, STDERR_FILENO);
  ::close(saved_);
 }
 bool observed() const {
  std::ifstream stream(path_);
  const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
  return text.find(r::kTrainingPersistenceFailureLine) != std::string::npos;
 }

private:
 std::filesystem::path path_;
 int saved_ = -1;
};
void test_telemetry_boundary_pressure_reserves_epoch_and_terminal() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-boundaries"};
 PersistenceNotice notice(temp.path());
 HeldHistory held(temp.path());
 r::TrainingMetricProgress progress;
 progress.phase = r::TrainingPhase::Validate;
 // With persistence held, these cannot drain. Later boundary attempts must
 // reject rather than replacing any earlier accepted non-live observation.
 for (int index = 0; index < 32; ++index) {
  progress.optimizer_steps = index;
  held.writer->Submit(progress, r::TrainingRecordRole::Boundary);
 }
 progress.phase = r::TrainingPhase::EpochComplete;
 progress.full_checkpoint_path = temp.path() / "checkpoint.pt";
 held.writer->Submit(progress, r::TrainingRecordRole::Epoch);
 held.writer->Submit(progress, r::TrainingRecordRole::Boundary);  // still full
 progress.phase = r::TrainingPhase::Completed;
 held.writer->Finish(progress, {});
 held.Drain();
 std::istringstream stream(held.history);
 std::string line;
 std::vector<r::TrainingRecord> records;
 while (std::getline(stream, line))
  records.push_back(mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32}));
 REQUIRE(records.size() > 2);
 for (std::size_t index = 1; index < records.size(); ++index) REQUIRE(records[index].sequence > records[index - 1].sequence);
 for (std::size_t index = 0; index + 2 < records.size(); ++index) {
  REQUIRE(records[index].role == r::TrainingRecordRole::Boundary);
  REQUIRE(records[index].sequence == index);
  REQUIRE(records[index].progress.optimizer_steps == static_cast<std::int64_t>(index));
 }
 REQUIRE(records[records.size() - 2].role == r::TrainingRecordRole::Epoch);
 REQUIRE(records[records.size() - 2].sequence == 32);
 REQUIRE(records.back().role == r::TrainingRecordRole::Terminal);
 REQUIRE(records.back().sequence == 34);
 REQUIRE(records.back().progress.full_checkpoint_path == progress.full_checkpoint_path);
 REQUIRE(records.back().dropped_before == 35 - records.size());
 REQUIRE(records.back().dropped_before == held.writer->persistence().dropped_records);
 REQUIRE(held.writer->persistence().degraded);
 REQUIRE(notice.observed());
 REQUIRE(std::filesystem::file_size(temp.path() / "log.txt") > 0);
}
void test_invalid_terminal_has_truthful_degraded_custody_and_notice() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-invalid-terminal"};
 PersistenceNotice notice(temp.path());
 HeldHistory held(temp.path());
 r::TrainingMetricProgress progress;
 progress.phase = r::TrainingPhase::Completed;
 progress.checkpoint_path = std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'x');
 progress.full_checkpoint_path = temp.path() / "checkpoint.pt";
 held.writer->Finish(progress, {});
 held.Drain();
 const auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(held.history, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
 REQUIRE(record.role == r::TrainingRecordRole::Terminal);
 REQUIRE(record.progress.phase == r::TrainingPhase::Error);
 REQUIRE(record.progress.checkpoint_path.empty());
 REQUIRE(record.progress.full_checkpoint_path == progress.full_checkpoint_path);
 REQUIRE(record.dropped_before == 1);
 REQUIRE(held.writer->persistence().degraded);
 REQUIRE(notice.observed());
 REQUIRE_FALSE(std::filesystem::exists(temp.path() / "results.json"));
}
void test_rejected_only_submission_notifies_before_empty_close() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-empty-close"};
 PersistenceNotice notice(temp.path());
 HeldHistory held(temp.path());
 r::TrainingMetricProgress progress;
 progress.full_checkpoint_path = std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'x');
 held.writer->Submit(progress, r::TrainingRecordRole::Boundary);
 held.Drain();
 REQUIRE(held.history.empty());
 REQUIRE(held.writer->persistence().dropped_records == 1);
 REQUIRE(held.writer->persistence().degraded);
 REQUIRE(notice.observed());
}
void test_unencodable_terminal_notifies_without_later_submission() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-unencodable-terminal"};
 PersistenceNotice notice(temp.path());
 HeldHistory held(temp.path());
 r::TrainingMetricProgress progress;
 progress.phase = static_cast<r::TrainingPhase>(255);
 held.writer->Finish(progress, {});
 held.Drain();
 REQUIRE(held.history.empty());
 REQUIRE(held.writer->persistence().degraded);
 REQUIRE(held.writer->persistence().dropped_records == 1);
 REQUIRE(notice.observed());
 REQUIRE_FALSE(std::filesystem::exists(temp.path() / "results.json"));
}
void test_telemetry_distinct_heads_drain_by_sequence() {
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-ordered-heads"};
 HeldHistory held(temp.path());
 r::TrainingMetricProgress progress;
 progress.phase = r::TrainingPhase::Train;
 for (const auto role : {r::TrainingRecordRole::Boundary, r::TrainingRecordRole::Epoch, r::TrainingRecordRole::Live, r::TrainingRecordRole::Boundary}) held.writer->Submit(progress, role);
 progress.phase = r::TrainingPhase::Completed;
 held.writer->Finish(progress, {});
 held.Drain();
 std::istringstream stream(held.history);
 std::string line;
 std::uint64_t sequence = 0;
 while (std::getline(stream, line)) {
  const auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
  REQUIRE(record.sequence == sequence++);
  REQUIRE(record.dropped_before == 0);
 }
 REQUIRE(sequence == 5);
 REQUIRE_FALSE(held.writer->persistence().degraded);
}
}  // namespace
TEST_CASE("test_telemetry_boundary_pressure_reserves_epoch_and_terminal", "[model][rfdetr][training][telemetry]") { test_telemetry_boundary_pressure_reserves_epoch_and_terminal(); }
TEST_CASE("test_invalid_terminal_has_truthful_degraded_custody_and_notice", "[model][rfdetr][training][telemetry]") { test_invalid_terminal_has_truthful_degraded_custody_and_notice(); }
TEST_CASE("test_rejected_only_submission_notifies_before_empty_close", "[model][rfdetr][training][telemetry]") { test_rejected_only_submission_notifies_before_empty_close(); }
TEST_CASE("test_unencodable_terminal_notifies_without_later_submission", "[model][rfdetr][training][telemetry]") { test_unencodable_terminal_notifies_without_later_submission(); }
TEST_CASE("test_telemetry_distinct_heads_drain_by_sequence", "[model][rfdetr][training][telemetry]") { test_telemetry_distinct_heads_drain_by_sequence(); }
TEST_CASE("telemetry retains complete history JSON through progress epoch and terminal projections", "[model][rfdetr][training][telemetry]") {
 namespace serial = mmltk::frameworks::serialization;
 mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-projections"};
 HeldHistory held(temp.path());
 std::array<r::TrainingMetricProgress, 3U> progress{};
 progress[0].phase = r::TrainingPhase::Starting;
 progress[0].total_images = 4096U;
 progress[1] = progress[0];
 progress[1].phase = r::TrainingPhase::EpochComplete;
 progress[1].scope = r::TrainingRecordScope::Session;
 progress[1].completed_images = 4096U;
 progress[1].scalars.total = 0.125;
 progress[1].epoch_global_loss = 0.25;
 progress[1].checkpoint_path = temp.path() / "best.pt";
 progress[1].full_checkpoint_path = temp.path() / "session.json";
 progress[2] = progress[1];
 progress[2].phase = r::TrainingPhase::Completed;
 held.writer->Submit(progress[0], r::TrainingRecordRole::Boundary);
 held.writer->Submit(progress[1], r::TrainingRecordRole::Epoch);
 held.writer->Finish(progress[2], {.history_size = 1U});
 held.Drain();
 REQUIRE_FALSE(held.writer->persistence().degraded);
 const auto read_json = [&](const char* name) {
  std::ifstream input(temp.path() / name);
  REQUIRE(input.good());
  return nlohmann::json::parse(input);
 };
 const auto manifest = read_json("run.json");
 std::vector<std::byte> scratch(r::kTrainingManifestBytes);
 const serial::wire::Limits limits{.max_bytes = scratch.size(), .max_items = 65536U, .max_depth = 32U};
 r::TrainingRun run;
 run.configuration.output_dir = temp.path();
 run.run_id = manifest.at("run_id").get<std::string>();
 run.attempt_id = held.writer->attempt_id();
 run.checkpoint_attempt_id = run.attempt_id;
 run.sources = r::training_source_catalog(run.configuration);
 CHECK(manifest == serial::reflected_json(run, scratch, limits));
 const std::array roles{r::TrainingRecordRole::Boundary, r::TrainingRecordRole::Epoch, r::TrainingRecordRole::Terminal};
 std::array<nlohmann::json, 3U> records;
 std::istringstream history(held.history);
 for (std::size_t index = 0U; index < records.size(); ++index) {
  std::string line;
  REQUIRE(static_cast<bool>(std::getline(history, line)));
  r::TrainingRecord expected;
  expected.run_id = run.run_id;
  expected.attempt_id = run.attempt_id;
  expected.sequence = index;
  expected.role = roles[index];
  expected.progress = progress[index];
  if (index == 0U) expected.attempt_configuration = run.configuration;
  records[index] = serial::reflected_json(expected, scratch, limits);
  CHECK(nlohmann::json::parse(line) == records[index]);
  CHECK(line == records[index].dump());
 }
 std::string excess;
 CHECK_FALSE(static_cast<bool>(std::getline(history, excess)));
 CHECK(read_json("log.txt") == records[1]);
 r::TrainingProgressDocument terminal;
 terminal.record = serial::decode_reflected_json<r::TrainingRecord>(records[2].dump(), limits);
 terminal.final = r::TrainingFinalFacts{.history_size = 1U};
 terminal.sources.catalog = run.sources;
 terminal.sources.execution = run.execution;
 const auto expected = serial::reflected_json(terminal, scratch, limits);
 CHECK(read_json("progress.json") == expected);
 CHECK(read_json("results.json") == expected);
}

TEST_CASE("telemetry reserves both complete periodic epoch traffic shapes", "[model][rfdetr][training][telemetry]") {
 for (const bool ema : {false, true}) for (const bool overflow : {false, true}) {
  mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-epoch-burst"};
  r::TrainingRun run; run.run_id = "session";
  run.configuration.use_ema = ema;
  run.configuration.lane_configuration.mode = r::TrainLaneMode::PeriodicAveraging;
  r::resize_training_models(run.configuration.lane_configuration, r::kMaximumTrainingModels, run.configuration.recipe, run.configuration.seed);
  HeldHistory held(temp.path(), run);
  r::TrainingMetricProgress progress;
  progress.phase = r::TrainingPhase::EpochComplete;
  progress.session_id = run.run_id;
  const auto synchronized = [&] {
   auto evaluated = progress;
   evaluated.scope = r::TrainingRecordScope::SynchronizedSession; evaluated.model_id = 0;
   evaluated.distribution.reset(); evaluated.val.emplace(); evaluated.val->bbox.ap = .5;
   evaluated.artifact.emplace(); evaluated.artifact->session_id = run.run_id; evaluated.artifact->model_id = 1;
   evaluated.artifact->path = "synchronized.pt";
   held.writer->Submit(evaluated, r::TrainingRecordRole::Epoch);
  };
  if (ema) synchronized();
  for (std::uint64_t model = 1; model <= r::kMaximumTrainingModels; ++model) {
   progress.scope = r::TrainingRecordScope::Model; progress.model_id = model;
   progress.scalars.total = static_cast<double>(model);
   progress.distribution = r::TrainingDistributionFacts{.model_id = model, .unique_images = model, .scheduled_draws = model * 2, .repeated_class_exposure = model * 3};
   if (ema) {
    progress.val.emplace(); progress.val->bbox.ap = static_cast<double>(model) / 100;
    progress.artifact.emplace(); progress.artifact->session_id = run.run_id;
    progress.artifact->model_id = model; progress.artifact->path = "ema-" + std::to_string(model) + ".pt";
    progress.artifact->weights = r::EvaluatedWeights::Ema;
   }
   held.writer->Submit(progress, r::TrainingRecordRole::Epoch);
  }
  if (!ema) synchronized();
  progress = {}; progress.session_id = run.run_id;
  progress.phase = r::TrainingPhase::EpochComplete; progress.scope = r::TrainingRecordScope::Session;
  held.writer->Submit(progress, r::TrainingRecordRole::Epoch);
  if (overflow) held.writer->Submit(progress, r::TrainingRecordRole::Epoch);
  progress.phase = r::TrainingPhase::Completed;
  held.writer->Finish(progress, {.history_size = ema ? r::kMaximumTrainingModels + 1 : 1});
  held.Drain();
  std::istringstream history(held.history);
  std::string line;
  std::uint64_t count = 0, evaluated_count = 0, models = 0, synchronized_count = 0;
  while (std::getline(history, line)) {
   const auto record = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingRecord>(line, {.max_bytes = r::kTrainingRecordBytes, .max_items = 8192, .max_depth = 32});
   CHECK(record.sequence == count + (overflow && count == 18 ? 1 : 0));
   if (record.progress.scope == r::TrainingRecordScope::SynchronizedSession) {
    ++synchronized_count; REQUIRE(record.progress.val); REQUIRE(record.progress.artifact);
    CHECK(record.evaluated_weights == r::EvaluatedWeights::Ordinary);
   } else if (record.progress.scope == r::TrainingRecordScope::Model) {
    ++models;
    CHECK(record.progress.model_id == models);
    CHECK(record.progress.scalars.total == static_cast<double>(models));
    REQUIRE(record.progress.distribution); CHECK(record.progress.distribution->model_id == models);
    CHECK(record.progress.distribution->scheduled_draws == models * 2);
    CHECK(record.progress.val.has_value() == ema); CHECK(record.progress.artifact.has_value() == ema);
   }
   evaluated_count += record.progress.val.has_value();
   CHECK(record.role == (count == 18 ? r::TrainingRecordRole::Terminal : r::TrainingRecordRole::Epoch));
   ++count;
  }
  CHECK(count == 19); CHECK(models == r::kMaximumTrainingModels); CHECK(synchronized_count == 1);
  CHECK(evaluated_count == (ema ? r::kMaximumTrainingModels + 1 : 1));
  std::ifstream input(temp.path() / "results.json");
  const auto document = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingProgressDocument>(nlohmann::json::parse(input).dump(), {.max_bytes = r::kTrainingProgressDocumentBytes, .max_items = 131072, .max_depth = 32});
  CHECK(document.sources.distributions.size() == r::kMaximumTrainingModels);
  for (std::size_t index = 0; index < document.sources.distributions.size(); ++index) {
   const auto& distribution = document.sources.distributions[index];
   CHECK(distribution.model_id == index + 1);
   CHECK(distribution.unique_images == index + 1);
   CHECK(distribution.scheduled_draws == (index + 1) * 2);
   CHECK(distribution.repeated_class_exposure == (index + 1) * 3);
  }
  CHECK(document.sources.observations.size() == evaluated_count);
  REQUIRE(document.final); CHECK(document.final->history_size == evaluated_count);
  CHECK(document.record.sequence == (overflow ? 19 : 18));
  CHECK(document.record.dropped_before == (overflow ? 1 : 0));
  CHECK_NOTHROW(r::validate_training_progress_document(document, r::training_source_catalog(run.configuration)));
  CHECK(held.writer->persistence().degraded == overflow);
  CHECK(held.writer->persistence().dropped_records == (overflow ? 1 : 0));
 }
}

TEST_CASE("live projection retains every scheduled source beside current progress", "[model][rfdetr][training][telemetry]") {
 for (const bool ema : {false, true}) {
  mmltk::testsupport::ScopedTempDir temp{"mmltk-telemetry-observation"};
  r::TrainingRun run;
  run.configuration.use_ema = ema;
  run.evaluated_weights = ema ? r::EvaluatedWeights::Ema : r::EvaluatedWeights::Ordinary;
  run.configuration.lane_configuration.mode = r::TrainLaneMode::PeriodicAveraging;
  r::resize_training_models(run.configuration.lane_configuration, 2, run.configuration.recipe, 42);
  HeldHistory held(temp.path(), run);
  r::TrainingMetricProgress observed;
  observed.phase = r::TrainingPhase::EpochComplete;
  observed.scope = r::TrainingRecordScope::SynchronizedSession;
  observed.artifact.emplace(); observed.artifact->path = "ordinary.pt";
  observed.artifact->model_id = 1;
  observed.val.emplace(); observed.val->bbox.ap = .2;
  held.writer->Submit(observed, r::TrainingRecordRole::Epoch);
  if (ema) {
   observed.scope = r::TrainingRecordScope::Model; observed.model_id = 1;
   observed.artifact->weights = r::EvaluatedWeights::Ema; observed.artifact->path = "ema-1.pt";
   observed.val->bbox.ap = .4;
   held.writer->Submit(observed, r::TrainingRecordRole::Epoch);
   auto other = observed; other.model_id = 2; other.artifact->model_id = 2;
   other.artifact->path = "ema-2.pt"; other.val->bbox.ap = .8;
   held.writer->Submit(other, r::TrainingRecordRole::Epoch);
  }
  r::TrainingMetricProgress current;
  current.phase = r::TrainingPhase::EpochComplete; current.scope = r::TrainingRecordScope::Session;
  current.full_checkpoint_path = "session.json";
  held.writer->Submit(current, r::TrainingRecordRole::Epoch);
  current.phase = r::TrainingPhase::Train; current.scope = r::TrainingRecordScope::Model;
  current.epoch = 1; current.model_id = 2; current.completed_images = 3; current.total_images = 9;
  held.writer->Submit(current, r::TrainingRecordRole::Live);
  held.Drain();
  std::ifstream input(temp.path() / "progress.json");
  const auto document = mmltk::frameworks::serialization::decode_reflected_json<r::TrainingProgressDocument>(nlohmann::json::parse(input).dump(), {.max_bytes = r::kTrainingProgressDocumentBytes, .max_items = 24576, .max_depth = 32});
  CHECK(document.record.progress == current);
  REQUIRE(document.sources.observations.size() == (ema ? 3 : 1));
  const auto selected = std::ranges::find_if(document.sources.observations, [&](const auto& record) { return r::training_metric_source(record) == *document.sources.catalog.default_source; });
  REQUIRE(selected != document.sources.observations.end());
  CHECK(selected->progress == observed);
  CHECK(selected->sequence == (ema ? 1 : 0));
  CHECK(selected->role == r::TrainingRecordRole::Epoch);
  CHECK(selected->evaluated_weights == run.evaluated_weights);
  CHECK_NOTHROW(r::validate_training_sources(document.sources, document.record));
  if (ema) CHECK(document.sources.observations.back().progress.val->bbox.ap == .8);
  CHECK_FALSE(held.writer->persistence().degraded);
 }
}

TEST_CASE("Retained selectable sources reject foreign duplicate inconsistent and future observations", "[model][rfdetr][training][telemetry]") {
 r::TrainRequest request; request.use_ema = true; request.lanes = 16;
 request.lane_configuration.mode = r::TrainLaneMode::PeriodicAveraging;
 r::resize_training_models(request.lane_configuration, r::kMaximumTrainingModels, request.recipe, request.seed);
 r::TrainingSources sources; sources.catalog = r::training_source_catalog(request);
 REQUIRE(sources.catalog.available.size() == r::kMaximumTrainingModels + 1);
 r::TrainingRecord current; current.run_id = "run"; current.attempt_id = "attempt";
 current.progress.session_id = "session"; current.progress.epoch = 4;
 current.role = r::TrainingRecordRole::Epoch; current.progress.phase = r::TrainingPhase::EpochComplete;
 for (const auto& source : sources.catalog.available) {
  ++current.sequence;
  current.progress.scope = source.scope; current.progress.model_id = source.model_id;
  current.evaluated_weights = source.weights;
  current.progress.artifact.emplace(); auto& artifact = *current.progress.artifact;
  artifact.session_id = "session"; artifact.model_id = source.model_id; artifact.epoch = 4; artifact.weights = source.weights;
  artifact.path = "model-" + std::to_string(source.model_id) + ".pt";
  current.progress.val.emplace(); current.progress.val->bbox.ap = static_cast<double>(current.sequence) / 100;
  r::retain_training_source(sources, current);
 }
 CHECK(sources.observations.size() == sources.catalog.available.size());
 CHECK_NOTHROW(r::validate_training_sources(sources, current));
 const auto retained = sources.observations;
 current.sequence += 50; current.dropped_before = 9; current.role = r::TrainingRecordRole::Live;
 current.progress.phase = r::TrainingPhase::Train; current.progress.artifact.reset(); current.progress.val.reset();
 r::retain_training_source(sources, current);
 CHECK(sources.observations == retained);
 CHECK(sources.observations.back().sequence < current.sequence);
 for (int fault = 0; fault != 9; ++fault) {
  auto invalid = sources;
  auto& record = invalid.observations.front();
  switch (fault) {
   case 0: record.run_id = "foreign"; break;
   case 1: record.attempt_id = "foreign"; break;
   case 2: record.sequence = current.sequence + 1; break;
   case 3: record.progress.artifact->session_id = "foreign"; break;
   case 4: invalid.observations.push_back(record); break;
   case 5: record.progress.artifact->weights = r::EvaluatedWeights::Ema; break;
   case 6: record.attempt_configuration.emplace(); break;
   case 7: record.progress.model_id = 999; break;
   case 8: record.progress.artifact->evaluation.emplace(); record.progress.artifact->evaluation->bbox.ap = .99; break;
  }
  CHECK_THROWS(r::validate_training_sources(invalid, current));
 }
 CHECK_THROWS(r::retain_training_source(sources, retained.front()));
 r::TrainingProgressDocument document;
 document.record = current;
 document.sources = sources;
 document.record.run_id = document.record.progress.session_id;
 for (auto& observation : document.sources.observations) observation.run_id = document.record.run_id;
 CHECK_NOTHROW(r::validate_training_progress_document(document, sources.catalog));
 document.record.run_id = "foreign";
 for (auto& observation : document.sources.observations) observation.run_id = document.record.run_id;
 CHECK_NOTHROW(r::validate_training_sources(document.sources, document.record));
 CHECK_THROWS(r::validate_training_progress_document(document, sources.catalog));
 document.record.run_id = document.record.progress.session_id;
 for (auto& observation : document.sources.observations) observation.run_id = document.record.run_id;
 document.record.role = r::TrainingRecordRole::Terminal;
 document.record.progress.phase = r::TrainingPhase::Error;
 document.record.progress.epoch = 0;
 CHECK_NOTHROW(r::validate_training_progress_document(document, sources.catalog));
 CHECK(document.sources.observations.front().progress.epoch == 4);
 CHECK(document.record.sequence == current.sequence);
 CHECK(document.record.dropped_before == 9);
 document.final.emplace();
 CHECK_THROWS(r::validate_training_progress_document(document, sources.catalog));
 // Selected terminal progress and its validated selection describe one exact
 // evaluation, including the complete summary rather than only its AP scalar.
 r::TrainingSelection selection;
 selection.artifact.session_id = document.record.run_id;
 selection.artifact.model_id = 1;
 selection.artifact.path = "selected.pt";
 selection.artifact.initialization = selection.artifact.configuration = selection.artifact.content = selection.artifact.sha256 = selection.artifact.validation = std::string(64, 'a');
 selection.validation.bbox.ap = .5; selection.validation.bbox.available = true;
 selection.artifact.evaluation = selection.validation; selection.artifact.selection_metric = .5;
 selection.best_individual_metric = .5; selection.ingredients.push_back({1, std::string(64, 'a'), 1});
 document.sources.selected = selection; document.final->selected = selection;
 document.record.progress.phase = r::TrainingPhase::Completed;
 document.record.progress.scope = r::TrainingRecordScope::SelectedOutput;
 document.record.progress.model_id = selection.artifact.model_id;
 document.record.evaluated_weights = selection.artifact.weights;
 document.record.progress.artifact = selection.artifact;
 document.record.progress.val = selection.validation;
 CHECK_NOTHROW(r::validate_training_progress_document(document, sources.catalog));
 document.record.progress.val.reset();
 CHECK_THROWS(r::validate_training_progress_document(document, sources.catalog));
 document.record.progress.val = selection.validation;
 document.record.progress.val->bbox.ap50 = .75;
 CHECK_THROWS(r::validate_training_progress_document(document, sources.catalog));
}
