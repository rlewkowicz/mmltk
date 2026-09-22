#include "src/controller/services/artifact_store.h"
#include <curl/curl.h>
#include <fcntl.h>
#include "src/common/io/file_digest.h"
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/staging_directory.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/runtime_paths.h"
#include "src/controller/contracts/gui_settings_mutation.h"
namespace mmltk::controller::services {
namespace contracts = mmltk::controller::contracts;
std::expected<ArtifactCompileRequest, ArtifactCompileMaterializationError> materialize_artifact_compile(const mmltk::controller::contracts::GuiSettingsState& settings) noexcept {
 const auto refusal = [](const std::string_view detail) { return std::unexpected{ArtifactCompileMaterializationError{.detail = mmltk::controller::contracts::bounded_artifact_detail(detail)}}; };
 if (!mmltk::controller::contracts::gui_settings_valid(settings)) return refusal("invalid settings");
 const auto& train = settings.workflows.train;
 ArtifactCompileRequest request{.source = train.dataset_source_dir,
  .output = train.compiled_dataset_dir,
  .preset = train.request.preset_name,
  .resolution = static_cast<std::uint32_t>(train.request.resolution),
  .overwrite = train.overwrite_compiled_dataset,
  .benchmark_selection = train.benchmark_selection,
  .perceptual_downscale = train.compile_perceptual_downscale,
  .resize_mode = train.compile_resize_mode};
 request.kind = train.compile_benchmark_dataset_override ? ArtifactCompileKind::Benchmark : ArtifactCompileKind::Directory;
 if (!request.valid()) return refusal("invalid dataset compile settings");
 return request;
}
namespace {
[[nodiscard]] std::uint64_t bounded_u64_count(const std::size_t value) noexcept {
 return static_cast<std::uint64_t>(std::min(value, static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())));
}
[[nodiscard]] std::string bounded_progress_text(const std::string_view value) { return std::string{value.substr(0U, contracts::kArtifactProgressTextCapacity)}; }
}  // namespace
contracts::ArtifactProgress project_artifact_progress(const mmltk::backend::data::CompileProgress& value) {
 return {.phase = value.phase,
  .activity = std::string{mmltk::backend::data::dataset_compile_phase_label(value.phase)},
  .completed = bounded_u64_count(value.done),
  .total = bounded_u64_count(value.total),
  .elapsed_seconds = value.elapsed_seconds,
  .remaining_seconds = value.remaining_seconds,
  .throughput_per_second = value.throughput_per_second,
  .dropped_instances = value.dropped_instances};
}
contracts::ArtifactProgress project_artifact_progress(const mmltk::backend::data::BenchmarkCompileProgress& value) {
 std::string activity = value.activity;
 for (const auto& source : value.sources) {
  if (value.current_source == source.source) {
   using Source = mmltk::backend::data::BenchmarkDatasetSource;
   const std::string_view source_name = [source = source.source] {
    switch (source) {
     case Source::kCoco2017: return std::string_view{"COCO 2017"};
     case Source::kObjects365V2: return std::string_view{"Objects365 v2"};
     case Source::kOpenImagesV7: return std::string_view{"Open Images v7"};
     case Source::kCoconut: return std::string_view{"COCONut"};
     case Source::kObjects365V1: return std::string_view{"Objects365 v1"};
    }
    std::unreachable();
   }();
   activity = std::string{source_name} + " · " + mmltk::backend::data::format_benchmark_source_status(source, activity);
   break;
  }
 }
 const mmltk::backend::data::ProgressEstimate estimate = mmltk::backend::data::estimate_progress(value.completed, value.total, value.activity_elapsed_seconds);
 return {.phase = value.phase,
  .activity = bounded_progress_text(activity),
  .completed = value.completed,
  .total = value.total,
  .elapsed_seconds = value.activity_elapsed_seconds,
  .remaining_seconds = estimate.remaining_seconds,
  .throughput_per_second = estimate.throughput_per_second,
  .projected_output_bytes = value.projected_output_bytes,
  .dropped_instances = value.dropped_instances,
  .quarantined_images = value.quarantined_images};
}
namespace {
class RuntimeArtifactCompilerOperations final : public ArtifactCompilerOperations {
private:
 void compile_benchmark(const mmltk::backend::data::BenchmarkDatasetSelection selection, const std::filesystem::path& output, const std::filesystem::path& publication, const std::uint32_t resolution,
  const bool perceptual_downscale, const mmltk::backend::imaging::resample::ImageResizeMode resize_mode, const mmltk::common::concurrency::CancellationObservation cancellation,
  const ArtifactProgressObserver progress, const ArtifactBenchmarkTraceObserver trace) const override {
  mmltk::backend::data::BenchmarkCompilerConfig configuration;
  configuration.selection = selection;
  configuration.output_dir = output;
  configuration.publication_dir = publication;
  configuration.resolution = resolution;
  configuration.perceptual_downscale = perceptual_downscale;
  configuration.resize_mode = resize_mode;
  configuration.overwrite = true;
  configuration.cancel_requested = cancellation;
  if (progress.report != nullptr) {
   configuration.progress = [progress](const mmltk::backend::data::BenchmarkCompileProgress& update) { progress(project_artifact_progress(update)); };
  }
  if (trace.report != nullptr) {
   configuration.trace = [trace](const std::string_view event, const std::string_view json_fields) noexcept { trace(event, json_fields); };
  }
  mmltk::backend::data::compile_benchmark_dataset(std::move(configuration));
 }
 void compile_directory(const std::filesystem::path& source, const std::filesystem::path& output, const std::uint32_t resolution, const bool perceptual_downscale,
  const mmltk::backend::imaging::resample::ImageResizeMode resize_mode, const mmltk::common::concurrency::CancellationObservation cancellation,
  const ArtifactProgressObserver progress) const override {
  std::array<std::string, contracts::kArtifactSplitCapacity> split_names{};
  std::size_t split_count = 0U;
  if (cancellation.requested()) return;
  for (const auto& entry : std::filesystem::directory_iterator(source)) {
   if (cancellation.requested()) return;
   if (!entry.is_directory()) continue;
   if (cancellation.requested()) return;
   if (split_count == split_names.size()) throw std::runtime_error("artifact source split count is outside the fixed protocol capacity");
   split_names[split_count++] = entry.path().filename().string();
  }
  if (cancellation.requested()) return;
  if (split_count == 0U) throw std::runtime_error("artifact source split count is outside the fixed protocol capacity");
  const std::span populated_splits{split_names.data(), split_count};
  std::ranges::sort(populated_splits);
  if (cancellation.requested()) return;
  std::vector<std::string> splits;
  splits.reserve(split_count);
  splits.insert(splits.end(), populated_splits.begin(), populated_splits.end());
  mmltk::backend::data::CompilerConfig configuration;
  configuration.source_dir = source.string();
  configuration.output_dir = output.string();
  configuration.perceptual_downscale = perceptual_downscale;
  configuration.resize_mode = resize_mode;
  configuration.target_width = resolution;
  configuration.target_height = resolution;
  configuration.worker_cpus = mmltk::common::system::allowed_cpu_set();
  auto plan = mmltk::backend::data::DatasetCompiler::prepare(std::move(configuration), std::move(splits), cancellation);
  for (std::size_t split = 0; split != plan.splits.size(); ++split) {
   if (progress.report == nullptr) {
    mmltk::backend::data::DatasetCompiler::compile(plan, split, nullptr, cancellation);
    continue;
   }
   mmltk::backend::data::CompileTelemetry telemetry{plan.splits[split].image_count,
    {.context = const_cast<ArtifactProgressObserver*>(&progress),
     .report = [](void* context, const mmltk::backend::data::CompileProgress& update) noexcept { (*static_cast<const ArtifactProgressObserver*>(context))(project_artifact_progress(update)); }}};
   mmltk::backend::data::DatasetCompiler::compile(plan, split, &telemetry, cancellation);
   if (cancellation.requested()) return;
  }
 }
};
const RuntimeArtifactCompilerOperations kRuntimeArtifactCompilerOperations;
[[nodiscard]] mmltk::controller::contracts::ArtifactInspection rejected_inspection(const std::string_view detail) {
 return {.compatible = false, .splits = {}, .detail = mmltk::controller::contracts::bounded_artifact_detail(detail)};
}
[[nodiscard]] std::optional<std::string_view> invalid_artifact_input(
 const std::string_view preset, const std::filesystem::path* const source = nullptr, const std::filesystem::path* const output = nullptr) {
 if (preset.empty() || preset.size() > contracts::kArtifactPresetCapacity) return "artifact preset exceeds reflected capacity";
 if (source != nullptr && (source->empty() || source->native().size() > contracts::kArtifactPathCapacity)) { return "artifact source path exceeds reflected capacity"; }
 if (output != nullptr && (output->empty() || output->native().size() > contracts::kArtifactPathCapacity)) { return "artifact output path exceeds reflected capacity"; }
 return std::nullopt;
}
[[nodiscard]] std::optional<std::string_view> invalid_inspected_split(const mmltk::backend::data::CompiledDatasetInfo& info) {
 const std::string path = info.path.string();
 if (path.empty() || path.size() > contracts::kArtifactPathCapacity) return "compiled artifact path exceeds the reflected protocol capacity";
 if (info.image_count == 0U || info.width == 0U || info.height == 0U || info.channels == 0U) { return "compiled artifact has invalid image geometry"; }
 if (!info.class_catalog || info.class_catalog->empty()) return "compiled artifact class catalog is empty";
 return std::nullopt;
}
[[nodiscard]] mmltk::controller::contracts::ArtifactInspection inspect_artifact(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, const std::string_view preset,
 const std::uint32_t resolution, const mmltk::common::concurrency::CancellationObservation cancellation) {
 mmltk::controller::contracts::ArtifactInspection result;
 if (const auto invalid = invalid_artifact_input(preset)) { return rejected_inspection(*invalid); }
 for (const auto& path : paths) {
  if (path.empty()) continue;
  if (const auto invalid = invalid_artifact_input(preset, &path)) { return rejected_inspection(*invalid); }
 }
 const auto* catalog = mmltk::backend::models::rfdetr::find_preset_catalog_entry(preset);
 if (catalog == nullptr) return rejected_inspection("unknown RF-DETR preset");
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> classes;
 for (const auto& path : paths) {
  if (path.empty()) continue;
  if (cancellation.requested()) return rejected_inspection("artifact inspection cancelled");
  try {
   const auto info = mmltk::backend::data::inspect_compiled_dataset(path);
   if (const auto invalid = invalid_inspected_split(info)) { return rejected_inspection(*invalid); }
   if (info.width != info.height || info.channels != 3U || (resolution != 0U && info.width != resolution)) {
    return rejected_inspection("compiled artifact does not match the configured square RGB resolution");
   }
   if (classes && !classes->ordered_equal(*info.class_catalog)) return rejected_inspection("compiled artifact class catalogs differ");
   classes = info.class_catalog;
   if (result.splits.size() >= contracts::kArtifactSplitCapacity) { return rejected_inspection("compiled artifact split count exceeds fixed capacity"); }
   auto& split = result.splits.emplace_back(mmltk::controller::contracts::ArtifactSplitFact{.path = info.path.string(),
    .image_count = static_cast<std::uint32_t>(info.image_count),
    .width = info.width,
    .height = info.height,
    .channels = info.channels,
    .max_instances_per_image = info.max_instances_per_image,
    .class_names = {}});
   for (const std::string& name : info.class_names()) split.class_names.push_back({.value = name});
  } catch (const std::exception& error) { return rejected_inspection(error.what()); }
 }
 if (result.splits.empty()) return rejected_inspection("compiled artifact inspection requires at least one path");
 result.compatible = true;
 return result;
}
[[nodiscard]] bool valid_weight_url(const std::string_view value) noexcept {
 constexpr std::string_view kHttps = "https://";
 if (!value.starts_with(kHttps) || value.size() <= kHttps.size() || value.size() > contracts::kArtifactPathCapacity) return false;
 return std::ranges::all_of(value, [](const unsigned char byte) { return !std::iscntrl(byte) && !std::isspace(byte); });
}
[[nodiscard]] bool valid_weight_md5(const std::string_view value) noexcept {
 return value.size() == 32U && std::ranges::all_of(value, [](const unsigned char byte) { return std::isxdigit(byte); });
}
[[nodiscard]] std::optional<std::filesystem::path> validated_weight_destination(const ArtifactWeightAsset& asset, const std::filesystem::path& cache_root) {
 const std::filesystem::path filename(asset.filename);
 if (asset.filename.empty() || asset.filename.size() > contracts::kArtifactPathCapacity || filename.is_absolute() || filename.has_parent_path() || filename == std::filesystem::path{"."} ||
     filename == std::filesystem::path{".."} || asset.filename.contains('/') || asset.filename.contains('\\') || !valid_weight_url(asset.url) || !valid_weight_md5(asset.md5)) {
  return std::nullopt;
 }
 const std::filesystem::path root = cache_root.lexically_normal();
 const std::filesystem::path destination = (root / filename).lexically_normal();
 if (destination.parent_path() != root) return std::nullopt;
 return destination;
}
struct WeightTransfer {
 std::ofstream output;
 const ArtifactCancellationToken* cancellation = nullptr;
 ArtifactWeightProgressObserver progress{};
 std::uint64_t reported_completed = 0U;
 std::uint64_t reported_total = 0U;
 bool reported_known = false;
 bool reported = false;
};
class RuntimeArtifactWeightOperations final : public ArtifactWeightOperations {
public:
 std::optional<ArtifactWeightAsset> find(const std::string_view preset) const override {
  const auto* entry = mmltk::backend::models::rfdetr::find_preset_catalog_entry(preset);
  if (entry == nullptr) return std::nullopt;
  return ArtifactWeightAsset{.filename = std::string(entry->canonical_weight_filename), .url = std::string(entry->canonical_weight_url), .md5 = std::string(entry->canonical_weight_md5)};
 }
 void download(const std::string_view url, const std::filesystem::path& output, const ArtifactCancellationToken& cancellation, ArtifactWeightProgressObserver) const override;
};
size_t write_weight(void* data, size_t size, size_t count, void* opaque) {
 auto& transfer = *static_cast<WeightTransfer*>(opaque);
 if (transfer.cancellation->cancelled()) return 0U;
 const std::size_t bytes = size * count;
 transfer.output.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
 return transfer.output ? bytes : 0U;
}
int weight_progress(void* opaque, const curl_off_t total, const curl_off_t completed, curl_off_t, curl_off_t) {
 constexpr std::uint64_t kProgressReportStride = 256U * 1024U;
 auto& transfer = *static_cast<WeightTransfer*>(opaque);
 if (transfer.cancellation->cancelled()) return 1;
 if (transfer.progress.report != nullptr) {
  const bool known = total > 0;
  const auto done = completed > 0 ? static_cast<std::uint64_t>(completed) : 0U;
  const auto expected = known ? static_cast<std::uint64_t>(total) : 0U;
  if (transfer.reported && transfer.reported_total == expected && transfer.reported_known == known) {
   if (transfer.reported_completed == done) return 0;
   const bool completed_transfer = known && done == expected;
   const bool advanced_enough = done < transfer.reported_completed || done - transfer.reported_completed >= kProgressReportStride;
   if (!completed_transfer && !advanced_enough) return 0;
  }
  transfer.reported = true;
  transfer.reported_completed = done;
  transfer.reported_total = expected;
  transfer.reported_known = known;
  transfer.progress({.stage = contracts::ModelProgressStage::Downloading, .activity = "Downloading model weights", .completed = done, .total = expected, .total_known = known});
 }
 return 0;
}
void RuntimeArtifactWeightOperations::download(
 const std::string_view url, const std::filesystem::path& output, const ArtifactCancellationToken& cancellation, const ArtifactWeightProgressObserver progress) const {
 WeightTransfer transfer{.output = std::ofstream(output, std::ios::binary | std::ios::trunc), .cancellation = &cancellation, .progress = progress};
 if (!transfer.output) throw std::runtime_error("cannot create staged canonical RF-DETR weights");
 CURL* curl = curl_easy_init();
 if (!curl) throw std::runtime_error("cannot initialize canonical weight download");
 const auto curl_cleanup = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>{curl, curl_easy_cleanup};
 const std::string owned_url(url);
 curl_easy_setopt(curl, CURLOPT_URL, owned_url.c_str());
 curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_weight);
 curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
 curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
 curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &weight_progress);
 curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
 curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
 curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
 const CURLcode result = curl_easy_perform(curl);
 transfer.output.close();
 if (result != CURLE_OK) throw std::runtime_error(cancellation.cancelled() ? "canonical weight acquisition cancelled" : std::string("canonical weight download failed: ") + curl_easy_strerror(result));
}
std::string md5_file(const std::filesystem::path& path, const ArtifactCancellationToken& cancellation) {
 const auto digests = mmltk::common::io::try_file_digests(path, true, [&] { return cancellation.cancelled(); });
 if (!digests) throw std::runtime_error("canonical weight acquisition cancelled");
 return digests->md5;
}
}  // namespace
bool ArtifactCompileRequest::valid() const noexcept {
 const auto* compile_source = kind == ArtifactCompileKind::Directory ? &source : nullptr;
 return !invalid_artifact_input(preset, compile_source, &output).has_value() && resolution != 0U && mmltk::frameworks::reflection::enum_contains(resize_mode) &&
        (kind == ArtifactCompileKind::Directory || mmltk::backend::data::valid_benchmark_selection(benchmark_selection));
}
ArtifactStore::ArtifactStore() : ArtifactStore(mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "weights") {}
ArtifactStore::ArtifactStore(std::filesystem::path cache_root)
    : ArtifactStore(std::move(cache_root), []() -> const ArtifactWeightOperations& {
       static const RuntimeArtifactWeightOperations operations;
       return operations;
      }()) {}
