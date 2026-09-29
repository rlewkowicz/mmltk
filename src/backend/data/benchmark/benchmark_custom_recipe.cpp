#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_recipe.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_annotation_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/io/file_digest.h"
#include "src/pch_std.h"
#include <thread>
#include "src/common/system/cpu_affinity.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::string combined_artifact_digest(const std::string& left, const std::string& right) {
 const mmltk::common::io::Sha256Digest left_digest = mmltk::common::io::parse_sha256_hex(left);
 const mmltk::common::io::Sha256Digest right_digest = mmltk::common::io::parse_sha256_hex(right);
 std::array<std::uint8_t, 64> identity{};
 std::ranges::copy(left_digest, identity.begin());
 std::ranges::copy(right_digest, identity.begin() + left_digest.size());
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(identity));
}
}  // namespace
BenchmarkResources custom_annotation_resources() {
 // Protect one complete independent source continuation before image leases.
 // Other metadata sources enter as actual capacity becomes available.
 const auto source = coco_annotation_resources();
 return BenchmarkResources::handles(0, true, source.descriptors + source.continuation_descriptors);
}
CustomRecipeCatalog custom_recipe_catalog() {
 return {
  coco_annotations_artifact(), coco_train_images_artifact(), coco_val_images_artifact(), objects365_annotations_artifact(), open_images_boxes_artifact(), open_images_classes_artifact(),
  objects365_train_image_artifacts()
 };
}
CustomRecipePreparation prepare_custom_recipe(const BenchmarkCompilerConfig&, const BenchmarkCacheLayout& cache, const CustomRecipeCatalog& catalog, ProgressReporter& progress,
 std::size_t effective_num_workers, mmltk::common::concurrency::CancellationObservation external_cancellation, const BenchmarkTraceSink& trace, std::span<const int> worker_cpus, BenchmarkCompilePipeline* execution,
 BenchmarkAllowance preparation) {
 struct SourceCancellation {
  mmltk::common::concurrency::CancellationObservation external;
  std::atomic<bool> stopped{false};
  bool cancelled() const noexcept { return stopped.load(std::memory_order_relaxed) || external.requested(); }
 } source_cancellation{external_cancellation};
 const auto cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(source_cancellation);
 const auto parse_workers = execution ? execution->workers() : std::max<std::size_t>(1, effective_num_workers);
 // One fixed transport promise precedes all independently admitted lifecycles.
 auto source_transport = execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, cancel_requested, coco_annotation_resources()) : nullptr;
 StorageReservationPool storage(cache.root, trace, execution ? &execution->storage() : nullptr);
 const auto objects_index_path = cache.source_indexes("objects365") / "train.normalized.bin";
 const auto open_images_index_path = cache.source_indexes("open-images") / "train.normalized.bin";
 std::optional<CocoAnnotationIndexes> coco_indexes_result;
 std::optional<NormalizedAnnotationIndex> objects, open_images;
 bool objects_index_cache_hit = false, open_images_index_cache_hit = false;
 constexpr std::uint64_t kIndexCount = 6;
 std::atomic<std::uint64_t> completed_indexes{0};
 progress.phase(DatasetCompilePhase::Indexing, 0, kIndexCount);
 const auto objects_annotation_request = make_download_request(cache, "objects365", catalog.objects_annotations);
 const auto open_images_boxes_request = make_download_request(cache, "open-images", catalog.open_images_boxes);
 const auto open_images_classes_request = make_download_request(cache, "open-images", catalog.open_images_classes);
 std::unordered_map<std::string, DownloadResult> annotation_downloads;
 for (const auto& artifact : {catalog.coco_annotations, catalog.objects_annotations, catalog.open_images_boxes, catalog.open_images_classes}) annotation_downloads.emplace(artifact.artifact_id, DownloadResult{});
 const auto annotation_parse_options = [&](BenchmarkDatasetSource source, std::string split, std::uint32_t expected, bool keep, const BenchmarkAllowance& input_allowance) {
  return AnnotationParseOptions{source, std::move(split), expected, static_cast<int>(parse_workers), keep, cancel_requested, trace, execution, input_allowance};
 };
 const auto acquire_lifecycle = [&](const std::string& name) {
  return ArtifactLease::acquire_charged(cache.locks / name, cancel_requested, execution, coco_annotation_resources(), preparation);
 };
 const auto download_source = [&](const std::vector<DownloadRequest>& requests, const BenchmarkAllowance& allowance) {
  progress.phase(DatasetCompilePhase::Downloading);
  progress.source_activity(requests.front().source, "Downloading annotation metadata");
  std::uint64_t bytes = 0;
  for (const auto& request : requests) bytes = common_math::checked_add(bytes, request.expected_size, "benchmark annotation storage estimate overflow");
  const bool archive = std::ranges::any_of(requests, [](const DownloadRequest& request) { return request.source != BenchmarkDatasetSource::kOpenImagesV7; });
  require_storage(cache.root, archive ? common_math::checked_add(bytes, kArchiveScratchBytes, "benchmark annotation storage estimate overflow") : bytes, "benchmark annotation acquisition and indexing", trace);
  auto results = download_artifacts(requests, parse_workers, cancel_requested,
   progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress.transfers().update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution, allowance, &storage);
  for (std::size_t i = 0; i < requests.size(); ++i) annotation_downloads.at(requests[i].artifact_id) = std::move(results[i]);
 };
 const auto repair_annotations = [&](BenchmarkDatasetSource source, const std::vector<DownloadRequest>& requests, std::string_view reason, const BenchmarkAllowance& allowance) {
  auto results = repair_annotation_artifacts(requests, source, reason, progress, progress.transfers(), parse_workers, cancel_requested, trace, execution, allowance, &storage);
  for (std::size_t i = 0; i < requests.size(); ++i) annotation_downloads.at(requests[i].artifact_id) = std::move(results[i]);
 };
 const auto prepare_coco = [&] {
  auto lease = acquire_lifecycle("coco-annotations.lifecycle.lock");
  CocoAnnotationCache coco_cache(cache, catalog.coco_annotations, CocoAnnotationRequest{CocoSplitAdmission::Required, CocoSplitAdmission::Required},
   catalog.coco_train_images_count, catalog.coco_validation_images_count, static_cast<int>(parse_workers), cancel_requested, trace, execution, &storage, lease);
  coco_cache.discover(progress);
  auto completed = coco_cache.completed_indexes(); completed_indexes.fetch_add(completed);
  if (coco_cache.pending_download()) {
   download_source({*coco_cache.pending_download()}, lease->allowance());
   const auto before = completed;
   coco_cache.settle(std::move(annotation_downloads.at(catalog.coco_annotations.artifact_id)), progress, parse_workers, completed, kIndexCount);
   completed_indexes.fetch_add(completed - before);
  }
  coco_indexes_result = coco_cache.finish();
 };
 const auto prepare_objects = [&] {
  auto lease = acquire_lifecycle("objects365-annotations.lifecycle.lock");
  const auto& lifecycle_allowance = lease->allowance();
  progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Validating cached Objects365 annotation index");
  objects = discover_cached_index(objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", cancel_requested, trace);
  objects_index_cache_hit = objects.has_value();
  if (objects) ++completed_indexes;
  else download_source({objects_annotation_request}, lifecycle_allowance);
  if (!objects) {
   retry_annotation_indexing(cancel_requested, [&] {
    const DownloadResult& annotation_archive = annotation_downloads.at(catalog.objects_annotations.artifact_id);
    const std::filesystem::path extracted_dir = cache.source_indexes("objects365") / "source-json";
    std::filesystem::create_directories(extracted_dir);
    const std::filesystem::path json_path = extracted_dir / "zhiyuan_objv2_train.json";
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Extracting Objects365 train annotations");
    const std::string annotation_digest =
     extract_archive_member(annotation_archive.path, "zhiyuan_objv2_train.json", json_path, annotation_archive.identity, cache.locks / "objects365-train-json.extract.lock", cancel_requested, trace, &storage, execution, lifecycle_allowance);
    objects = load_or_build_index(cache, objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", annotation_digest, cancel_requested, trace, [&](const BenchmarkAllowance& input_allowance) {
     progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Parsing and indexing Objects365 annotations");
     return parse_coco_style_annotations(json_path, annotation_digest, objects365_category_mappings(), annotation_parse_options(BenchmarkDatasetSource::kObjects365V2, "train", 0U, false, input_allowance));
    }, &storage, execution, lifecycle_allowance);
    ++completed_indexes;
    progress.phase(DatasetCompilePhase::Indexing, completed_indexes.load(), kIndexCount);
   }, [&](const std::exception& error) {
    const std::filesystem::path json_path = cache.source_indexes("objects365") / "source-json" / "zhiyuan_objv2_train.json";
    remove_cache_path(json_path.string() + ".extract.json");
    remove_cache_path(json_path);
    remove_normalized_annotation_index(objects_index_path);
    repair_annotations(BenchmarkDatasetSource::kObjects365V2, {objects_annotation_request}, error.what(), lifecycle_allowance);
   });
  }
 };
 const auto prepare_open_images = [&] {
  auto lease = acquire_lifecycle("open-images-annotations.lifecycle.lock");
  const auto& lifecycle_allowance = lease->allowance();
  progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Validating cached Open Images annotation index");
  open_images = discover_cached_index(open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", cancel_requested, trace);
  open_images_index_cache_hit = open_images.has_value();
  if (open_images) ++completed_indexes;
  else download_source({open_images_boxes_request, open_images_classes_request}, lifecycle_allowance);
  if (!open_images) {
   retry_annotation_indexing(cancel_requested, [&] {
    const DownloadResult& boxes = annotation_downloads.at(catalog.open_images_boxes.artifact_id);
    const DownloadResult& classes = annotation_downloads.at(catalog.open_images_classes.artifact_id);
    const std::string annotation_identity = combined_artifact_digest(boxes.identity, classes.identity);
    open_images = load_or_build_index(cache, open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", annotation_identity, cancel_requested, trace, [&](const BenchmarkAllowance& input_allowance) {
     progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Parsing and indexing Open Images annotations");
     return parse_open_images_annotations(
      boxes.path, classes.path, annotation_identity, open_images_category_mappings(), annotation_parse_options(BenchmarkDatasetSource::kOpenImagesV7, "train", 0U, false, input_allowance));
    }, &storage, execution, lifecycle_allowance);
    ++completed_indexes;
    progress.phase(DatasetCompilePhase::Indexing, completed_indexes.load(), kIndexCount);
   }, [&](const std::exception& error) {
    remove_normalized_annotation_index(open_images_index_path);
    repair_annotations(BenchmarkDatasetSource::kOpenImagesV7, {open_images_boxes_request, open_images_classes_request}, error.what(), lifecycle_allowance);
   });
  }
 };
 const auto cpus = worker_cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(worker_cpus.begin(), worker_cpus.end());
 std::exception_ptr failure;
 std::mutex failure_mutex;
 // These three source controllers wait on locks/I/O independently even with a
 // single compile CPU; every parser uses that shared CPU owner.
 std::vector<std::jthread> controllers;
 struct RetireSources {
  SourceCancellation& cancellation;
  std::vector<std::jthread>& controllers;
  ~RetireSources() { cancellation.stopped.store(true, std::memory_order_relaxed); controllers.clear(); }
 } retire_sources{source_cancellation, controllers};
 for (int task = 0; task < 3; ++task) controllers.emplace_back([&, task] {
  try {
   mmltk::common::system::set_thread_affinity(cpus);
   switch (task) { case 0: prepare_coco(); break; case 1: prepare_objects(); break; default: prepare_open_images(); break; }
  } catch (...) { const std::lock_guard lock(failure_mutex); if (!failure) failure = std::current_exception(); source_cancellation.stopped.store(true, std::memory_order_relaxed); }
 });
 controllers.clear();
 if (failure) std::rethrow_exception(failure);
 auto coco_indexes = std::move(*coco_indexes_result);
 auto coco_train_index_path = std::move(coco_indexes.train_path);
 auto coco_val_index_path = std::move(coco_indexes.validation_path);
 auto coco_train = std::move(coco_indexes.train);
 auto coco_val = std::move(coco_indexes.validation);
 const bool coco_indexes_cache_hit = coco_indexes.cache_hit;
 if (!coco_train || !coco_val || !objects || !open_images) { throw std::runtime_error("benchmark normalized annotation indexing did not complete"); }
 if (coco_val->images.size() != catalog.coco_validation_images_count) { throw std::runtime_error("COCO val2017 normalized index must contain exactly 5,000 images"); }
 {
  progress.activity("Checking COCO train and validation metadata reuse");
  std::unordered_set<std::uint64_t> validation_ids;
  validation_ids.reserve(coco_val->images.size());
  for (const NormalizedImage& image : coco_val->images) { validation_ids.emplace(image.source_image_id); }
  for (const NormalizedImage& image : coco_train->images) {
   if (validation_ids.contains(image.source_image_id)) { throw std::runtime_error("COCO train and validation metadata reuse an image ID"); }
  }
  trace_benchmark_event(
   trace, "benchmark.split_reuse.metadata_check", [&] { return nlohmann::json{{"coco_train_images", coco_train->images.size()}, {"coco_val_images", coco_val->images.size()}, {"overlap", 0}}; });
 }
 progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Selecting byte-efficient Objects365 shards and balanced images");
 const std::vector<CatalogArtifact> sampling_object_artifacts = catalog.objects_images;
 std::vector<std::uint64_t> object_shard_bytes;
 object_shard_bytes.reserve(sampling_object_artifacts.size());
 for (const CatalogArtifact& artifact : sampling_object_artifacts) { object_shard_bytes.push_back(artifact.expected_size); }
 CombinedSupplementalSamplingResult combined_sampling = sample_combined_supplemental_indices(*coco_train, *objects, *open_images, object_shard_bytes, cancel_requested, execution);
 SupplementalSamplingResult objects_sampling = std::move(combined_sampling.objects365);
 SupplementalSamplingResult open_images_sampling = std::move(combined_sampling.open_images);
 progress.phase(DatasetCompilePhase::Indexing, ++completed_indexes, kIndexCount);
 progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Selecting Open Images class-deficit and diversity sample");
 progress.phase(DatasetCompilePhase::Indexing, ++completed_indexes, kIndexCount);
 const auto trace_sampling = [&](const SupplementalSamplingStats& stats, const BenchmarkDatasetSource source) {
  trace_benchmark_event(trace, "benchmark.sampling.complete", [&] {
   return nlohmann::json{
    {"source", benchmark_source_name(source)},
    {"revision", kSupplementalSamplingRevision},
    {"full_images", stats.full_images},
    {"full_boxes", stats.full_boxes},
    {"selected_images", stats.selected_images},
    {"selected_boxes", stats.selected_boxes},
    {"available_class_images", stats.available_class_images},
    {"selected_class_images", stats.selected_class_images},
   };
  });
 };
 trace_sampling(objects_sampling.stats, BenchmarkDatasetSource::kObjects365V2);
 trace_sampling(open_images_sampling.stats, BenchmarkDatasetSource::kOpenImagesV7);
 trace_benchmark_event(trace, "benchmark.sampling.source_mix", [&] {
  return nlohmann::json{
   {"target_images", combined_sampling.target_images}, {"objects365_images", objects_sampling.stats.selected_images}, {"open_images_images", open_images_sampling.stats.selected_images},
   {"open_images_floor", combined_sampling.open_images_floor}, {"open_images_ceiling", combined_sampling.open_images_ceiling}, {"objects365_shards", combined_sampling.objects365_shards},
   {"objects365_archive_bytes", combined_sampling.objects365_archive_bytes}
  };
 });
 progress.activity("Finalizing normalized annotation cache");
 return CustomRecipePreparation{
  std::move(coco_train_index_path), std::move(coco_val_index_path), std::move(objects_index_path), std::move(open_images_index_path), NormalizedAnnotationReadView(std::move(*coco_train)), NormalizedAnnotationReadView(std::move(*coco_val)), objects_sampling.view,
  open_images_sampling.view, std::move(coco_indexes_cache_hit), std::move(objects_index_cache_hit), std::move(open_images_index_cache_hit), std::move(combined_sampling), std::move(objects_sampling),
  std::move(open_images_sampling), std::move(sampling_object_artifacts)
 };
}
}  // namespace mmltk::backend::data::benchmark_internal
