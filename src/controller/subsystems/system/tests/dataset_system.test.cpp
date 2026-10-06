#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/settings/settings_system.h"
#include "src/common/types/cancellation_observation.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/backend/data/tests/benchmark_http_fixture.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/common/io/file_digest.h"
#include <vector>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
class UnusedWeightOperations final : public services::ArtifactWeightOperations {
public:
 [[nodiscard]] std::optional<services::ArtifactWeightAsset> find(std::string_view) const override { return std::nullopt; }
 void download(std::string_view, const std::filesystem::path&, const services::ArtifactCancellationToken&, services::ArtifactWeightProgressObserver) const override {}
};
class DiagnosticCompiler final : public services::ArtifactCompilerOperations {
public:
 mutable mmltk::backend::data::BenchmarkDatasetSelection selection;
 mutable std::filesystem::path physical_output;
 mutable std::filesystem::path publication_output;
 mutable bool observer_enabled = false;
 mutable bool observer_context = false;
 std::function<void(mmltk::common::concurrency::CancellationObservation)> work;

private:
 void compile_directory(const std::filesystem::path&, const std::filesystem::path&, std::uint32_t, bool, mmltk::backend::imaging::resample::ImageResizeMode,
  mmltk::backend::imaging::resample::RgbResizeMethod, mmltk::common::concurrency::CancellationObservation, services::ArtifactProgressObserver) const override {}
 void compile_benchmark(mmltk::backend::data::BenchmarkDatasetSelection selected, const std::filesystem::path& output, const std::filesystem::path& publication, std::uint32_t, bool,
  mmltk::backend::imaging::resample::ImageResizeMode, mmltk::backend::imaging::resample::RgbResizeMethod, mmltk::common::concurrency::CancellationObservation cancellation,
  services::ArtifactProgressObserver, services::ArtifactBenchmarkTraceObserver trace) const override {
  selection = selected;
  physical_output = output;
  publication_output = publication;
  observer_enabled = trace.report != nullptr;
  observer_context = trace.context != nullptr;
  trace("benchmark.direct", R"({"records":1})");
  if (work) work(cancellation);
 }
};
void compile_staged_fixture(const std::filesystem::path& output, mmltk::common::concurrency::CancellationObservation cancellation) {
 const mmltk::backend::data::testsupport::FixtureSpec fixture{.root_dir = (output.parent_path() / "source").string(), .width = 32, .height = 32, .num_images = 1};
 mmltk::backend::data::testsupport::create_synthetic_dataset(fixture);
 auto config = mmltk::backend::data::testsupport::compiler_config(fixture);
 config.output_dir = output.string();
 config.num_workers = 1;
 const auto plan = mmltk::backend::data::DatasetCompiler::prepare(std::move(config), {fixture.split}, cancellation);
 mmltk::backend::data::DatasetCompiler::compile(plan, 0, nullptr, cancellation);
}
std::vector<nlohmann::json> artifact_failures(const std::filesystem::path& log) {
 std::ifstream input{log};
 std::vector<nlohmann::json> failures;
 for (std::string line; std::getline(input, line);) {
  auto record = nlohmann::json::parse(line);
  if (record.at("name") == "artifact.compile.failed") failures.push_back(std::move(record.at("fields")));
 }
 return failures;
}
TEST_CASE("artifact dataset runtime owns its diagnostic target and borrows only during compile", "[controller][systems][dataset][diagnostics]") {
 const mmltk::testsupport::ScopedTempDir root{"ordinary-dataset-diagnostics"};
 UnusedWeightOperations weights;
 DiagnosticCompiler compiler;
 const auto log = root.path() / "trace.jsonl";
 services::DiagnosticsClient client{mmltk::common::io::ScopedFd{::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)}, services::DiagnosticsExecutionPolicy::CallerDriven};
 REQUIRE(client.enabled());
 services::RuntimeDiagnosticTarget target;
 {
  services::RuntimeDiagnostics creator{client.producer()};
  target = creator.target();
 }
 services::ArtifactCompileRequest request{
  .kind = services::ArtifactCompileKind::Benchmark,
  .source = {},
  .output = root.path() / "output",
  .preset = "rf-detr-nano",
  .resolution = 32U,
  .overwrite = false,
  .benchmark_selection = {mmltk::backend::data::BenchmarkDatasetVariant::Coconut, mmltk::backend::data::CoconutValidation::Stock}
 };
 bool enabled = true;
 std::string_view operation = "staged_inspection";
 SECTION("compilation retains the original error") {
  operation = "compilation";
  compiler.work = [](auto) { throw std::runtime_error("compiler original failure"); };
 }
 SECTION("staged inspection rejects corrupt output") {
  compiler.work = [&](auto) { mmltk::testsupport::write_text_file(compiler.physical_output / "train.bin", "incomplete compiled output"); };
 }
 SECTION("final publication refuses a concurrently occupied output") {
  operation = "final_publication";
  compiler.work = [&](auto cancellation) {
   compile_staged_fixture(compiler.physical_output, cancellation);
   mmltk::testsupport::write_text_file(request.output / "concurrent", "preserved");
  };
 }
 SECTION("target copy survives creator and caller copy destruction") {}
 SECTION("absent target installs no observer") {
  target = {};
  enabled = false;
 }
 SECTION("closed producer installs no observer") {
  client.close();
  enabled = false;
 }
 SECTION("destroyed producer installs no observer") {
  services::DiagnosticsClient temporary{
   mmltk::common::io::ScopedFd{::open((root.path() / "closed.jsonl").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)}, services::DiagnosticsExecutionPolicy::CallerDriven
  };
  services::RuntimeDiagnostics creator{temporary.producer()};
  target = creator.target();
  enabled = false;
 }
 std::string original_error;
 {
  ArtifactDatasetRuntime runtime{services::ArtifactStore{root.path() / "cache", weights, compiler}, target};
  target = {};
  const auto result = runtime.Compile(request, {}, {});
  CHECK_FALSE(result.cancelled);
  CHECK_FALSE(result.inspection.available());
  original_error = result.inspection.detail;
  CHECK_FALSE(original_error.empty());
  if (operation == "compilation") CHECK(original_error == "compiler original failure");
  if (operation == "final_publication") {
   CHECK(original_error == "publication destination already exists: " + request.output.string());
   CHECK(std::filesystem::exists(request.output / "concurrent"));
  }
  CHECK(compiler.observer_enabled == enabled);
  CHECK(compiler.observer_context == enabled);
  CHECK(compiler.publication_output == request.output);
  CHECK(compiler.selection == request.benchmark_selection);
  CHECK(compiler.physical_output != request.output);
  CHECK(compiler.physical_output.parent_path() == request.output.parent_path());
  CHECK_FALSE(std::filesystem::exists(compiler.physical_output));
 }
 CHECK(client.counters().accepted == (enabled ? 2U : 0U));
 client.close(services::DiagnosticsCloseMode::Flush);
 std::ifstream input{log};
 const std::string records{std::istreambuf_iterator<char>{input}, {}};
 CHECK(records.contains("benchmark.direct") == enabled);
 const auto failures = artifact_failures(log);
 REQUIRE(failures.size() == (enabled ? 1U : 0U));
 if (enabled) {
  CHECK(failures.front().at("operation") == operation);
  CHECK(failures.front().at("staged_path") == compiler.physical_output.string());
  CHECK(failures.front().at("final_path") == request.output.string());
  CHECK(failures.front().at("error") == original_error);
 }
}
TEST_CASE("owned dataset diagnostics survive runtime failure reconstruction and stop join", "[controller][systems][dataset][diagnostics]") {
 const mmltk::testsupport::ScopedTempDir root{"dataset-diagnostic-reconstruction"};
 const auto output = root.path() / "compiled";
 SettingsSystem settings;
 const services::SettingsLocation location{(root.path() / "settings.json").string()};
 auto stored = contracts::default_gui_settings_state();
 stored.workflows.train.compile_benchmark_dataset_override = true;
 stored.workflows.train.compiled_dataset_dir = output.string();
 stored.workflows.train.request.resolution = 32;
 stored.workflows.train.request.preset_name = "rf-detr-nano";
 REQUIRE(services::SettingsStore::save(location.value(), stored, 1U).succeeded());
 REQUIRE(settings.Load(location).applied());
 services::DiagnosticsClient client{
  mmltk::common::io::ScopedFd{::open((root.path() / "trace.jsonl").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)}, services::DiagnosticsExecutionPolicy::CallerDriven
 };
 REQUIRE(client.enabled());
 services::RuntimeDiagnosticTarget target;
 {
  services::RuntimeDiagnostics creator{client.producer()};
  target = creator.target();
 }
 UnusedWeightOperations weights;
 DiagnosticCompiler compiler;
 std::size_t constructions = 0U;
 TerminalSequence<DatasetSystem::event_type> terminals;
 mmltk::testsupport::TestGate gate{"dataset compiler"};
 compiler.work = [&](auto) {
  mmltk::testsupport::write_text_file(compiler.physical_output / "train.bin", "unfinished candidate");
  throw std::runtime_error("diagnostic compiler failure");
 };
 DatasetSystem dataset{settings, [&, target] {
  ++constructions;
  return std::make_unique<ArtifactDatasetRuntime>(services::ArtifactStore{root.path() / "cache", weights, compiler}, target);
 }, [&](DatasetSystem::event_type event) {
  if (std::holds_alternative<DatasetChanged>(event)) terminals.Publish(std::move(event));
 }};
 mmltk::testsupport::ScopedTestCleanup cleanup{[&] {
  gate.Release();
  dataset.Shutdown();
 }};
 target = {};
 static_cast<void>(dataset.Compile({}));
 auto first = terminals.First();
 const auto failed = mmltk::testsupport::await_test_future(first, "failed compile");
 CHECK(std::get<DatasetChanged>(failed).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
 CHECK(std::get<DatasetChanged>(failed).snapshot.terminal.detail == "diagnostic compiler failure");
 CHECK(compiler.observer_enabled);
 const auto failed_staging = compiler.physical_output;
 CHECK_FALSE(std::filesystem::exists(failed_staging));
 CHECK_FALSE(std::filesystem::exists(output));
 compiler.work = [receipt = gate.receipt()](auto) { receipt.ArriveAndWait(); };
 bool cancel = true;
 SECTION("retry can stop while its reconstructed compiler is active") {}
 SECTION("retry publishes and inspects new output after the failed output remained absent") {
  cancel = false;
  compiler.work = [&](auto cancellation) {
   if (compiler.physical_output == failed_staging || !std::filesystem::is_empty(compiler.physical_output) || std::filesystem::exists(output))
    throw std::runtime_error("retry did not receive fresh staging beside absent output");
   compile_staged_fixture(compiler.physical_output, cancellation);
  };
 }
 static_cast<void>(dataset.Compile({}));
 if (cancel) {
  REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
  static_cast<void>(dataset.Stop());
  gate.Release();
 }
 auto second = terminals.Second();
 const auto retried = std::get<DatasetChanged>(mmltk::testsupport::await_test_future(second, "retried compile")).snapshot;
 CAPTURE(retried.terminal.detail);
 CHECK(retried.terminal.outcome == (cancel ? contracts::ArtifactTerminalOutcome::Cancelled : contracts::ArtifactTerminalOutcome::Succeeded));
 CHECK(std::filesystem::exists(output / "train.bin") == !cancel);
 CHECK(retried.inspection.available() == !cancel);
 CHECK(compiler.physical_output != failed_staging);
 if (!cancel) {
  CHECK(retried.terminal.artifact == output.string());
  REQUIRE(retried.inspection.splits.size() == 1U);
  const auto& split = retried.inspection.splits.front();
  CHECK(split.path == (output / "train.bin").string());
  CHECK(split.image_count == 1U);
  CHECK(split.width == 32U);
  CHECK(split.height == 32U);
 }
 dataset.Shutdown();
 CHECK(constructions == 2U);
 CHECK(compiler.observer_enabled);
 CHECK_FALSE(std::filesystem::exists(compiler.physical_output));
 CHECK(client.counters().accepted == 3U);
 client.close(services::DiagnosticsCloseMode::Flush);
 const auto failures = artifact_failures(root.path() / "trace.jsonl");
 REQUIRE(failures.size() == 1U);
 CHECK(failures.front().at("operation") == "compilation");
 CHECK(failures.front().at("staged_path") == failed_staging.string());
 CHECK(failures.front().at("final_path") == output.string());
 CHECK(failures.front().at("error") == "diagnostic compiler failure");
}
TEST_CASE("dataset publishes direct progress, returns Busy, stops locally, and reconstructs after failure", "[controller][systems][dataset]") {
 const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 std::atomic_size_t constructions = 0U;
 TerminalSequence<DatasetSystem::event_type> terminals;
 std::atomic_size_t progress = 0U;
 DatasetSystem dataset{settings, [&] {
  const bool fail = constructions++ == 0U;
  return std::make_unique<FakeDatasetRuntime>(gate, fail);
 }, [&](DatasetSystem::event_type event) {
  if (std::holds_alternative<DatasetProgress>(event))
   ++progress;
  else
   terminals.Publish(std::move(event));
 }};
 const auto admitted = dataset.Compile({});
 CHECK(admitted.revision > 0);
 CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
 // CLEANUP-IGNORE: Compile and Inspect are separate dataset admission endpoints sharing one system-owned runtime.
 CHECK_THROWS_AS(dataset.Inspect({}, "rf-detr-base", 560U), contracts::BusyError);
 gate->Release();
 const auto failed_dataset = terminals.First().get();
 REQUIRE(std::holds_alternative<DatasetChanged>(failed_dataset));
 CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.generation == 1U);
 CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
 CHECK(progress == 0U);
 CHECK_FALSE(dataset.snapshot().active);
 const auto failed_revision = dataset.snapshot().revision;
 CHECK(failed_revision > admitted.revision);
 gate = std::make_shared<mmltk::testsupport::StopGate>();
 const auto restarted = dataset.Compile({});
 CHECK(restarted.revision > failed_revision);
 const auto stopping = dataset.Stop();
 CHECK(stopping.revision > restarted.revision);
 CHECK(std::holds_alternative<DatasetChanged>(terminals.Second().get()));
 CHECK(dataset.snapshot().terminal.outcome == contracts::ArtifactTerminalOutcome::Cancelled);
 CHECK(dataset.snapshot().revision >= stopping.revision);
 gate->Release();
 static_cast<void>(dataset.Compile({}));
 CHECK(std::holds_alternative<DatasetChanged>(terminals.Third().get()));
 CHECK(dataset.snapshot().terminal.outcome == contracts::ArtifactTerminalOutcome::Succeeded);
 CHECK(constructions == 2U);
}
TEST_CASE("dataset rejects Compile and a second Inspect while Inspect owns the runtime", "[controller][systems][dataset][admission]") {
 const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset-inspect-admission");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 auto observation = std::make_shared<DatasetRuntimeObservation>();
 std::atomic_int constructions = 0;
 std::promise<DatasetSystem::event_type> compile_done;
 DatasetSystem dataset{settings, [&] {
  ++constructions;
  return std::make_unique<BlockingInspectRuntime>(observation);
 }, [&](DatasetSystem::event_type event) {
  if (!std::holds_alternative<DatasetProgress>(event)) compile_done.set_value(std::move(event));
 }};
 const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity> paths{root / "train.bin", {}, {}};
 auto inspecting = std::async(std::launch::async, [&] { return dataset.Inspect(paths, "rf-detr-base", 560U); });
 observation->inspect_started.get_future().wait();
 CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
 CHECK_THROWS_AS(dataset.Inspect(paths, "rf-detr-base", 560U), contracts::BusyError);
 CHECK(constructions == 1);
 CHECK(observation->compile_calls == 0);
 CHECK(observation->maximum_active_calls == 1);
 observation->inspect_gate->Release();
 CHECK(inspecting.get().available());
 static_cast<void>(dataset.Compile({}));
 CHECK(std::holds_alternative<DatasetChanged>(compile_done.get_future().get()));
 CHECK(observation->compile_calls == 1);
 CHECK(observation->maximum_active_calls == 1);
}
TEST_CASE("compilation captures its perceptual selection independently of augmentation", "[controller][systems][dataset][perceptual]") {
 namespace contracts = mmltk::controller::contracts;
 namespace services = mmltk::controller::services;
 contracts::GuiSettingsState settings;
 using ResizeMode = mmltk::backend::imaging::resample::ImageResizeMode;
 CHECK(settings.workflows.train.compile_resize_mode == ResizeMode::Stretch);
 settings.workflows.train.compile_resize_mode = ResizeMode::Letterbox;
 settings.workflows.train.compile_perceptual_downscale = true;
 settings.workflows.train.compile_resize_method = mmltk::backend::imaging::resample::RgbResizeMethod::TorchBilinearAntialias;
 settings.workflows.train.request.gpu_augmentation.enabled = false;
 settings.workflows.train.request.gpu_augmentation.perceptual_downscale = false;
 const auto captured = services::materialize_artifact_compile(settings);
 REQUIRE(captured);
 CHECK(captured->perceptual_downscale);
 CHECK(captured->resize_method == mmltk::backend::imaging::resample::RgbResizeMethod::TorchBilinearAntialias);
 CHECK(captured->resize_mode == ResizeMode::Letterbox);
 settings.workflows.train.compile_perceptual_downscale = false;
 settings.workflows.train.compile_resize_method = mmltk::backend::imaging::resample::RgbResizeMethod::Native;
 settings.workflows.train.compile_resize_mode = ResizeMode::Stretch;
 settings.workflows.train.request.gpu_augmentation.perceptual_downscale = true;
 const auto next = services::materialize_artifact_compile(settings);
 REQUIRE(next);
 CHECK_FALSE(next->perceptual_downscale);
 CHECK(next->resize_method == mmltk::backend::imaging::resample::RgbResizeMethod::Native);
 CHECK(next->resize_mode == ResizeMode::Stretch);
 CHECK(captured->perceptual_downscale);
 CHECK(captured->resize_method == mmltk::backend::imaging::resample::RgbResizeMethod::TorchBilinearAntialias);
 CHECK(captured->resize_mode == ResizeMode::Letterbox);
}
class AcquisitionDatasetRuntime final : public DatasetRuntime {
public:
 using Observer = std::function<void(const contracts::ArtifactProgress&)>;
 using Work = std::function<void(const Observer&)>;
 explicit AcquisitionDatasetRuntime(Work work) : work_(std::move(work)) {}
 services::ArtifactCompileResult Compile(const services::ArtifactCompileRequest& request, std::stop_token, const Observer& progress) override {
  work_(progress);
  return {.output = request.output, .inspection = successful_inspection({request.output / "train.bin", {}, {}})};
 }
 contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, std::string_view, std::uint32_t, std::stop_token) override {
  return successful_inspection(paths);
 }

