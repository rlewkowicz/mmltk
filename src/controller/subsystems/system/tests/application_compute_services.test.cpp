#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/environment_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/detail/benchmark_progress.h"
#include "src/backend/data/detail/benchmark_compiler.h"
#include "src/backend/data/tests/benchmark_http_fixture.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/train_process_client.h"
#include "src/controller/services/vast_client.h"
#include "src/controller/services/vast_provider_owner.h"
#include "src/backend/data/tests/test_fixture.h"
namespace mmltk::controller::subsystems::system {
namespace {
namespace data = mmltk::backend::data;
namespace domain = mmltk::controller::contracts;
namespace rfdetr = mmltk::backend::models::rfdetr;
using namespace mmltk::controller::contracts;
using namespace mmltk::controller::services;
static_assert(std::is_copy_constructible_v<VastProviderClient>);
static_assert(!std::is_constructible_v<VastProviderClient, std::uint16_t, std::uint64_t>);
static_assert(!std::is_default_constructible_v<VastClient>);
static_assert(std::is_constructible_v<VastClient, VastProviderClient>);
static_assert(!std::is_constructible_v<VastClient, VastBridgeConfig>);
class VastTestOperations final : public VastOperations {
public:
 std::vector<VastOfferSummary> query(const VastQueryConfig&, const std::vector<ProviderGpuFamily>&, const VastBridgeInvocation&) const override {
  ++queries;
  if (query_observer != nullptr) ++*query_observer;
  return offers;
 }
 VastCreateInstanceResult create(const VastBridgeConfig&, int, std::string_view, const VastLaunchTemplateOptions& options, const VastBridgeInvocation&) const override {
  ++creates;
  created_label = options.label;
  if (throw_create) throw std::runtime_error("create effect failed");
  return create_result;
 }
 void start(const VastBridgeConfig&, int, const VastBridgeInvocation&) const override {
  ++starts;
  if (throw_start) throw std::runtime_error("start effect failed");
 }
 void stop(const VastBridgeConfig&, int, const VastBridgeInvocation&) const override {
  ++stops;
  if (throw_stop) throw std::runtime_error("stop effect failed");
 }
 VastInstanceInfo show(const VastBridgeConfig&, int id, const VastBridgeInvocation&) const override {
  ++shows;
  auto value = record;
  value.instance_id = returned_instance_id == 0 ? id : returned_instance_id;
  return value;
 }
 std::vector<VastInstanceInfo> inventory(const VastBridgeConfig&, const VastBridgeInvocation&) const override {
  ++inventories;
  return records;
 }
 std::string logs(const VastBridgeConfig&, int, std::optional<std::size_t>, const VastBridgeInvocation&) const override {
  ++log_reads;
  return output;
 }
 mutable std::size_t queries = 0U, creates = 0U, starts = 0U, stops = 0U, shows = 0U, inventories = 0U, log_reads = 0U;
 mutable std::string created_label;
 std::size_t* query_observer = nullptr;
 std::vector<VastOfferSummary> offers;
 std::vector<VastInstanceInfo> records;
 VastInstanceInfo record;
 std::string output;
 int returned_instance_id = 0;
 VastCreateInstanceResult create_result;
 bool throw_create = false, throw_start = false, throw_stop = false;
};
struct ArtifactCancellationFixture final {
 ArtifactCancellationFixture() : ArtifactCancellationFixture(ArtifactCancellationSource::Mint()) {}
 void cancel() noexcept { static_cast<void>(source.RequestCancel()); }
 ArtifactCancellationSource source;
 ArtifactCancellationToken token;

private:
 explicit ArtifactCancellationFixture(std::pair<ArtifactCancellationSource, ArtifactCancellationToken>&& pair) noexcept : source(std::move(pair.first)), token(std::move(pair.second)) {}
};
class ArtifactTestOperations final : public ArtifactWeightOperations {
public:
 std::optional<ArtifactWeightAsset> find(std::string_view preset) const override {
  ++finds;
  return preset == "fixture" ? std::optional<ArtifactWeightAsset>{{.filename = filename, .url = url, .md5 = md5}} : std::nullopt;
 }
 void download(std::string_view, const std::filesystem::path& output, const ArtifactCancellationToken& cancellation, ArtifactWeightProgressObserver progress) const override {
  ++downloads;
  progress({.stage = domain::ModelProgressStage::Downloading, .activity = "fixture download", .completed = payload.size(), .total = payload.size(), .total_known = true});
  if (cancel_on_download && cancellation_trigger != nullptr) {
   if (&cancellation_trigger->token != &cancellation) { throw std::runtime_error("artifact fixture cancellation identity mismatch"); }
   cancellation_trigger->cancel();
   return;
  }
  std::ofstream stream(output, std::ios::binary);
  stream << payload;
 }
 mutable std::size_t finds = 0U;
 mutable std::size_t downloads = 0U;
 bool cancel_on_download = false;
 ArtifactCancellationFixture* cancellation_trigger = nullptr;
 std::string filename = "fixture.bin";
 std::string url = "https://fixture.invalid/weight";
 std::string payload = "abc";
 std::string md5 = "900150983cd24fb0d6963f7d28e17f72";
};
[[nodiscard]] rfdetr::TrainRequest train_request(const std::filesystem::path& output) {
 rfdetr::TrainRequest request;
 request.train_compiled_path = output / "train.bin";
 request.val_compiled_path = output / "val.bin";
 request.output_dir = output;
 request.weights_path = output / "weights.pt";
 request.resolution = 384U;
 request.device_id = 0;
 return request;
}
[[nodiscard]] std::filesystem::path script(mmltk::testsupport::ScopedTempDir& temp, std::string_view body) {
 const auto path = temp.path() / "fixture.sh";
 mmltk::testsupport::write_text_file(path, std::string("#!/bin/sh\n") + std::string(body));
 if (::chmod(path.c_str(), 0700) != 0) throw std::runtime_error("cannot make train fixture executable");
 return path;
}
[[nodiscard]] bool ready(const int fd) {
 pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
 return ::poll(&descriptor, 1U, 2'000) > 0;
}
[[nodiscard]] TrainProcessClient launch_train_ignoring_term(mmltk::testsupport::ScopedTempDir& temp) {
 const auto executable = script(temp, "trap '' TERM\nprintf ready\nexec sleep 30\n");
 auto client = TrainProcessClient::launch(train_request(temp.path() / "output"), executable, {}, {.escalation_delay = std::chrono::milliseconds{10}});
 REQUIRE(ready(client.stdout_fd()));
 std::string readiness;
 client.consume_output(readiness, 5U);
 REQUIRE(readiness == "ready");
 return client;
}
[[nodiscard]] VastBridgeConfig valid_vast_config() {
 VastBridgeConfig config;
 config.api_key = "test-api-key";
 config.python_executable = "/usr/bin/python3";
 config.bridge_script_path = "/tmp/vast-bridge.py";
 config.http_timeout = std::chrono::milliseconds{1};
 return config;
}
class VastClientFixture final {
public:
 VastClientFixture()
     : operations_storage_{std::make_unique<VastTestOperations>()},
       operations_{operations_storage_.get()},
       provider_{valid_vast_config(), std::move(operations_storage_)},
       client_{provider_.client()},
       cancellation_{VastCancellationSource::Mint()} {}
 [[nodiscard]] VastTestOperations& operations() const noexcept { return *operations_; }
 [[nodiscard]] VastClient& client() noexcept { return client_; }
 [[nodiscard]] VastCancellationSource& source() noexcept { return cancellation_.first; }
 [[nodiscard]] const VastCancellationToken& cancellation() const noexcept { return cancellation_.second; }

private:
 std::unique_ptr<VastTestOperations> operations_storage_;
 VastTestOperations* operations_;
 VastProviderOwner provider_;
 VastClient client_;
 decltype(VastCancellationSource::Mint()) cancellation_;
};
[[nodiscard]] domain::ProviderPreferences valid_provider_preferences() {
 domain::ProviderPreferences preferences;
 preferences.minimum_gpus = 1;
 preferences.result_limit = 1U;
 preferences.families.push_back(ProviderGpuFamily::A100);
 preferences.image = "image";
 return preferences;
}
}  // namespace
TEST_CASE("artifact service refuses invalid and cancelled work without shared lifecycle state", "[gui][services]") {
 ArtifactStore store;
 ArtifactCancellationFixture cancellation;
 cancellation.cancel();
 const std::array<std::filesystem::path, domain::kArtifactSplitCapacity> paths{};
 const domain::ArtifactInspection inspection = store.inspect(paths, "rf-detr-nano", 384U, cancellation.token);
 CHECK_FALSE(inspection.compatible);
 CHECK_FALSE(inspection.detail.empty());
 const ArtifactCompileResult invalid = store.compile({}, cancellation.token);
 CHECK_FALSE(invalid.cancelled);
 CHECK_FALSE(invalid.inspection.detail.empty());
}
TEST_CASE("artifact service derives bounded inspection contracts from reflected state", "[gui][services]") {
 ArtifactStore store;
 ArtifactCancellationFixture cancellation;
 const std::array<std::filesystem::path, domain::kArtifactSplitCapacity> no_paths{};
 const domain::ArtifactInspection accepted = store.inspect(no_paths, "rf-detr-nano", 384U, cancellation.token);
 CHECK(accepted.detail == "compiled artifact inspection requires at least one path");
 CHECK(accepted.detail.size() <= domain::kArtifactErrorCapacity);
 std::array<std::filesystem::path, domain::kArtifactSplitCapacity> oversized{};
 oversized.front() = std::string(domain::kArtifactPathCapacity + 1U, 'p');
 const domain::ArtifactInspection rejected = store.inspect(oversized, "rf-detr-nano", 384U, cancellation.token);
 CHECK_FALSE(rejected.compatible);
 CHECK_FALSE(rejected.detail.empty());
 CHECK(rejected.detail.size() <= domain::kArtifactErrorCapacity);
 ArtifactCompileRequest exact{.source = std::filesystem::path(std::string(domain::kArtifactPathCapacity, 's')),
  .output = std::filesystem::path(std::string(domain::kArtifactPathCapacity, 'o')),
  .preset = std::string(domain::kArtifactPresetCapacity, 'p'),
  .resolution = 1U};
 CHECK(exact.valid());
 exact.source = std::filesystem::path(std::string(domain::kArtifactPathCapacity + 1U, 's'));
 CHECK_FALSE(exact.valid());
 exact.source = "/source";
 exact.output = std::filesystem::path(std::string(domain::kArtifactPathCapacity + 1U, 'o'));
 CHECK_FALSE(exact.valid());
 exact.output = "/output";
 exact.preset.assign(domain::kArtifactPresetCapacity + 1U, 'p');
 CHECK_FALSE(exact.valid());
 ArtifactTestOperations operations;
 ArtifactStore bounded_store("/tmp/mmltk-artifact-input-contract", operations);
 CHECK_THROWS(bounded_store.canonical_weight_path(std::string(domain::kArtifactPresetCapacity + 1U, 'p')));
 CHECK(operations.finds == 0U);
}
TEST_CASE("artifact service preserves canonical RF-DETR validation", "[gui][services]") {
 ArtifactStore store;
 CHECK_THROWS(store.canonical_weight_path("not-a-rf-detr-preset"));
}
TEST_CASE("artifact compile progress is borrowed for invalid and cancelled calls", "[gui][services]") {
 ArtifactStore store;
 ArtifactCancellationFixture cancellation;
 std::size_t reports = 0U;
 const ArtifactProgressObserver observer{.context = &reports, .report = [](void* context, const domain::ArtifactProgress&) { ++*static_cast<std::size_t*>(context); }};
 CHECK_FALSE(store.compile({}, cancellation.token, observer).inspection.detail.empty());
 cancellation.cancel();
 ArtifactCompileRequest request{.source = "/unavailable", .output = "/unavailable-output", .preset = "fixture", .resolution = 384U};
 CHECK(store.compile(request, cancellation.token, observer).cancelled);
 CHECK(reports == 0U);
}
TEST_CASE("artifact benchmark compilation uses the environment cache and retires staging", "[gui][services][benchmark]") {
 namespace fs = std::filesystem;
 mmltk::testsupport::ScopedTempDir temporary{"artifact-benchmark-cache"};
 const auto original_directory = fs::current_path();
 const mmltk::testsupport::ScopedEnvironmentVariable environment{"MMLTK_BENCHMARK_DATASET_CACHE_ROOT"};
 const mmltk::testsupport::ScopedTestCleanup restore_directory{[&] { fs::current_path(original_directory); }};
 fs::create_directories(temporary.path() / "working");
 fs::current_path(temporary.path() / "working");
 const auto output = temporary.path() / "compiled";
 auto cache = temporary.path() / "source-cache";
 bool overlap = false;
 SECTION("cancellation before annotation acquisition preserves the environment cache") {}
 SECTION("overlap rejection preserves cache and published output") {
  cache = temporary.path();
  overlap = true;
 }
 SECTION("cache equals final output while staging is a sibling") {
  cache = output;
  overlap = true;
 }
 SECTION("cache lies below final output while staging is a sibling") {
  cache = output / "cache";
  overlap = true;
 }
 SECTION("relative cache alias equals final output") {
  cache = "../compiled";
  overlap = true;
 }
 SECTION("symlink cache alias equals final output") {
  fs::create_directories(output);
  fs::create_directory_symlink(output, cache);
  overlap = true;
 }
 SECTION("symlink final output lies below cache while staging is a sibling") {
  fs::create_directories(cache / "published");
  fs::create_directory_symlink(cache / "published", output);
  overlap = true;
 }
 REQUIRE(::setenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT", cache.c_str(), 1) == 0);
 const auto retained = cache / "downloads/coco/retained.fixture";
 mmltk::testsupport::write_text_file(retained, "existing cached fixture");
 mmltk::testsupport::write_text_file(output / "train.bin", "existing published fixture");
 struct stat before{};
 REQUIRE(::stat(retained.c_str(), &before) == 0);
 struct stat published_before{};
 REQUIRE(::stat((output / "train.bin").c_str(), &published_before) == 0);
 ArtifactCancellationFixture cancellation;
 struct Observations final {
  mutable std::string paths;
  mutable std::size_t path_reports = 0U;
 };
 Observations observations;
 const ArtifactDiagnosticObserver diagnostics{.benchmark = {
                                               .context = &observations,
                                               .report =
                                                [](const void* context, const std::string_view event, const std::string_view fields) noexcept {
                                                 if (event == "benchmark.compile.paths") {
                                                  const auto& observed = *static_cast<const Observations*>(context);
                                                  observed.paths = fields;
                                                  ++observed.path_reports;
                                                 }
                                                },
                                              }};
 // Cancellation begins inside the real backend, after ArtifactStore's entry
 // check. Even a cache-selection regression cannot reach a public endpoint.
 const ArtifactProgressObserver progress{
  .context = &cancellation,
  .report = [](void* context, const domain::ArtifactProgress&) { static_cast<ArtifactCancellationFixture*>(context)->cancel(); },
 };
 ArtifactStore store;
 const ArtifactCompileRequest request{.kind = ArtifactCompileKind::Benchmark, .source = {}, .output = output, .preset = "rf-detr-nano", .resolution = 1U, .overwrite = true};
 const auto result = store.compile(request, cancellation.token, progress, diagnostics);
 CHECK(result.cancelled);
 CHECK(result.output.empty());
 CHECK(result.inspection.detail == (overlap ? "benchmark output and cache directories must not overlap" : "benchmark dataset compilation cancelled"));
 REQUIRE(observations.path_reports == 1U);
 const auto paths = nlohmann::json::parse(observations.paths);
 CHECK(paths.at("cache_root") == fs::weakly_canonical(cache).native());
 const fs::path staging{paths.at("output_root").get<std::string>()};
 CHECK(staging.parent_path() == fs::weakly_canonical(output.parent_path()));
 CHECK(staging.filename().string().starts_with("compiled.tmp."));
 CHECK_FALSE(fs::exists(staging));
 for (const auto& entry : fs::directory_iterator(output.parent_path())) { CHECK_FALSE(entry.path().filename().string().starts_with("compiled.tmp.")); }
 struct stat after{};
 REQUIRE(::stat(retained.c_str(), &after) == 0);
 CHECK(after.st_ino == before.st_ino);
 CHECK(after.st_size == before.st_size);
 CHECK(after.st_mtim.tv_sec == before.st_mtim.tv_sec);
 CHECK(after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
 std::ifstream cached{retained};
 CHECK(std::string(std::istreambuf_iterator<char>{cached}, {}) == "existing cached fixture");
 struct stat published_after{};
 REQUIRE(::stat((output / "train.bin").c_str(), &published_after) == 0);
 CHECK(published_after.st_ino == published_before.st_ino);
 CHECK(published_after.st_mtim.tv_sec == published_before.st_mtim.tv_sec);
 CHECK(published_after.st_mtim.tv_nsec == published_before.st_mtim.tv_nsec);
 std::ifstream published{output / "train.bin"};
 CHECK(std::string(std::istreambuf_iterator<char>{published}, {}) == "existing published fixture");
}
TEST_CASE("artifact compile adapters preserve canonical ordinary and benchmark estimates", "[gui][services]") {
 const data::CompileProgress ordinary{
  .done = 30U,
  .total = 90U,
  .elapsed_seconds = 6U,
  .remaining_seconds = 12U,
  .throughput_per_second = 5U,
  .phase = data::DatasetCompilePhase::Pixels,
  .dropped_instances = 7U,
 };
 const domain::ArtifactProgress projected_ordinary = project_artifact_progress(ordinary);
 CHECK(projected_ordinary.phase == data::DatasetCompilePhase::Pixels);
 CHECK(projected_ordinary.activity == "pixels");
 CHECK(projected_ordinary.completed == 30U);
 CHECK(projected_ordinary.total == 90U);
 CHECK(projected_ordinary.elapsed_seconds == 6U);
 CHECK(projected_ordinary.remaining_seconds == 12U);
 CHECK(projected_ordinary.throughput_per_second == 5U);
 CHECK(projected_ordinary.dropped_instances == 7U);
 CHECK(projected_ordinary.tracks == ordinary.tracks);
 data::BenchmarkCompileProgress benchmark{
  .phase = data::DatasetCompilePhase::Pixels,
  .activity = "Downloading",
  .activity_elapsed_seconds = 7U,
  .completed = 1U,
  .total = 10U,
  .projected_output_bytes = 4096U,
  .dropped_instances = 11U,
  .quarantined_images = 13U,
  .sources = {},
 };
 benchmark.current_source = data::BenchmarkDatasetSource::kOpenImagesV7;
 benchmark.sources.push_back({
  .source = data::BenchmarkDatasetSource::kOpenImagesV7,
  .activity = "Fetching shard",
  .completed_bytes = 128U,
  .total_bytes = 1024U,
  .resumed = true,
 });
 const domain::ArtifactProgress projected_benchmark = project_artifact_progress(benchmark);
 CHECK(projected_benchmark.activity == "Open Images v7 · Fetching shard · resumed");
 CHECK(projected_benchmark.elapsed_seconds == 7U);
 CHECK(projected_benchmark.remaining_seconds == 63U);
 CHECK(projected_benchmark.throughput_per_second == 0U);
 CHECK(projected_benchmark.projected_output_bytes == 4096U);
 CHECK(projected_benchmark.dropped_instances == 11U);
 CHECK(projected_benchmark.quarantined_images == 13U);
 CHECK(projected_benchmark.sources == benchmark.sources);
 CHECK(projected_benchmark.tracks == benchmark.tracks);
}
TEST_CASE("artifact service owns verified cache publication and cancellation cleanup", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temporary("mmltk-artifact-service-contract");
 const auto root = temporary.path() / "weights";
 ArtifactTestOperations operations;
 ArtifactStore store(root, operations);
 ArtifactCancellationFixture pre_cancelled;
 pre_cancelled.cancel();
 CHECK_THROWS(store.canonical_weight_path("fixture", pre_cancelled.token));
 CHECK(operations.finds == 0U);
 CHECK(operations.downloads == 0U);
 CHECK_FALSE(std::filesystem::exists(root / "fixture.bin"));
 ArtifactCancellationFixture first;
 std::vector<domain::ModelProgress> weight_progress;
 const ArtifactWeightProgressObserver observer{
  .context = &weight_progress, .report = [](void* context, const domain::ModelProgress& value) noexcept { static_cast<std::vector<domain::ModelProgress>*>(context)->push_back(value); }};
 CHECK(store.canonical_weight_path("fixture", first.token, observer) == root / "fixture.bin");
 CHECK(operations.finds == 1U);
 CHECK(operations.downloads == 1U);
 REQUIRE(weight_progress.size() >= 3U);
 CHECK(weight_progress.front().stage == domain::ModelProgressStage::InspectingCache);
 CHECK(std::ranges::any_of(weight_progress, [](const auto& value) { return value.stage == domain::ModelProgressStage::Downloading && value.total_known && value.completed == value.total; }));
 CHECK(weight_progress.back().stage == domain::ModelProgressStage::Verifying);
 ArtifactCancellationFixture cached;
 weight_progress.clear();
 CHECK(store.canonical_weight_path("fixture", cached.token, observer) == root / "fixture.bin");
 CHECK(operations.downloads == 1U);
 REQUIRE(weight_progress.size() == 2U);
 CHECK(weight_progress.front().stage == domain::ModelProgressStage::InspectingCache);
 CHECK(weight_progress.back().stage == domain::ModelProgressStage::Verifying);
 std::ofstream(root / "fixture.bin", std::ios::binary | std::ios::trunc) << "bad";
 CHECK(store.canonical_weight_path("fixture", cached.token) == root / "fixture.bin");
 CHECK(operations.downloads == 2U);
 operations.md5 = "00000000000000000000000000000000";
 CHECK_THROWS(store.canonical_weight_path("fixture", cached.token));
 CHECK_FALSE(std::filesystem::exists(root / "fixture.bin"));
 operations.md5 = "900150983cd24fb0d6963f7d28e17f72";
 operations.cancel_on_download = true;
 std::filesystem::remove(root / "fixture.bin");
 ArtifactCancellationFixture cancelled;
 operations.cancellation_trigger = &cancelled;
 CHECK_THROWS(store.canonical_weight_path("fixture", cancelled.token));
 for (const auto& entry : std::filesystem::directory_iterator(root)) { CHECK(entry.path().filename().string().find(".weights.") == std::string::npos); }
}
TEST_CASE("artifact service refuses hostile weight metadata before transfer or cache mutation", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temporary("mmltk-artifact-metadata-contract");
 const auto root = temporary.path() / "weights";
 ArtifactTestOperations operations;
 ArtifactStore store(root, operations);
 const std::array<std::string, 8U> hostile_filenames{
  "",
  ".",
  "..",
  "../outside.bin",
  "/outside.bin",
  "nested/weight.bin",
  "nested\\weight.bin",
  std::string(domain::kArtifactPathCapacity + 1U, 'f'),
 };
 for (const std::string& filename : hostile_filenames) {
  operations.filename = filename;
  operations.url = "https://fixture.invalid/weight";
  operations.md5 = "900150983cd24fb0d6963f7d28e17f72";
  const std::size_t downloads_before = operations.downloads;
  CHECK_THROWS(store.canonical_weight_path("fixture"));
  CHECK(operations.downloads == downloads_before);
  CHECK_FALSE(std::filesystem::exists(root / "fixture.bin"));
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "outside.bin"));
 }
 operations.filename = "fixture.bin";
 operations.url = "http://fixture.invalid/weight";
 CHECK_THROWS(store.canonical_weight_path("fixture"));
 operations.url = "https://fixture.invalid/ weight";
 CHECK_THROWS(store.canonical_weight_path("fixture"));
 operations.url = "https://fixture.invalid/weight";
 operations.md5 = "not-a-md5";
 CHECK_THROWS(store.canonical_weight_path("fixture"));
 operations.md5 = "900150983cd24fb0d6963f7d28e17f72";
 operations.url.assign(domain::kArtifactPathCapacity + 1U, 'u');
 CHECK_THROWS(store.canonical_weight_path("fixture"));
 CHECK(operations.downloads == 0U);
 CHECK_FALSE(std::filesystem::exists(root));
}
TEST_CASE("compute service domain values retain ordered terminal-safe progress", "[gui][services]") {
 const domain::ComputeProgress initial{.sequence = 1U, .completed = 0U, .total = 8U, .status = "training"};
 const domain::ComputeProgress terminal{.sequence = 2U, .completed = 8U, .total = 8U, .status = "complete"};
 CHECK(initial.valid());
 CHECK(terminal.valid());
 CHECK_FALSE(domain::ComputeProgress{.sequence = 0U, .completed = 0U, .total = 0U, .status = {}}.valid());
 CHECK_FALSE(domain::ComputeProgress{.sequence = 1U, .completed = 9U, .total = 8U, .status = {}}.valid());
 CHECK(domain::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 0U, .status = {}}.valid());
 CHECK_FALSE(domain::ComputeProgress{.sequence = 1U, .completed = 0U, .total = 1U, .status = std::string(domain::kComputeStatusCapacity + 1U, 's')}.valid());
 CHECK(domain::bounded_compute_error(std::string(domain::kComputeErrorCapacity + 1U, 'e')).size() == domain::kComputeErrorCapacity);
}
TEST_CASE("Vast client owns bounded public service limits", "[gui][services]") {
 CHECK(kVastOfferCapacity == 32U);
 CHECK(kVastInventoryCapacity == 64U);
 CHECK(kVastLogCapacity == 64U * 1024U);
 CHECK(kVastLogTailLineLimit == 10'000U);
}
TEST_CASE("Vast provider cancellation transfers one system source to one worker token", "[gui][services]") {
 auto [source, token] = VastCancellationSource::Mint();
 CHECK_FALSE(token.cancelled());
 CHECK(source.RequestCancel());
 CHECK(token.cancelled());
 CHECK_FALSE(source.RequestCancel());
}
TEST_CASE("Vast provider client retains its immutable ordinary dependency", "[gui][services]") {
 auto operations_storage = std::make_unique<VastTestOperations>();
 auto* const operations = operations_storage.get();
 operations->offers.resize(1U);
 operations->offers.front().offer_id = 7;
 operations->offers.front().num_gpus = 1;
 std::size_t query_count = 0U;
 operations->query_observer = &query_count;
 VastProviderClient stale;
 {
  VastProviderOwner owner{valid_vast_config(), std::move(operations_storage)};
  stale = owner.client();
  REQUIRE(stale.valid());
  auto [source, cancellation] = VastCancellationSource::Mint();
  static_cast<void>(source);
  CHECK(VastClient{stale}.query(valid_provider_preferences(), cancellation).size() == 1U);
 }
 CHECK(stale.valid());
 auto [source, cancellation] = VastCancellationSource::Mint();
 static_cast<void>(source);
 CHECK(VastClient{stale}.query(valid_provider_preferences(), cancellation).size() == 1U);
 CHECK(query_count == 2U);
 auto successor_operations = std::make_unique<VastTestOperations>();
 VastProviderOwner successor{valid_vast_config(), std::move(successor_operations)};
 const auto live = successor.client();
 CHECK(live.valid());
 CHECK_FALSE(live == stale);
}
TEST_CASE("Vast provider owner validates configuration before dependency publication", "[gui][services]") {
 VastProviderOwner absent;
 CHECK_FALSE(absent.client().valid());
 auto missing_key = valid_vast_config();
 missing_key.api_key.clear();
 CHECK_THROWS(VastProviderOwner(std::move(missing_key), std::make_unique<VastTestOperations>()));
 auto missing_path = valid_vast_config();
 missing_path.bridge_script_path.clear();
 CHECK_THROWS(VastProviderOwner(std::move(missing_path), std::make_unique<VastTestOperations>()));
 auto invalid_timeout = valid_vast_config();
 invalid_timeout.http_timeout = std::chrono::milliseconds::zero();
 CHECK_THROWS(VastProviderOwner(std::move(invalid_timeout), std::make_unique<VastTestOperations>()));
 CHECK_THROWS(VastProviderOwner(valid_vast_config(), std::unique_ptr<const VastOperations>{}));
}
TEST_CASE("duplicated Vast cancellation signal reaches a running invocation fd", "[gui][services]") {
 class BlockingOperations final : public VastOperations {
 public:
  std::vector<VastOfferSummary> query(const VastQueryConfig&, const std::vector<ProviderGpuFamily>&, const VastBridgeInvocation& invocation) const override {
   started.count_down();
   pollfd event{.fd = invocation.cancellation_fd, .events = POLLIN, .revents = 0};
   int ready = -1;
   do { ready = ::poll(&event, 1U, -1); } while (ready < 0 && errno == EINTR);
   observed = ready > 0 && (event.revents & POLLIN) != 0;
   return {};
  }
  VastCreateInstanceResult create(const VastBridgeConfig&, int, std::string_view, const VastLaunchTemplateOptions&, const VastBridgeInvocation&) const override { return {}; }
  void start(const VastBridgeConfig&, int, const VastBridgeInvocation&) const override {}
  void stop(const VastBridgeConfig&, int, const VastBridgeInvocation&) const override {}
  VastInstanceInfo show(const VastBridgeConfig&, int, const VastBridgeInvocation&) const override { return {}; }
  std::vector<VastInstanceInfo> inventory(const VastBridgeConfig&, const VastBridgeInvocation&) const override { return {}; }
  std::string logs(const VastBridgeConfig&, int, std::optional<std::size_t>, const VastBridgeInvocation&) const override { return {}; }
  mutable std::latch started{1};
  mutable bool observed = false;
 };
 auto operations_storage = std::make_unique<BlockingOperations>();
 auto* const operations = operations_storage.get();
 VastProviderOwner provider{valid_vast_config(), std::move(operations_storage)};
 auto [source, token] = VastCancellationSource::Mint();
 auto signal = source.DuplicateSignal();
 VastClient client{provider.client()};
 const auto preferences = valid_provider_preferences();
 std::thread running{[&] { static_cast<void>(client.query(preferences, token)); }};
 operations->started.wait();
 REQUIRE(signal.RequestCancel());
 running.join();
 CHECK(operations->observed);
 CHECK(token.cancelled());
 CHECK_FALSE(signal.RequestCancel());
}
TEST_CASE("Vast client owns bounded conversion and reconciliation over injected operations", "[gui][services]") {
 auto operations_storage = std::make_unique<VastTestOperations>();
 auto* const operations = operations_storage.get();
 operations->offers.resize(1U);
 operations->offers.front().offer_id = 7;
 operations->offers.front().num_gpus = 1;
 operations->record.actual_status = "running";
 operations->output.assign(kVastLogCapacity + 8U, 'x');
 operations->create_result = {.success = true, .offer_id = 7, .instance_id = 9, .instance_api_key = {}};
 VastProviderOwner provider{valid_vast_config(), std::move(operations_storage)};
 auto [source, cancellation] = VastCancellationSource::Mint();
 VastClient client{provider.client()};
 const auto preferences = valid_provider_preferences();
 CHECK(client.query(preferences, cancellation).front().offer_id == 7);
 CHECK(operations->queries == 1U);
 VastEffectAttempt create_attempt;
 CHECK_NOTHROW(client.create(7, preferences, "launch-7", create_attempt, cancellation));
 CHECK(create_attempt.started());
 CHECK(operations->created_label == "launch-7");
 VastEffectAttempt start_attempt;
 client.mutate(domain::ProviderMutation::Start, 9, start_attempt, cancellation);
 VastEffectAttempt stop_attempt;
 client.mutate(domain::ProviderMutation::Stop, 9, stop_attempt, cancellation);
 CHECK(start_attempt.started());
 CHECK(stop_attempt.started());
 CHECK(operations->creates == 1U);
 CHECK(operations->starts == 1U);
 CHECK(operations->stops == 1U);
 const auto reconciliation = client.reconcile({.mutation = domain::ProviderMutation::Start, .instance_id = 9, .launch_token = {}}, cancellation);
 CHECK(reconciliation.disposition == VastReconciliation::Disposition::Applied);
 CHECK(client.instance(9, cancellation).instance_id == 9);
 CHECK(client.logs(9, 1U, cancellation).size() == kVastLogCapacity);
 CHECK(operations->shows == 2U);
 CHECK(operations->log_reads == 1U);
 operations->offers.resize(kVastOfferCapacity + 1U);
 CHECK_THROWS(client.query(preferences, cancellation));
 CHECK(source.RequestCancel());
 VastEffectAttempt cancelled_attempt;
 CHECK_THROWS_AS(client.mutate(domain::ProviderMutation::Start, 9, cancelled_attempt, cancellation), VastBridgeError);
 CHECK_FALSE(cancelled_attempt.started());
 CHECK(operations->starts == 1U);
}
TEST_CASE("Vast client rejects duplicate offer identities at the injected service boundary", "[gui][services]") {
 auto operations_storage = std::make_unique<VastTestOperations>();
 auto* const operations = operations_storage.get();
 operations->offers.resize(2U);
 operations->offers[0].offer_id = 7;
 operations->offers[0].num_gpus = 1;
 operations->offers[1].offer_id = 8;
 operations->offers[1].num_gpus = 1;
 VastProviderOwner provider{valid_vast_config(), std::move(operations_storage)};
 VastClient client{provider.client()};
 auto [source, cancellation] = VastCancellationSource::Mint();
 static_cast<void>(source);
 auto preferences = valid_provider_preferences();
 preferences.result_limit = 2U;
 const auto unique = client.query(preferences, cancellation);
 REQUIRE(unique.size() == 2U);
 CHECK(unique[0].offer_id == 7);
 CHECK(unique[1].offer_id == 8);
 operations->offers[1].offer_id = 7;
 bool duplicate_rejected = false;
 try {
  static_cast<void>(client.query(preferences, cancellation));
 } catch (const std::runtime_error& error) {
  duplicate_rejected = true;
  CHECK(std::string_view{error.what()} == "Vast provider returned duplicate offer identities");
 }
 CHECK(duplicate_rejected);
 CHECK(operations->queries == 2U);
}
TEST_CASE("Vast client rejects mismatched exact-instance provider records", "[gui][services]") {
 auto operations_storage = std::make_unique<VastTestOperations>();
 auto* const operations = operations_storage.get();
 operations->record.actual_status = "running";
 operations->returned_instance_id = 18;
 VastProviderOwner provider{valid_vast_config(), std::move(operations_storage)};
 VastClient client{provider.client()};
 auto [source, cancellation] = VastCancellationSource::Mint();
 static_cast<void>(source);
 CHECK_THROWS(client.instance(9, cancellation));
 CHECK_THROWS(client.reconcile({.mutation = domain::ProviderMutation::Start, .instance_id = 9, .launch_token = {}}, cancellation));
 CHECK(operations->shows == 2U);
}
TEST_CASE("Vast client validates before provider effects and classifies all reconciliation states", "[gui][services]") {
 auto operations_storage = std::make_unique<VastTestOperations>();
 auto* const operations = operations_storage.get();
 VastProviderOwner provider{valid_vast_config(), std::move(operations_storage)};
 VastClient client{provider.client()};
 auto [source, cancellation] = VastCancellationSource::Mint();
 auto preferences = valid_provider_preferences();
 preferences.result_limit = 0U;
 CHECK_THROWS(client.query(preferences, cancellation));
 CHECK(operations->queries == 0U);
 preferences = valid_provider_preferences();
 preferences.families.clear();
 CHECK_THROWS(client.query(preferences, cancellation));
 CHECK(operations->queries == 0U);
 VastEffectAttempt invalid_instance_attempt;
 CHECK_THROWS(client.mutate(domain::ProviderMutation::Start, 0, invalid_instance_attempt, cancellation));
 CHECK_FALSE(invalid_instance_attempt.started());
 CHECK(operations->starts == 0U);
 const auto corrupt_mutation = static_cast<domain::ProviderMutation>(255U);
 CHECK_FALSE(valid_vast_mutation(corrupt_mutation));
 CHECK_FALSE(VastReconciliationRequest{.mutation = corrupt_mutation, .instance_id = 3, .launch_token = {}}.valid());
 VastEffectAttempt invalid_mutation_attempt;
 CHECK_THROWS(client.mutate(corrupt_mutation, 3, invalid_mutation_attempt, cancellation));
 CHECK_FALSE(invalid_mutation_attempt.started());
 CHECK(operations->starts == 0U);
 CHECK(operations->stops == 0U);
 CHECK_THROWS(client.reconcile({.mutation = corrupt_mutation, .instance_id = 3, .launch_token = {}}, cancellation));
 CHECK(operations->shows == 0U);
 CHECK(operations->inventories == 0U);
 CHECK_THROWS(client.logs(1, 0U, cancellation));
 CHECK(operations->log_reads == 0U);
 operations->record.actual_status = "stopped";
 CHECK(client.reconcile({.mutation = domain::ProviderMutation::Start, .instance_id = 3, .launch_token = {}}, cancellation).disposition == VastReconciliation::Disposition::NotApplied);
 operations->record.actual_status = "pending";
 CHECK(client.reconcile({.mutation = domain::ProviderMutation::Start, .instance_id = 3, .launch_token = {}}, cancellation).disposition == VastReconciliation::Disposition::Inconclusive);
 VastInstanceInfo created;
 created.instance_id = 17;
 created.label = "launch-17";
 operations->records = {created};
 const auto create_reconciliation = client.reconcile({.mutation = domain::ProviderMutation::Create, .instance_id = 0, .launch_token = "launch-17"}, cancellation);
 CHECK(create_reconciliation.disposition == VastReconciliation::Disposition::Applied);
 CHECK(create_reconciliation.instance->instance_id == 17);
 operations->records.clear();
 const auto missing = client.reconcile({.mutation = domain::ProviderMutation::Create, .instance_id = 0, .launch_token = "missing"}, cancellation);
 CHECK(missing.disposition == VastReconciliation::Disposition::NotApplied);
 CHECK_FALSE(missing.instance.has_value());
 VastInstanceInfo duplicate_a;
 duplicate_a.instance_id = 18;
 duplicate_a.label = "duplicate";
 VastInstanceInfo duplicate_b;
 duplicate_b.instance_id = 19;
 duplicate_b.label = "duplicate";
 operations->records = {duplicate_a, duplicate_b};
 const auto duplicate = client.reconcile({.mutation = domain::ProviderMutation::Create, .instance_id = 0, .launch_token = "duplicate"}, cancellation);
 CHECK(duplicate.disposition == VastReconciliation::Disposition::Inconclusive);
 CHECK_FALSE(duplicate.instance.has_value());
 operations->records = {duplicate_b, duplicate_a};
 const auto reordered_duplicate = client.reconcile({.mutation = domain::ProviderMutation::Create, .instance_id = 0, .launch_token = "duplicate"}, cancellation);
 CHECK(reordered_duplicate.disposition == VastReconciliation::Disposition::Inconclusive);
 CHECK_FALSE(reordered_duplicate.instance.has_value());
 operations->records.resize(kVastInventoryCapacity + 1U);
 CHECK_THROWS(client.instances(cancellation));
 CHECK(source.RequestCancel());
 VastEffectAttempt cancelled_create_attempt;
 CHECK_THROWS_AS(client.create(1, valid_provider_preferences(), "launch-1", cancelled_create_attempt, cancellation), VastBridgeError);
 CHECK_FALSE(cancelled_create_attempt.started());
 CHECK(operations->creates == 0U);
}
TEST_CASE("Vast effect attempt records the exact external mutation edge", "[gui][services]") {
 VastClientFixture fixture;
 auto& operations = fixture.operations();
 auto& client = fixture.client();
 const auto& cancellation = fixture.cancellation();
 const auto preferences = valid_provider_preferences();
 operations.throw_create = true;
 VastEffectAttempt create_attempt;
 CHECK_THROWS(client.create(7, preferences, "launch-7", create_attempt, cancellation));
 CHECK(create_attempt.started());
 CHECK(operations.creates == 1U);
 operations.throw_start = true;
 VastEffectAttempt start_attempt;
 CHECK_THROWS(client.mutate(domain::ProviderMutation::Start, 9, start_attempt, cancellation));
 CHECK(start_attempt.started());
 CHECK(operations.starts == 1U);
}
TEST_CASE("Vast admission rejects malformed input before provider effects", "[gui][services]") {
 VastClientFixture fixture;
 auto& operations = fixture.operations();
 auto& client = fixture.client();
 const auto& cancellation = fixture.cancellation();
 auto preferences = valid_provider_preferences();
 preferences.families.clear();
 CHECK_THROWS(client.query(preferences, cancellation));
 preferences = valid_provider_preferences();
 preferences.families.push_back(ProviderGpuFamily::A100);
 CHECK_THROWS(client.query(preferences, cancellation));
 preferences = valid_provider_preferences();
 preferences.minimum_gpus = 0;
 CHECK_THROWS(client.query(preferences, cancellation));
 preferences = valid_provider_preferences();
 preferences.result_limit = kVastOfferCapacity + 1U;
 CHECK_THROWS(client.query(preferences, cancellation));
 preferences = valid_provider_preferences();
 preferences.template_text =
  R"({"args":["","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","",""]})";
 VastEffectAttempt malformed_attempt;
 CHECK_THROWS(client.create(1, preferences, "token", malformed_attempt, cancellation));
 CHECK_FALSE(malformed_attempt.started());
 preferences = valid_provider_preferences();
 preferences.image.assign(domain::kProviderTextCapacity + 1U, 'i');
 VastEffectAttempt image_attempt;
 CHECK_THROWS(client.create(1, preferences, "token", image_attempt, cancellation));
 CHECK_FALSE(image_attempt.started());
 VastEffectAttempt token_attempt;
 CHECK_THROWS(client.create(1, valid_provider_preferences(), std::string(kVastLaunchTokenCapacity + 1U, 't'), token_attempt, cancellation));
 CHECK_FALSE(token_attempt.started());
 CHECK(operations.queries == 0U);
 CHECK(operations.creates == 0U);
}
// CLEANUP-IGNORE: These are aliases into the existing Vast fixture; mutation-edge and result-admission assertions remain independent.
TEST_CASE("Vast create accepts only complete provider result records", "[gui][services]") {
 VastClientFixture fixture;
 auto& operations = fixture.operations();
 auto& client = fixture.client();
 const auto& cancellation = fixture.cancellation();
 const auto preferences = valid_provider_preferences();
 const auto create = [&] {
  VastEffectAttempt attempt;
  return client.create(17, preferences, "launch-17", attempt, cancellation);
 };
 operations.create_result = {.success = true, .offer_id = 17, .instance_id = 31, .instance_api_key = {}};
 const auto success_without_credential = create();
 CHECK(success_without_credential.success);
 CHECK(success_without_credential.instance_id == 31);
 CHECK(operations.creates == 1U);
 operations.create_result = {.success = true, .offer_id = 17, .instance_id = 32, .instance_api_key = std::string(kVastApiKeyCapacity, 'k')};
 const auto success_with_credential = create();
 CHECK(success_with_credential.success);
 CHECK(success_with_credential.instance_api_key.size() == kVastApiKeyCapacity);
 CHECK(operations.creates == 2U);
 operations.create_result = {.success = false, .offer_id = 17, .instance_id = 0, .instance_api_key = {}};
 const auto failure = create();
 CHECK_FALSE(failure.success);
 CHECK(operations.creates == 3U);
 const std::array<VastCreateInstanceResult, 11U> malformed{{
  {.success = true, .offer_id = 0, .instance_id = 31, .instance_api_key = {}},
  {.success = true, .offer_id = 16, .instance_id = 31, .instance_api_key = {}},
  {.success = true, .offer_id = 17, .instance_id = 0, .instance_api_key = {}},
  {.success = true, .offer_id = 17, .instance_id = -1, .instance_api_key = {}},
  {.success = true, .offer_id = 17, .instance_id = 31, .instance_api_key = std::string(kVastApiKeyCapacity + 1U, 'k')},
  {.success = false, .offer_id = 0, .instance_id = 0, .instance_api_key = {}},
  {.success = false, .offer_id = 16, .instance_id = 0, .instance_api_key = {}},
  {.success = false, .offer_id = 17, .instance_id = 1, .instance_api_key = {}},
  {.success = false, .offer_id = 17, .instance_id = -1, .instance_api_key = {}},
  {.success = false, .offer_id = 17, .instance_id = 0, .instance_api_key = "unexpected-key"},
  {.success = false, .offer_id = 17, .instance_id = 0, .instance_api_key = std::string(kVastApiKeyCapacity + 1U, 'k')},
 }};
 for (const auto& result : malformed) {
  operations.create_result = result;
  const std::size_t effects_before = operations.creates;
  CHECK_THROWS(create());
  CHECK(operations.creates == effects_before + 1U);
 }
 CHECK(operations.creates == malformed.size() + 3U);
}
TEST_CASE("artifact compile reports synchronously through the caller-owned observer", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temporary("mmltk-artifact-progress");
 const mmltk::backend::data::testsupport::FixtureSpec fixture{.root_dir = temporary.path().string(), .split = "train", .width = 16, .height = 16, .num_images = 1};
 mmltk::backend::data::testsupport::create_synthetic_dataset(fixture);
 ArtifactStore store;
 ArtifactCancellationFixture cancellation;
 struct Reports {
  std::size_t count = 0U;
  bool accepting = true;
 } reports;
 const ArtifactProgressObserver observer{.context = &reports, .report = [](void* context, const domain::ArtifactProgress&) {
                                          auto& value = *static_cast<Reports*>(context);
                                          REQUIRE(value.accepting);
                                          ++value.count;
                                         }};
 const ArtifactCompileResult result = store.compile(
  {.source = mmltk::backend::data::testsupport::dataset_dir(fixture), .output = temporary.path() / "compiled", .preset = "rf-detr-nano", .resolution = 16U}, cancellation.token, observer);
 reports.accepting = false;
 CHECK(result.inspection.detail.empty());
 CHECK(std::filesystem::exists(result.output));
 CHECK(reports.count >= 1U);
 const auto compiled = result.output / "train.bin";
 data::FileHeader header{};
 {
  std::fstream stream(compiled, std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(stream);
  stream.read(reinterpret_cast<char*>(&header), sizeof(header));
  REQUIRE(stream);
  header.class_names[0].fill('n');
  header.class_names[0].back() = '\0';
  stream.seekp(0);
  stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
  REQUIRE(stream);
 }
 std::array<std::filesystem::path, domain::kArtifactSplitCapacity> paths{};
 paths.front() = compiled;
 const domain::ArtifactInspection accepted = store.inspect(paths, "rf-detr-nano", 16U, cancellation.token);
 REQUIRE(accepted.compatible);
 REQUIRE(accepted.splits.size() == 1U);
 CHECK(accepted.splits.front().class_names.size() == header.num_classes);
 CHECK(accepted.splits.front().class_names.front().value.size() == header.class_names.front().size() - 1U);
 {
  std::fstream stream(compiled, std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(stream);
  header.num_classes = data::MAX_CLASSES + 1U;
  stream.seekp(0);
  stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
  REQUIRE(stream);
 }
 const domain::ArtifactInspection oversized_catalog = store.inspect(paths, "rf-detr-nano", 16U, cancellation.token);
 CHECK_FALSE(oversized_catalog.compatible);
 CHECK(oversized_catalog.splits.empty());
 CHECK_FALSE(oversized_catalog.detail.empty());
 CHECK(oversized_catalog.detail.size() <= domain::kArtifactErrorCapacity);
}
TEST_CASE("Train process client exposes setup failure and one terminal", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 auto client = TrainProcessClient::launch(train_request(temp.path() / "output"), temp.path() / "missing");
 REQUIRE(ready(client.pid_fd()));
 const auto terminal = client.consume_exit();
 REQUIRE(terminal.has_value());
 CHECK(terminal->setup_failure);
 CHECK_FALSE(client.consume_exit().has_value());
}
TEST_CASE("Train process run owns its stop token, forwards progress, and reaps success", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-run");
 const auto output = temp.path() / "output";
 const auto executable = script(temp,
  "out=''; while [ $# -gt 0 ]; do [ \"$1\" = '--output-dir' ] && { out=\"$2\"; break; }; shift; done\n"
  "printf "
  "'{\"phase\":\"training\",\"completed_batches\":1,\"total_batches\":1,\"checkpoint_path\":\"checkpoint."
  "pt\"}' > \"$out/progress.json\"\n"
  "printf complete\n");
 auto client = TrainProcessClient::launch(train_request(output), executable);
 auto [source, token] = TrainProcessStopSource::Mint();
 std::size_t progress_reports = 0U;
 const auto result = client.Run(std::move(token), {.context = &progress_reports, .report = [](void* context, const TrainProcessProgress&) noexcept { ++*static_cast<std::size_t*>(context); }});
 CHECK(result.terminal.outcome == services::TrainProcessExitOutcome::Succeeded);
 CHECK_FALSE(result.terminal.setup_failure);
 CHECK_FALSE(client.active());
 CHECK(progress_reports >= 1U);
 CHECK(source.RequestCancel());
}
TEST_CASE("Train process run consumes a separately-owned stop capability and escalates", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-run-stop");
 auto client = launch_train_ignoring_term(temp);
 auto [source, token] = TrainProcessStopSource::Mint();
 REQUIRE(source.RequestCancel());
 const auto result = client.Run(std::move(token));
 CHECK(result.terminal.outcome == services::TrainProcessExitOutcome::Cancelled);
 REQUIRE(WIFSIGNALED(result.terminal.wait_status));
 CHECK(WTERMSIG(result.terminal.wait_status) == SIGKILL);
 CHECK_FALSE(client.active());
 CHECK_FALSE(source.RequestCancel());
}
// CLEANUP-IGNORE: The independently named "Train process client observes bounded output and inotify progress" scenario
// keeps its own setup and oracle adjacent.
TEST_CASE("Train process client observes bounded output and inotify progress", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 const auto output = temp.path() / "output";
 std::filesystem::create_directories(output);
 REQUIRE(::mkfifo((temp.path() / "gate").c_str(), 0600) == 0);
 const auto executable = script(temp,
  "out=''; while [ $# -gt 0 ]; do [ \"$1\" = '--output-dir' ] && { out=\"$2\"; break; }; shift; done\n"
  "read ignored < \"$(dirname \"$out\")/gate\"\n"
  "printf first; printf "
  "'{\"phase\":\"training\",\"completed_batches\":1,\"total_batches\":2,\"checkpoint_path\":\"checkpoint."
  "pt\"}' > \"$out/progress.json\"\n"
  "printf second; printf '{\"best_checkpoint\":\"best.pt\"}' > \"$out/results.json\"\n");
 auto client = TrainProcessClient::launch(train_request(output), executable);
 {
  std::ofstream gate(temp.path() / "gate");
  gate << "go\n";
 }
 REQUIRE(ready(client.stdout_fd()));
 std::string stdout;
 client.consume_output(stdout, 5U);
 CHECK(stdout == "first");
 REQUIRE(ready(client.stdout_fd()));
 client.consume_output(stdout, 64U);
 CHECK(stdout == "firstsecond");
 REQUIRE(ready(client.progress_fd()));
 const auto progress = client.consume_progress();
 REQUIRE(progress.has_value());
 CHECK(progress->progress.completed == 1U);
 CHECK(progress->checkpoint_path == "checkpoint.pt");
 REQUIRE(ready(client.pid_fd()));
 const auto terminal = client.consume_exit();
 REQUIRE(terminal.has_value());
 CHECK(terminal->outcome == services::TrainProcessExitOutcome::Succeeded);
}
// CLEANUP-IGNORE: The independently named "Train process client rejects oversized public progress fields" scenario
// keeps its own setup and oracle adjacent.
TEST_CASE("Train process client rejects oversized public progress fields", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 const auto output = temp.path() / "output";
 std::filesystem::create_directories(output);
 REQUIRE(::mkfifo((temp.path() / "gate").c_str(), 0600) == 0);
 const auto executable = script(temp,
  "out=''; while [ $# -gt 0 ]; do [ \"$1\" = '--output-dir' ] && { out=\"$2\"; break; }; shift; done\n"
  "read ignored < \"$(dirname \"$out\")/gate\"\n"
  "long=$(yes x | tr -d '\\n' | head -c 4097)\n"
  "printf '{\"phase\":\"%s\",\"checkpoint_path\":\"checkpoint.pt\"}' \"$long\" > \"$out/progress.json\"\n");
 auto client = TrainProcessClient::launch(train_request(output), executable);
 {
  std::ofstream gate(temp.path() / "gate");
  gate << "go\n";
 }
 REQUIRE(ready(client.progress_fd()));
 CHECK_THROWS(client.consume_progress());
}
TEST_CASE("Train process client rejects oversized public checkpoint paths", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 const auto output = temp.path() / "output";
 std::filesystem::create_directories(output);
 REQUIRE(::mkfifo((temp.path() / "gate").c_str(), 0600) == 0);
 const auto executable = script(temp,
  "out=''; while [ $# -gt 0 ]; do [ \"$1\" = '--output-dir' ] && { out=\"$2\"; break; }; shift; done\n"
  "read ignored < \"$(dirname \"$out\")/gate\"\n"
  "long=$(yes x | tr -d '\\n' | head -c 4097)\n"
  "printf '{\"phase\":\"training\",\"checkpoint_path\":\"%s\"}' \"$long\" > \"$out/progress.json\"\n");
 auto client = TrainProcessClient::launch(train_request(output), executable);
 {
  std::ofstream gate(temp.path() / "gate");
  gate << "go\n";
 }
 REQUIRE(ready(client.progress_fd()));
 CHECK_THROWS(client.consume_progress());
}
TEST_CASE("Train process client escalates a stopped process group", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 auto client = launch_train_ignoring_term(temp);
 REQUIRE(client.request_stop(false));
 REQUIRE(ready(client.control_fd()));
 REQUIRE(client.consume_stop_request());
 REQUIRE(ready(client.escalation_fd()));
 CHECK(client.consume_escalation());
 REQUIRE(ready(client.pid_fd()));
 const auto terminal = client.consume_exit();
 REQUIRE(terminal.has_value());
 CHECK(terminal->outcome == services::TrainProcessExitOutcome::Cancelled);
 REQUIRE(WIFSIGNALED(terminal->wait_status));
 CHECK(WTERMSIG(terminal->wait_status) == SIGKILL);
}
TEST_CASE("Train process client retains group custody after its leader exits", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 const auto executable = script(temp, "(trap '' TERM; exec sleep 30) &\nexit 0\n");
 auto client = TrainProcessClient::launch(train_request(temp.path() / "output"), executable, {}, {.escalation_delay = std::chrono::milliseconds{10}});
 REQUIRE(ready(client.pid_fd()));
 CHECK_FALSE(client.consume_exit().has_value());
 REQUIRE(client.request_stop(true));
 REQUIRE(ready(client.control_fd()));
 REQUIRE(client.consume_stop_request());
 REQUIRE(ready(client.escalation_fd()));
 static_cast<void>(client.consume_escalation());
 const auto terminal = client.consume_exit();
 REQUIRE(terminal.has_value());
 CHECK_FALSE(client.consume_exit().has_value());
}
TEST_CASE("Train process client destructor reaps a live process group", "[gui][services]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-service");
 const auto executable = script(temp, "exec sleep 30\n");
 pid_t group = -1;
 {
  auto client = TrainProcessClient::launch(train_request(temp.path() / "output"), executable);
  group = client.process_group_id();
  REQUIRE(group > 0);
 }
 CHECK(::kill(-group, 0) == -1);
 CHECK(errno == ESRCH);
}
TEST_CASE("Train observes persistence failure beyond bounded console output without a progress file", "[gui][services][train]") {
 mmltk::testsupport::ScopedTempDir temp("mmltk-train-persistence-marker");
 const auto executable = script(temp,
  "head -c 131064 /dev/zero\n"
  "printf '\\nMMLTK_TRAIN_PERSISTENCE_FAILED_V1\\n'\n");
 auto client = TrainProcessClient::launch(train_request(temp.path() / "output"), executable);
 auto [source, token] = TrainProcessStopSource::Mint();
 const auto result = client.Run(std::move(token));
 CHECK(result.terminal.outcome == services::TrainProcessExitOutcome::Succeeded);
 REQUIRE(result.terminal.final_progress.has_value());
 CHECK(result.terminal.final_progress->persistence.degraded);
 CHECK(result.output.size() <= kTrainProcessReadBudget);
 CHECK_FALSE(result.terminal.final_progress->persistence.error.empty());
}
TEST_CASE("artifact benchmark requests admit canonical selections and Directory ignores retained choices", "[gui][services][benchmark]") {
 namespace data = mmltk::backend::data;
 ArtifactCompileRequest request{.kind = mmltk::controller::services::ArtifactCompileKind::Benchmark, .source = "/source", .output = "/output", .preset = "rf-detr-nano", .resolution = 384U};
 for (const auto dataset : {data::BenchmarkDatasetVariant::CocoCustom, data::BenchmarkDatasetVariant::Coconut}) {
  for (const auto validation : {data::CoconutValidation::Coconut, data::CoconutValidation::Stock, data::CoconutValidation::CoconutStock}) {
   request.benchmark_selection = {dataset, validation};
   CHECK(request.valid());
  }
 }
 request.benchmark_selection.dataset = static_cast<data::BenchmarkDatasetVariant>(255);
 CHECK_FALSE(request.valid());
 request.benchmark_selection = {data::BenchmarkDatasetVariant::Coconut, static_cast<data::CoconutValidation>(255)};
 CHECK_FALSE(request.valid());
 request.kind = mmltk::controller::services::ArtifactCompileKind::Directory;
 CHECK(request.valid());
}
}  // namespace mmltk::controller::subsystems::system
namespace mmltk::controller::subsystems::system {
TEST_CASE("benchmark current transfer changes artifact activity at the exact image plateau", "[gui][services][progress]") {
 using namespace data::benchmark_internal;
 using Source = data::BenchmarkDatasetSource;
 const BenchmarkTraceSink quiet;
 domain::ArtifactProgress displayed;
 data::BenchmarkCompileProgress latest;
 ProgressReporter reporter(
  [&](const data::BenchmarkCompileProgress& update) {
   latest = update;
   displayed = project_artifact_progress(update);
  },
  quiet);
 reporter.phase(data::DatasetCompilePhase::Extracting);
 reporter.source_activity(Source::kCoco2017, "Old COCO activity");
 reporter.source_images(Source::kCoco2017, 123U, 123U);
 reporter.source_images(Source::kObjects365V2, 170161U, 408551U);
 reporter.source_images(Source::kOpenImagesV7, 56008U, 56008U);
 reporter.projected(1039610446176ULL);
 const auto check_plateau = [&] {
  CHECK(displayed.tracks.acquisition.active);
  CHECK(displayed.sources.size() == 3U);
  CHECK(displayed.projected_output_bytes == 1039610446176ULL);
  CHECK(latest.sources[1].completed_images == 170161U);
 };
 DownloadProgress update{"objects365-patch-17", 100U, 1000U, 1U};
 update.source = Source::kObjects365V2;
 for (const bool resumed : {false, true}) {
  for (const auto attempt : {1U, 2U}) {
   update.attempt = attempt;
   update.resumed = resumed;
   update.retained_bytes = resumed ? 64U : 0U;
   update.cache_hit = false;
   update.total_bytes = 1000U;
   update.completed_bytes = 100U;
   reporter.transfers().update(update, reporter);
   check_plateau();
   CHECK(displayed.activity.find("Objects365 v2") != std::string::npos);
   CHECK(displayed.activity.find("objects365-patch-17") != std::string::npos);
   CHECK(displayed.activity.find("100 / 1000 bytes") != std::string::npos);
   CHECK(displayed.activity.find("attempt " + std::to_string(attempt)) != std::string::npos);
   if (resumed) { CHECK(displayed.activity.find("retained 64 bytes") != std::string::npos); }
   const auto prior = displayed.activity;
   update.completed_bytes = 200U;
   reporter.transfers().update(update, reporter);
   check_plateau();
   CHECK(displayed.activity != prior);
   CHECK(displayed.activity.find("200 / 1000 bytes") != std::string::npos);
  }
 }
 update = DownloadProgress{"objects365-invalidated-archive", 16U, 1000U, 1U};
 update.source = Source::kObjects365V2;
 update.redownload = true;
 reporter.transfers().update(update, reporter);
 CHECK(displayed.activity.find("Re-downloading objects365-invalidated-archive") != std::string::npos);
 CHECK(displayed.activity.find("attempt 1") != std::string::npos);
 check_plateau();
 update = DownloadProgress{"objects365-cached-patch", 5000U, 5000U, 0U, false, true};
 update.source = Source::kObjects365V2;
 reporter.transfers().update(update, reporter);
 CHECK(displayed.activity.find("bytes reused") != std::string::npos);
 update = DownloadProgress{"objects365-live-patch", 32U, 0U, 1U};
 update.source = Source::kObjects365V2;
 reporter.transfers().update(update, reporter);
 CHECK(displayed.activity.find("32 bytes (total unknown)") != std::string::npos);
 CHECK(displayed.activity.find("reused") == std::string::npos);
 check_plateau();
 for (const auto phase : {DownloadProgressPhase::kVerifyingCachedArtifact, DownloadProgressPhase::kVerifyingDownloadedArtifact}) {
  update.phase = phase;
  reporter.transfers().update(update, reporter);
  CHECK(displayed.activity.find(phase == DownloadProgressPhase::kVerifyingCachedArtifact ? "Verifying cached" : "Verifying downloaded") != std::string::npos);
  check_plateau();
 }
 reporter.activity("Preparing labels");
 CHECK(displayed.activity == "Preparing labels");
 reporter.phase(data::DatasetCompilePhase::Labels);
 reporter.source_images(Source::kCoco2017, 123U, 123U);
 CHECK(displayed.activity == "Preparing compiled labels");
 reporter.transfers().update(update, reporter);
 CHECK(displayed.activity == "Preparing compiled labels");
 reporter.source_complete(Source::kObjects365V2, false);
 reporter.pixels(0U, 20U);
 reporter.source_activity(Source::kObjects365V2, "Repairing patch-17");
 CHECK(displayed.activity.find("Repairing patch-17") != std::string::npos);
 reporter.pixels(0U, 20U);
 CHECK(displayed.tracks.pixels.active);
 CHECK(displayed.tracks.pixels.total == 20U);
 reporter.source_activity(Source::kObjects365V2, std::string(4096U, 'x'));
 CHECK(displayed.activity.size() <= domain::kArtifactProgressTextCapacity);
}
}  // namespace mmltk::controller::subsystems::system
namespace mmltk::controller::subsystems::system {
TEST_CASE("real ranged HTTP restart reaches the bounded artifact activity as a re-download", "[gui][services][progress]") {
 using namespace data::benchmark_internal;
 mmltk::testsupport::ScopedTempDir root("artifact-range-restart");
 std::vector<std::uint8_t> payload(4096U);
 for (std::size_t i = 0U; i < payload.size(); ++i) { payload[i] = static_cast<std::uint8_t>(i % 251U); }
 data::testsupport::HttpServer server(payload);
 DownloadRequest request{
  "objects365-restart", server.url("restart"), root.path() / "archive.bin", root.path() / "archive.lock", payload.size(), mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(payload)), 1U};
 request.source = data::BenchmarkDatasetSource::kObjects365V2;
 constexpr std::uint64_t retained = 512U;
 {
  std::ofstream partial(request.destination.string() + ".part", std::ios::binary);
  partial.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(retained));
  REQUIRE(partial.good());
 }
 write_json_atomically(request.destination.string() + ".part.json", {{"schema_version", kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", "\"benchmark-test-etag\""}}, {});
 server.RestartNextRangedTransfer();
 std::vector<DownloadProgress> observed;
 std::vector<domain::ArtifactProgress> displayed;
 bool traced_restart = false;
 bool traced_completion = false;
 const auto trace = make_trace_sink([&](const std::string_view event, const std::string_view fields) {
  const auto value = nlohmann::json::parse(fields);
  if (event == "benchmark.download.progress" && value.at("redownload").get<bool>()) { traced_restart = true; }
  if (event == "benchmark.download.complete") { traced_completion = value.at("redownload").get<bool>(); }
 });
 ProgressReporter reporter([&](const auto& update) { displayed.push_back(project_artifact_progress(update)); }, trace);
 ArtifactProgressTotals totals;
 reporter.phase(data::DatasetCompilePhase::Extracting);
 reporter.source_images(data::BenchmarkDatasetSource::kCoco2017, 123U, 123U);
 reporter.source_images(data::BenchmarkDatasetSource::kObjects365V2, 170161U, 408551U);
 reporter.source_images(data::BenchmarkDatasetSource::kOpenImagesV7, 56008U, 56008U);
 displayed.clear();
 const auto completed = download_artifacts(
  {request}, 1U, {},
  [&](const auto& update) {
   observed.push_back(update);
   totals.update(update, reporter);
  },
  trace);
 REQUIRE(completed.size() == 1U);
 REQUIRE(observed.size() == displayed.size());
 bool saw_retained = false;
 bool saw_restart = false;
 for (std::size_t i = 0U; i < observed.size(); ++i) {
  const auto& update = observed[i];
  CHECK(displayed[i].valid());
  CHECK(displayed[i].tracks.acquisition.completed == update.completed_bytes);
  CHECK(displayed[i].tracks.acquisition.total == update.total_bytes);
  CHECK(displayed[i].sources[1].completed_images == 170161U);
  CHECK(displayed[i].activity.size() <= domain::kArtifactProgressTextCapacity);
  if (update.resumed) {
   CHECK_FALSE(saw_restart);
   saw_retained = true;
   CHECK(update.retained_bytes == retained);
   CHECK(displayed[i].activity.find("Resuming objects365-restart") != std::string::npos);
  }
  if (update.redownload) {
   CHECK(saw_retained);
   saw_restart = true;
   CHECK_FALSE(update.resumed);
   CHECK(update.retained_bytes == 0U);
   CHECK(displayed[i].activity.find("Re-downloading objects365-restart") != std::string::npos);
  }
 }
 CHECK(saw_retained);
 CHECK(saw_restart);
 CHECK(traced_restart);
 CHECK(traced_completion);
 CHECK(mmltk::common::io::sha256_file(request.destination) == mmltk::common::io::sha256_bytes(payload));
 CHECK_FALSE(completed.front().identity.empty());
 CHECK(read_json_file(request.destination.string() + ".download.json").at("identity") == completed.front().identity);
 const auto requests_before = server.requests();
 const auto cached = download_artifacts({request}, 1U, {}, [&](const auto& update) {
  CHECK(update.cache_hit);
  CHECK_FALSE(update.redownload);
  totals.update(update, reporter);
 });
 CHECK(cached.front().cache_hit);
 CHECK(cached.front().identity == completed.front().identity);
 CHECK(displayed.back().activity.find("bytes reused") != std::string::npos);
 CHECK(server.requests() == requests_before);
 server.Check();
}
}  // namespace mmltk::controller::subsystems::system
namespace mmltk::controller::subsystems::system {
TEST_CASE("real unknown metadata bytes remain open ended through artifact projection", "[gui][services][progress]") {
 using namespace data::benchmark_internal;
 using Source = data::BenchmarkDatasetSource;
 mmltk::testsupport::ScopedTempDir root("artifact-unknown-metadata");
 const std::vector<std::uint8_t> payload(1024U * 1024U, 37U);
 data::testsupport::HttpServer server(payload);
 server.OmitContentLength();
 server.GateNextTransfer();
 bool mixed = false;
 auto source = Source::kCoco2017;
 SECTION("unknown only") {}
 SECTION("mixed artifacts in one source") { mixed = true; }
 SECTION("mixed artifacts across sources") {
  mixed = true;
  source = Source::kObjects365V2;
 }
 std::vector<data::BenchmarkCompileProgress> native;
 std::vector<domain::ArtifactProgress> displayed;
 const BenchmarkTraceSink trace;
 ProgressReporter reporter(
  [&](const auto& update) {
   native.push_back(update);
   displayed.push_back(project_artifact_progress(update));
  },
  trace);
 ArtifactProgressTotals totals;
 reporter.phase(data::DatasetCompilePhase::Downloading);
 if (mixed) {
  DownloadRequest known{"coco-known", server.url("known"), root.path() / "known.bin", root.path() / "known.lock", payload.size(), {}, 1U};
  // Preseeded artifact admission takes the ordinary cache path, with no network request.
  mmltk::testsupport::write_binary_file(known.destination, payload);
  (void)download_artifacts({known}, 1U, {}, [&](const auto& update) { totals.update(update, reporter); });
  REQUIRE(server.requests() == 0U);
 }
 const DownloadRequest unknown{"metadata-unknown", server.url("unknown"), root.path() / "unknown.bin", root.path() / "unknown.lock", 0U, {}, 1U, false, source};
 std::promise<void> open_ended;
 auto observed = open_ended.get_future();
 bool notified = false;
 auto transfer = std::async(std::launch::async, [&] {
  return download_artifacts({unknown}, 1U, {}, [&](const auto& update) {
   totals.update(update, reporter);
   if (!notified && update.completed_bytes > 0U && update.total_bytes == 0U) {
    notified = true;
    open_ended.set_value();
   }
  });
 });
 const mmltk::testsupport::ScopedTestCleanup settle([&] { server.ReleasePartial(); });
 REQUIRE(server.WaitPartial());
 mmltk::testsupport::await_test_future(observed, "positive unknown-length metadata progress");
 server.ReleasePartial();
 const auto downloaded = mmltk::testsupport::await_test_future(transfer, "unknown-length metadata completion");
 REQUIRE(downloaded.size() == 1U);
 CHECK(downloaded.front().size == payload.size());
 REQUIRE(displayed.size() == native.size());
 bool saw_open_ended = false;
 for (std::size_t i = 0U; i < native.size(); ++i) {
  CHECK(displayed[i].valid());
  CHECK(displayed[i].completed == native[i].completed);
  CHECK(displayed[i].total == native[i].total);
  std::uint64_t completed = 0U;
  std::uint64_t total = 0U;
  bool known = true;
  for (const auto& contribution : native[i].sources) {
   completed += contribution.completed_bytes;
   total += contribution.total_bytes;
   known = known && contribution.byte_total_known;
  }
  CHECK(native[i].completed == completed);
  CHECK(native[i].total == (known ? total : 0U));
  if (!known && native[i].completed > (mixed ? payload.size() : 0U)) {
   saw_open_ended = true;
   CHECK(displayed[i].activity.contains("total unknown"));
  }
 }
 CHECK(saw_open_ended);
 CHECK(displayed.back().completed == payload.size() * (mixed ? 2U : 1U));
 CHECK(displayed.back().total == displayed.back().completed);
 CHECK(mmltk::common::io::sha256_file(unknown.destination) == mmltk::common::io::sha256_bytes(payload));
 CHECK(read_json_file(unknown.destination.string() + ".download.json").at("identity") == downloaded.front().identity);
 server.Check();
}
}  // namespace mmltk::controller::subsystems::system