ArtifactStore::ArtifactStore(std::filesystem::path cache_root, const ArtifactWeightOperations& operations) : ArtifactStore(std::move(cache_root), operations, kRuntimeArtifactCompilerOperations) {}
ArtifactStore::ArtifactStore(std::filesystem::path cache_root, const ArtifactWeightOperations& operations, const ArtifactCompilerOperations& compiler_operations)
    : cache_root_(std::move(cache_root)), weight_operations_(&operations), compiler_operations_(&compiler_operations) {}
mmltk::controller::contracts::ArtifactInspection ArtifactStore::inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, const std::string_view preset,
 const std::uint32_t resolution, const ArtifactCancellationToken& cancellation) const {
 return inspect_artifact(paths, preset, resolution, mmltk::common::concurrency::CancellationObservation::Borrow(cancellation));
}
ArtifactCompileResult ArtifactStore::compile(
 const ArtifactCompileRequest& request, const ArtifactCancellationToken& cancellation, const ArtifactProgressObserver progress, const ArtifactDiagnosticObserver diagnostics) const {
 const auto cancellation_observation = mmltk::common::concurrency::CancellationObservation::Borrow(cancellation);
 if (!request.valid()) return {.output = {}, .cancelled = false, .inspection = rejected_inspection("invalid artifact compile request")};
 if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
 try {
  if (request.kind == ArtifactCompileKind::Directory && !std::filesystem::is_directory(request.source))
   return {.output = {}, .cancelled = false, .inspection = rejected_inspection("artifact source directory is unavailable")};
  if (!request.overwrite && std::filesystem::exists(request.output)) return {.output = {}, .cancelled = false, .inspection = rejected_inspection("artifact output already exists")};
  mmltk::common::io::StagingDirectory staging{request.output, "", ".tmp.XXXXXX", "failed to stage compiled artifact"};
  switch (request.kind) {
   case ArtifactCompileKind::Directory:
    compiler_operations_->compile_directory(request.source, staging.path(), request.resolution, request.perceptual_downscale, request.resize_mode, cancellation_observation, progress);
    break;
   case ArtifactCompileKind::Benchmark: {
    const ArtifactBenchmarkTraceObserver trace = diagnostics.benchmark;
    compiler_operations_->compile_benchmark(
     request.benchmark_selection, staging.path(), request.output, request.resolution, request.perceptual_downscale, request.resize_mode, cancellation_observation, progress, trace);
    break;
   }
  }
  if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
  std::array<std::filesystem::path, contracts::kArtifactSplitCapacity> paths{};
  std::size_t path_count = 0U;
  for (const auto& entry : std::filesystem::directory_iterator(staging.path())) {
   if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
   if (!entry.is_regular_file() || entry.path().extension() != ".bin") continue;
   if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
   if (path_count == paths.size()) return {.output = {}, .cancelled = false, .inspection = rejected_inspection("compiled artifact split count is outside fixed capacity")};
   paths[path_count++] = entry.path();
  }
  if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
  if (path_count == 0U) return {.output = {}, .cancelled = false, .inspection = rejected_inspection("compiled artifact split count is outside fixed capacity")};
  std::ranges::sort(std::span{paths.data(), path_count});
  if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
  mmltk::controller::contracts::ArtifactInspection inspection = inspect_artifact(paths, request.preset, request.resolution, cancellation_observation);
  if (!inspection.compatible) { return {.output = {}, .cancelled = cancellation_observation.requested(), .inspection = std::move(inspection)}; }
  // This is the publication linearization point: cancellation may stop
  // preparation and verification up to this check, while a completed
  // atomic publish owns the successful output.
  if (cancellation_observation.requested()) return {.output = {}, .cancelled = true, .inspection = {}};
  mmltk::common::io::publish_staged_path_atomically(staging.path(), request.output, request.overwrite);
  staging.published();
  for (auto& split : inspection.splits) { split.path = (request.output / std::filesystem::path{split.path}.filename()).string(); }
  return {.output = request.output, .cancelled = false, .inspection = std::move(inspection)};
 } catch (const std::exception& error) { return {.output = {}, .cancelled = cancellation_observation.requested(), .inspection = rejected_inspection(error.what())}; }
}
std::filesystem::path ArtifactStore::canonical_weight_path(const std::string_view preset) const {
 auto pair = ArtifactCancellationSource::Mint();
 return canonical_weight_path(preset, pair.second);
}
std::filesystem::path ArtifactStore::canonical_weight_path(const std::string_view preset, const ArtifactCancellationToken& cancellation, const ArtifactWeightProgressObserver progress) const {
 const auto require_not_cancelled = [&cancellation] {
  if (cancellation.cancelled()) throw std::runtime_error("canonical weight acquisition cancelled");
 };
 require_not_cancelled();
 if (const auto invalid = invalid_artifact_input(preset)) throw std::invalid_argument(std::string(*invalid));
 const auto asset = weight_operations_->find(preset);
 require_not_cancelled();
 if (!asset) throw std::invalid_argument("unknown RF-DETR preset");
 const auto destination = validated_weight_destination(*asset, cache_root_);
 if (!destination) throw std::invalid_argument("invalid canonical RF-DETR weight metadata");
 require_not_cancelled();
 if (progress.report != nullptr) progress({.stage = contracts::ModelProgressStage::InspectingCache, .activity = "Inspecting model cache"});
 if (std::filesystem::is_regular_file(*destination)) {
  require_not_cancelled();
  if (progress.report != nullptr) progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "Verifying cached model weights"});
  if (md5_file(*destination, cancellation) == asset->md5) {
   require_not_cancelled();
   return *destination;
  }
  require_not_cancelled();
  std::filesystem::remove(*destination);
 }
 require_not_cancelled();
 mmltk::common::io::StagingDirectory staging{*destination, ".", ".weights.XXXXXX", "failed to stage canonical RF-DETR weights"};
 const auto staged = staging.path() / asset->filename;
 require_not_cancelled();
 weight_operations_->download(asset->url, staged, cancellation, progress);
 require_not_cancelled();
 if (progress.report != nullptr) progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "Verifying downloaded model weights"});
 if (md5_file(staged, cancellation) != asset->md5) throw std::runtime_error("canonical RF-DETR weight checksum mismatch");
 require_not_cancelled();
 mmltk::common::io::publish_staged_path_atomically(staged, *destination, true);
 return *destination;
}
}  // namespace mmltk::controller::services