private:
 Work work_;
};
TEST_CASE("dataset delivers mixed Coconut row extents and continuing pixels through successful publication", "[controller][systems][dataset][progress]") {
 namespace data = mmltk::backend::data;
 using namespace data::benchmark_internal;
 const mmltk::testsupport::ScopedTempDir root{"dataset-mixed-annotation-extents"};
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 std::vector<contracts::ArtifactProgress> produced, delivered;
 std::promise<DatasetChanged> terminal;
 auto completed = terminal.get_future();
 const AcquisitionDatasetRuntime::Work work = [&](const auto& progress) {
  ProgressReporter reporter([&](const auto& snapshot) {
   auto projected = services::project_artifact_progress(snapshot);
   produced.push_back(projected);
   progress(projected);
  }, {});
  reporter.phase(data::DatasetCompilePhase::Planning);
  auto& rows = data::testsupport::mixed_coconut_row_progress(reporter);
  for (std::size_t release = 1; release < 3; ++release) {
   rows.update(release, 128, reporter);
   reporter.pixel_completed();
   // The ordinary COCO cache concurrently completes one original-index job.
   reporter.phase(data::DatasetCompilePhase::Indexing, release, 2);
   reporter.flush();
  }
  rows.settle(0, 241602, reporter);
  rows.settle(1, 128, reporter);
  rows.settle(2, 128, reporter);
  for (std::size_t i = 0; i < 3; ++i) reporter.label_plan_completed(i);
  reporter.acquisition_complete();
  reporter.pixels(16179, 16179);
  reporter.phase(data::DatasetCompilePhase::Publishing, 1, 1);
  reporter.flush();
 };
 DatasetSystem dataset{settings, [&] { return std::make_unique<AcquisitionDatasetRuntime>(work); }, [&](DatasetSystem::event_type event) {
  if (const auto* progress = std::get_if<DatasetProgress>(&event))
   delivered.push_back(progress->snapshot.progress);
  else
   terminal.set_value(std::get<DatasetChanged>(std::move(event)));
 }};
 const mmltk::testsupport::ScopedTestCleanup settle([&] { dataset.Shutdown(); });
 static_cast<void>(dataset.Compile({}));
 const auto result = mmltk::testsupport::await_test_future(completed, "mixed row extents publish through DatasetSystem");
 dataset.Shutdown();
 CHECK(result.snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Succeeded);
 CHECK(result.snapshot.terminal.detail.empty());
 CHECK_FALSE(result.snapshot.active);
 produced.erase(std::unique(produced.begin(), produced.end()), produced.end());
 CHECK(delivered == produced);
 REQUIRE_FALSE(delivered.empty());
 for (const auto& snapshot : delivered) CHECK(snapshot.valid());
 for (std::uint64_t pixels = 16177; pixels <= 16179; ++pixels)
  CHECK(std::ranges::any_of(delivered, [&](const auto& snapshot) { return snapshot.tracks.pixels.completed == pixels && !snapshot.tracks.labels.total_known; }));
 CHECK(std::ranges::any_of(delivered, [](const auto& snapshot) { return snapshot.tracks.labels.completed > 241605 && !snapshot.tracks.labels.total_known; }));
 CHECK(delivered.back().tracks.labels.completed == 241858);
 CHECK(delivered.back().tracks.labels.complete);
}
TEST_CASE("dataset admits real open ended acquisition and successful HTTP recovery", "[controller][systems][dataset][progress]") {
 namespace data = mmltk::backend::data;
 using namespace data::benchmark_internal;
 using Source = data::BenchmarkDatasetSource;
 const mmltk::testsupport::ScopedTempDir root{"dataset-acquisition-admission"};
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 bool mixed = false;
 bool retry = false;
 bool redirect = false;
 auto source = Source::kCoco2017;
 SECTION("unknown only metadata") {}
 SECTION("mixed metadata within one source") { mixed = true; }
 SECTION("mixed metadata across sources") {
  mixed = true;
  source = Source::kObjects365V2;
 }
 SECTION("nonempty failed response followed by success") { retry = true; }
 SECTION("discarded redirect followed by success") { redirect = true; }
 const bool unknown = !retry && !redirect;
 const std::vector<std::uint8_t> payload(unknown ? 1024U * 1024U : 4096U, 29U);
 data::testsupport::HttpServer server(payload);
 if (unknown) {
  server.OmitContentLength();
  server.GateNextTransfer();
 }
 if (retry) server.fail_next(1, 8192U);
 if (redirect) server.RedirectNextTransfer();
 const DownloadRequest request{"metadata", server.url("metadata"), root.path() / "metadata.bin", root.path() / "metadata.lock", unknown ? 0U : payload.size(), {}, 2U, false, source};
 const DownloadRequest known{"known", server.url("known"), root.path() / "known.bin", root.path() / "known.lock", payload.size(), {}, 1U};
 if (mixed) { mmltk::testsupport::write_binary_file(known.destination, payload); }
 std::vector<contracts::ArtifactProgress> delivered;
 std::vector<contracts::ArtifactProgress> produced;
 std::vector<DownloadProgress> transfers;
 std::promise<void> open_ended;
 auto observing = open_ended.get_future();
 bool notified = false;
 std::promise<DatasetChanged> terminal;
 auto completed = terminal.get_future();
 const AcquisitionDatasetRuntime::Work work = [&](const auto& progress) {
  const BenchmarkTraceSink trace;
  ProgressReporter reporter([&](const auto& update) {
   const auto projected = services::project_artifact_progress(update);
   produced.push_back(projected);
   progress(projected);
  }, trace);
  ArtifactProgressTotals totals;
  reporter.phase(data::DatasetCompilePhase::Downloading);
  if (mixed) (void)download_artifacts({known}, 1U, {}, [&](const auto& update) { totals.update(update, reporter); });
  (void)download_artifacts({request}, 1U, {}, [&](const auto& update) {
   transfers.push_back(update);
   totals.update(update, reporter);
  });
  reporter.flush();  // Settle this runtime's callback captures before returning.
 };
 DatasetSystem dataset{settings, [&] { return std::make_unique<AcquisitionDatasetRuntime>(work); }, [&](DatasetSystem::event_type event) {
  if (const auto* progress = std::get_if<DatasetProgress>(&event)) {
   delivered.push_back(progress->snapshot.progress);
   if (!notified && !progress->snapshot.progress.tracks.acquisition.total_known && progress->snapshot.progress.tracks.acquisition.completed > (mixed ? payload.size() : 0U)) {
    notified = true;
    open_ended.set_value();
   }
  } else {
   terminal.set_value(std::get<DatasetChanged>(std::move(event)));
  }
 }};
 const mmltk::testsupport::ScopedTestCleanup settle([&] {
  server.ReleasePartial();
  dataset.Shutdown();
 });
 static_cast<void>(dataset.Compile({}));
 if (unknown) {
  REQUIRE(server.WaitPartial());
  mmltk::testsupport::await_test_future(observing, "admitted open-ended metadata progress");
  server.ReleasePartial();
 }
 const auto result = mmltk::testsupport::await_test_future(completed, "successful acquisition terminal");
 dataset.Shutdown();
 CHECK(result.snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Succeeded);
 CHECK(result.snapshot.terminal.detail.empty());
 CHECK_FALSE(result.snapshot.active);
 // Projection can produce identical adjacent snapshots; unchanged public state
 // has no new revision or event, while every distinct observation is retained.
 produced.erase(std::unique(produced.begin(), produced.end()), produced.end());
 CHECK(delivered == produced);
 REQUIRE_FALSE(delivered.empty());
 for (const auto& progress : delivered) {
  CHECK(progress.valid());
  CHECK(progress.activity.find(" bytes") == std::string::npos);
  CHECK(progress.activity.find(" · attempt ") == std::string::npos);
  CHECK(progress.activity.find(" · retained ") == std::string::npos);
  for (const auto& observed_source : progress.sources) {
   if (observed_source.activity.empty()) continue;
   REQUIRE(observed_source.transfer);
   CHECK(observed_source.transfer->valid());
   CHECK(observed_source.activity.find(" bytes") == std::string::npos);
  }
 }
 for (const auto& observed : delivered.back().sources)
  if (observed.source == source) {
   REQUIRE(observed.transfer);
   CHECK(observed.transfer->attempt == (retry ? 2U : 1U));
  }
 // Each delivered source transfer is an ordered observation of the real HTTP
 // producer; counter-only observations may coalesce before delivery.
 std::size_t next_transfer = 0;
 bool saw_first_attempt = false, saw_retry = false;
 for (const auto& progress : delivered) {
  const auto current = std::ranges::find(progress.sources, source, &data::BenchmarkSourceProgress::source);
  if (current == progress.sources.end() || !current->transfer || current->activity.find("metadata") == std::string::npos) continue;
  const auto found = std::find_if(transfers.begin() + next_transfer, transfers.end(), [&](const auto& value) { return value.transfer == *current->transfer; });
  REQUIRE(found != transfers.end());
  next_transfer = static_cast<std::size_t>(found - transfers.begin()) + 1;
  if (current->transfer->attempt == 1U) {
   CHECK_FALSE(saw_retry);
   saw_first_attempt = true;
  }
  if (current->transfer->attempt == 2U) {
   CHECK(saw_first_attempt);
   saw_retry = true;
  }
 }
 CHECK(saw_first_attempt);
 CHECK(saw_retry == retry);
 CHECK(delivered.back().tracks.acquisition.completed == payload.size() * (mixed ? 2U : 1U));
 CHECK(delivered.back().tracks.acquisition.total == delivered.back().tracks.acquisition.completed);
 REQUIRE_FALSE(transfers.empty());
 for (const auto& transfer : transfers) {
  CHECK(transfer.transfer.completed_bytes <= payload.size());
  if (retry && transfer.transfer.attempt == 1U) CHECK(transfer.transfer.completed_bytes == 0U);
 }
 CHECK(transfers.back().transfer.attempt == (retry ? 2U : 1U));
 CHECK(mmltk::common::io::sha256_file(request.destination) == mmltk::common::io::sha256_bytes(payload));
 CHECK(read_json_file(request.destination.string() + ".download.json").at("size") == payload.size());
 server.Check();
}
TEST_CASE("dataset independently rejects each malformed progress invariant", "[controller][systems][dataset][progress]") {
 const mmltk::testsupport::ScopedTempDir root{"dataset-progress-rejection"};
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 contracts::ArtifactProgress malformed{.phase = contracts::ArtifactCompilePhase::Downloading, .activity = "metadata", .completed = 1U, .total = 2U};
 SECTION("known total overrun") { malformed.completed = 3U; }
 SECTION("track known-zero overrun") {
  malformed.tracks.acquisition.total_known = true;
  malformed.tracks.acquisition.completed = 1;
 }
 SECTION("completed track remains active") {
  malformed.tracks.labels.active = true;
  malformed.tracks.labels.complete = true;
 }
 SECTION("invalid track unit") { malformed.tracks.labels.unit = static_cast<mmltk::backend::data::DatasetCompileUnit>(255); }
 SECTION("invalid track activity") { malformed.tracks.pixels.activity = static_cast<mmltk::backend::data::DatasetCompileActivity>(255); }
 SECTION("oversized source activity") { malformed.sources.push_back({.activity = std::string(contracts::kArtifactProgressTextCapacity + 1, 'x')}); }
 SECTION("artifact transfer exceeds known total") { malformed.sources.emplace_back().transfer = mmltk::backend::data::BenchmarkTransferProgress{.completed_bytes = 3, .total_bytes = 2}; }
 SECTION("artifact retained prefix exceeds completed bytes") { malformed.sources.emplace_back().transfer = mmltk::backend::data::BenchmarkTransferProgress{.completed_bytes = 2, .retained_bytes = 3}; }
 SECTION("invalid phase") { malformed.phase = static_cast<contracts::ArtifactCompilePhase>(255U); }
 SECTION("oversized activity") { malformed.activity.assign(contracts::kArtifactProgressTextCapacity + 1U, 'x'); }
 CHECK_FALSE(malformed.valid());
 std::promise<DatasetChanged> terminal;
 auto completion = terminal.get_future();
 std::size_t delivered = 0U;
 DatasetSystem dataset{settings, [&] { return std::make_unique<AcquisitionDatasetRuntime>([&](const auto& progress) { progress(malformed); }); }, [&](DatasetSystem::event_type event) {
  if (std::holds_alternative<DatasetProgress>(event))
   ++delivered;
  else
   terminal.set_value(std::get<DatasetChanged>(std::move(event)));
 }};
 static_cast<void>(dataset.Compile({}));
 const auto result = mmltk::testsupport::await_test_future(completion, "malformed progress rejection");
 dataset.Shutdown();
 CHECK(delivered == 0U);
 CHECK(result.snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
 CHECK(result.snapshot.terminal.detail == "dataset compiler returned invalid progress");
}
TEST_CASE("dataset compile observes the settled benchmark selection and retains it while settings change", "[controller][systems][dataset][benchmark]") {
 namespace data = mmltk::backend::data;
 using mmltk::frameworks::serialization::wire::FlatValue;
 const mmltk::testsupport::ScopedTempDir root{"dataset-selection-admission"};
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root.path())).applied());
 contracts::SettingsUpdateRequest edit;
 edit.updates = {
  {.path = "workflows.train.compile_benchmark_dataset_override", .value = FlatValue{true}}, {.path = "workflows.train.benchmark_selection.dataset", .value = *FlatValue::text("Coconut", 7U)},
  {.path = "workflows.train.benchmark_selection.validation", .value = *FlatValue::text("CoconutStock", 12U)},
  {.path = "workflows.train.benchmark_selection.recover_dropped_masks", .value = FlatValue{true}},
  {.path = "workflows.train.compiled_dataset_dir", .value = *FlatValue::text((root.path() / "output").string(), 4096U)}
 };
 const auto settled = settings.Update(std::move(edit));
 const data::BenchmarkDatasetSelection selected{data::BenchmarkDatasetVariant::Coconut, data::CoconutValidation::CoconutStock, true};
 REQUIRE(settled.settings_state.workflows.train.benchmark_selection == selected);
 UnusedWeightOperations weights;
 DiagnosticCompiler compiler;
 mmltk::testsupport::TestGate gate{"dataset selection"};
 compiler.work = [receipt = gate.receipt()](auto) { receipt.ArriveAndWait(); };
 DatasetSystem dataset{settings, [&] { return std::make_unique<ArtifactDatasetRuntime>(services::ArtifactStore{root.path() / "cache", weights, compiler}); }};
 mmltk::testsupport::ScopedTestCleanup cleanup{[&] {
  gate.Release();
  dataset.Shutdown();
 }};
 CHECK_FALSE(dataset.snapshot().active);
 static_cast<void>(dataset.Compile({}));
 REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
 CHECK(dataset.snapshot().active);
 CHECK(compiler.selection == selected);
 contracts::SettingsUpdateRequest later;
 later.updates = {
  {.path = "workflows.train.benchmark_selection.recover_dropped_masks", .value = FlatValue{false}}, {.path = "workflows.train.benchmark_selection.validation", .value = *FlatValue::text("Stock", 5U)},
  {.path = "workflows.train.compile_benchmark_dataset_override", .value = FlatValue{false}}
 };
 const auto changed = settings.Update(std::move(later));
 CHECK(changed.settings_state.workflows.train.benchmark_selection.validation == data::CoconutValidation::Stock);
 CHECK(compiler.selection == selected);
 CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
 static_cast<void>(dataset.Stop());
 gate.Release();
 dataset.Shutdown();
 CHECK(compiler.selection == selected);
}
TEST_CASE("configured artifact compiler receives every benchmark selection without changing staging paths", "[controller][systems][dataset][benchmark]") {
 namespace data = mmltk::backend::data;
 const mmltk::testsupport::ScopedTempDir root{"benchmark-compiler-selection"};
 UnusedWeightOperations weights;
 DiagnosticCompiler compiler;
 ArtifactDatasetRuntime runtime{services::ArtifactStore{root.path() / "cache", weights, compiler}};
 services::ArtifactCompileRequest request{.kind = services::ArtifactCompileKind::Benchmark, .source = {}, .output = root.path() / "output", .preset = "rf-detr-nano", .resolution = 384U};
 for (const auto dataset : {data::BenchmarkDatasetVariant::CocoCustom, data::BenchmarkDatasetVariant::Coconut}) {
  for (const auto validation : {data::CoconutValidation::Coconut, data::CoconutValidation::Stock, data::CoconutValidation::CoconutStock}) {
   request.benchmark_selection = {dataset, validation};
   static_cast<void>(runtime.Compile(request, {}, {}));
   CHECK(compiler.selection == request.benchmark_selection);
   CHECK(compiler.publication_output == request.output);
   CHECK(compiler.physical_output != request.output);
   CHECK_FALSE(std::filesystem::exists(compiler.physical_output));
  }
 }
}
TEST_CASE("benchmark materialization retains each admitted selection after later settings edits", "[controller][systems][dataset][benchmark]") {
 namespace data = mmltk::backend::data;
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.compile_benchmark_dataset_override = true;
 for (const auto dataset : {data::BenchmarkDatasetVariant::CocoCustom, data::BenchmarkDatasetVariant::Coconut}) {
  for (const auto validation : {data::CoconutValidation::Coconut, data::CoconutValidation::Stock, data::CoconutValidation::CoconutStock}) {
   const data::BenchmarkDatasetSelection selected{dataset, validation};
   settings.workflows.train.benchmark_selection = selected;
   const auto captured = services::materialize_artifact_compile(settings);
   REQUIRE(captured);
   CHECK(captured->benchmark_selection == selected);
   settings.workflows.train.benchmark_selection = {};
   settings.workflows.train.compile_benchmark_dataset_override = false;
   CHECK(captured->kind == services::ArtifactCompileKind::Benchmark);
   CHECK(captured->benchmark_selection == selected);
   const auto directory = services::materialize_artifact_compile(settings);
   REQUIRE(directory);
   CHECK(directory->kind == services::ArtifactCompileKind::Directory);
   settings.workflows.train.compile_benchmark_dataset_override = true;
  }
 }
}
}  // namespace
}  // namespace mmltk::controller
