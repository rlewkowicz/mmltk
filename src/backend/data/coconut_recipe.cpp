#include "detail/benchmark_recipe.h"
#include "detail/coconut_mask_recovery.h"
#include "detail/benchmark_annotation_cache.h"
#include "detail/benchmark_image_decoder.h"
#include "detail/benchmark_storage.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include <condition_variable>
#include <exception>
#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <tuple>
#include <iterator>
#include <atomic>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutReleaseInputs {
 std::unordered_map<std::string, DownloadResult> artifacts;
 std::map<std::pair<CoconutEdition, CoconutImageNamespace>, CoconutComponent> components;
};
// Opaque outside this owner. A release holds its lifecycle lease only until its
// own parser finishes, never while waiting for another release's lease.
struct CoconutRecipeInputs {
 struct Release {
  CoconutReleaseInputs inputs;
  std::optional<CoconutRecipePreparation> metadata, complete;
 };
 CoconutRecipeInputs(std::size_t count, mmltk::common::concurrency::CancellationObservation cancellation, std::function<void(std::exception_ptr)> failure)
     : releases(count), external(cancellation), failed(std::move(failure)) {
  ready.reserve(count);
 }
 ~CoconutRecipeInputs() {
  {
   const std::lock_guard lock(mutex);
   retiring = true;
   stopped.store(true, std::memory_order_relaxed);
  }
  changed.notify_all();
  pool.reset();
 }
 [[nodiscard]] bool cancelled() const noexcept { return stopped.load(std::memory_order_relaxed) || external.requested(); }
 void fail(std::exception_ptr value) {
  bool first = false;
  {
   const std::lock_guard lock(mutex);
   if (retiring) return;
   first = !error;
   if (first) error = value;
   stopped.store(true, std::memory_order_relaxed);
  }
  changed.notify_all();
  if (first && failed) failed(value);
 }
 void acquire_release(std::size_t index) {
  const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
  const auto& release = catalog->releases[index];
  auto lease = ArtifactLease::acquire(cache->locks / (std::string(release.name) + ".annotations.lifecycle.lock"), cancellation);
  for (const auto& artifact : release.annotations) {
   const auto request = make_download_request(*cache, "coconut-" + std::string(release.name), artifact);
   const auto reservation = reservations->reserve(additional_download_bytes(request.destination, request.expected_size), "additional COCONut annotation download bytes");
   auto result = download_artifacts({request}, connections, cancellation,
    progress->transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress->transfers().update(update, *progress); }} : DownloadProgressSink{}, trace)
                  .front();
   releases[index].inputs.artifacts.emplace(artifact.artifact_id, std::move(result));
  }
  {
   std::unique_lock lock(mutex);
   changed.wait(lock, [&] { return activated || stopped.load(std::memory_order_relaxed); });
   throw_if_benchmark_cancelled(cancellation);
  }
  prepare(index);
  // All source readers have returned before this release's lease is retired.
  lease = {};
  {
   const std::lock_guard lock(mutex);
   ++completed;
  }
  changed.notify_all();
 }
 void activate(std::function<void(std::size_t)> work) {
  {
   const std::lock_guard lock(mutex);
   if (activated) return;
   prepare = std::move(work);
   activated = true;
  }
  changed.notify_all();
  // With one configured CPU there is no concurrent receiver or spare lane.
  // Execute serially instead of waiting for work in an absent pool.
  if (!pool)
   for (std::size_t index = 0; index < releases.size(); ++index) acquire_release(index);
 }
 void metadata_ready(std::size_t index, CoconutRecipePreparation value) {
  {
   const std::lock_guard lock(mutex);
   releases[index].metadata = std::move(value);
   ready.push_back(index);
  }
  changed.notify_all();
 }
 std::pair<std::size_t, CoconutRecipePreparation> take_metadata() {
  std::unique_lock lock(mutex);
  // Managed release lanes own every blocking operation. The receiver remains
  // available even when another release is acquiring inputs or importing masks.
  changed.wait(lock, [&] { return error || consumed < ready.size(); });
  if (error) std::rethrow_exception(error);
  const auto index = ready[consumed++];
  auto value = std::move(*releases[index].metadata);
  releases[index].metadata.reset();
  return {index, std::move(value)};
 }
 void finish() {
  {
   std::unique_lock lock(mutex);
   changed.wait(lock, [&] { return error || completed == releases.size(); });
  }
  pool.reset();
  if (error) std::rethrow_exception(error);
  prepare = {};
 }
 std::vector<Release> releases;
 CocoAnnotationIndexes originals;
 std::optional<CoconutRecoveryOriginals> recovery_originals;
 bool originals_ready = false;
 const BenchmarkCacheLayout* cache = nullptr;
 const CoconutRecipeCatalog* catalog = nullptr;
 ProgressReporter* progress = nullptr;
 BenchmarkTraceSink trace;
 std::size_t connections = 1;
 std::unique_ptr<StorageReservationPool> reservations;
 IndexingProgressTotals* indexing = nullptr;
 mmltk::common::concurrency::CancellationObservation external;
 std::function<void(std::exception_ptr)> failed;
 std::atomic<bool> stopped{false};
 bool retiring = false;
 std::atomic<std::size_t> next{0};
 std::mutex mutex;
 std::condition_variable changed;
 std::exception_ptr error;
 bool activated = false;
 std::function<void(std::size_t)> prepare;
 std::vector<std::size_t> ready;
 std::size_t consumed = 0, completed = 0;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
};
namespace {
std::string digest_text(std::string_view text) { return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()))); }
}  // namespace
std::span<const CoconutImageNamespace> coconut_release_sources(CoconutEdition edition) {
 static constexpr CoconutImageNamespace base[]{CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoUnlabeled};
 static constexpr CoconutImageNamespace validation[]{CoconutImageNamespace::CocoValidation};
 static constexpr CoconutImageNamespace objects[]{CoconutImageNamespace::Objects365V2};
 static constexpr CoconutImageNamespace objects_validation[]{CoconutImageNamespace::Objects365V1};
 switch (edition) {
  case CoconutEdition::Base: return base;
  case CoconutEdition::RelabeledValidation: return validation;
  case CoconutEdition::Large:
  case CoconutEdition::XLarge: return objects;
  case CoconutEdition::ObjectsValidation: return objects_validation;
 }
 throw std::invalid_argument("invalid COCONut edition");
}
CoconutFailureReport::CoconutFailureReport(const std::filesystem::path& cache_root, ProgressReporter& progress) : progress_(progress) {
 auto directory = cache_root;
 for (auto parent = cache_root; !parent.empty(); parent = parent.parent_path()) {
  if (parent.filename() == ".cache") {
   directory = parent;
   break;
  }
  if (parent == parent.root_path()) break;
 }
 path_ = directory / "failed.txt";
}
void CoconutFailureReport::reject(const CoconutPhysicalImage& image, const std::uint64_t release_image_id, const std::string_view release, const std::uint64_t object_id,
 const std::uint64_t category_id, const std::string_view reason) noexcept {
 try {
  const std::lock_guard lock(mutex_);
  if (!attempted_) {
   attempted_ = true;
   stream_.open(path_, std::ios::app);
  }
  if (stream_) {
   stream_ << nlohmann::json{{"image", image.member}, {"image_id", image.image_id}, {"release_image_id", release_image_id}, {"source", coconut_namespace_name(image.source)}, {"release", release},
               {"object_id", object_id}, {"category_id", category_id}, {"reason", reason}}
               .dump()
           << '\n';
   stream_.flush();
  }
  if (!warned_) {
   warned_ = true;
   progress_.activity(std::string("Skipping invalid COCONut objects; ") + (stream_ ? "details: " : "cannot write failure report: ") + path_.string());
  }
 } catch (...) {
  // Reporting cannot make rejected object metadata fatal to compilation.
 }
}
bool coconut_validation_component(CoconutEdition edition) noexcept { return edition == CoconutEdition::RelabeledValidation || edition == CoconutEdition::ObjectsValidation; }
AnnotationSource coconut_annotation_source(CoconutImageNamespace source) {
 switch (source) {
  case CoconutImageNamespace::CocoTrain:
  case CoconutImageNamespace::CocoUnlabeled:
  case CoconutImageNamespace::CocoValidation: return AnnotationSource::CoconutCoco;
  case CoconutImageNamespace::Objects365V1: return AnnotationSource::CoconutObjects365V1;
  case CoconutImageNamespace::Objects365V2: return AnnotationSource::CoconutObjects365V2;
 }
 throw std::invalid_argument("invalid COCONut image namespace");
}
CoconutRecipeCatalog coconut_recipe_catalog(CoconutValidation validation) {
 CoconutRecipeCatalog catalog;
 for (const auto& release : coconut_release_catalog()) {
  if (release.edition == CoconutEdition::RelabeledValidation && validation == CoconutValidation::Stock) continue;
  if (release.edition == CoconutEdition::ObjectsValidation && validation != CoconutValidation::Coconut) continue;
  catalog.releases.push_back(release);
 }
 catalog.stock_annotations = coco_annotations_artifact();
 catalog.images = {{CoconutImageNamespace::CocoTrain, 0, "train2017", coco_train_images_artifact()}, {CoconutImageNamespace::CocoUnlabeled, 0, "unlabeled2017", coconut_unlabeled_images_artifact()},
  {CoconutImageNamespace::CocoValidation, 0, "val2017", coco_val_images_artifact()}};
 const auto objects = objects365_train_image_artifacts();
 std::vector<bool> admitted(objects.size(), false);
 for (const auto edition : {CoconutEdition::Large, CoconutEdition::XLarge}) {
  for (const auto shard : coconut_objects_training_shards(edition)) {
   if (admitted.at(shard)) continue;
   admitted[shard] = true;
   catalog.images.push_back({CoconutImageNamespace::Objects365V2, static_cast<std::uint16_t>(shard), "patch-" + std::to_string(shard), objects.at(shard)});
  }
 }
 if (validation == CoconutValidation::Coconut) catalog.images.push_back({CoconutImageNamespace::Objects365V1, 0, "validation", coconut_validation_images_artifact()});
 return catalog;
}
namespace {
CoconutRecipePreparation prepare_coconut_release(const BenchmarkCacheLayout& cache, const CoconutReleaseComponent& release, const std::map<CoconutImageNamespace, std::string>& physical_identities,
 const CoconutPhysicalMembership& physical, ProgressReporter& progress, CoconutFailureReport& failures, const CoconutRecoveryOriginals* originals, CoconutReleaseInputs& retained,
 mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, std::span<const CoconutImageNamespace> refreshed_sources, bool metadata_only,
 IndexingProgressTotals* indexing, std::size_t release_index) {
 using namespace mmltk::common::math;
 CoconutRecipePreparation prepared;
 prepared.manifest = {{"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& totals = progress.transfers();
 StorageReservationPool reservations(cache.root, trace);
 constexpr std::size_t acquisition_workers = 1;
 std::optional<CoconutMaskRecovery> recovery;
 if (originals) recovery.emplace(*originals);
 const auto acquire = [&](const CatalogArtifact& artifact, std::string_view owner, bool redownload = false) {
  if (!redownload) return retained.artifacts.at(artifact.artifact_id);
  auto request = make_download_request(cache, owner, artifact);
  request.redownload = true;
  const auto reservation = reservations.reserve(additional_download_bytes(request.destination, artifact.expected_size), "additional COCONut annotation repair bytes");
  auto result = download_artifacts({request}, acquisition_workers, cancellation,
   progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace)
                 .front();
  retained.artifacts.insert_or_assign(artifact.artifact_id, result);
  return result;
 };
 const auto sources = coconut_release_sources(release.edition);
 const auto owner = std::string("coconut-") + std::string(release.name);
 std::vector<DownloadResult> downloads;
 const auto first_artifact = prepared.manifest["artifacts"].size();
 std::string identity = std::string(release.revision) + std::string(kCoconutNormalizationRevision);
 for (const auto source : sources) identity += physical_identities.at(source);
 for (const auto& artifact : release.annotations) {
  downloads.push_back(acquire(artifact, owner));
  identity += downloads.back().identity;
  prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, downloads.back().size, "COCONut annotation archive storage overflow");
  prepared.manifest["artifacts"].push_back({{"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename}, {"expected_size", artifact.expected_size},
   {"expected_sha256", artifact.expected_sha256}, {"identity", downloads.back().identity}});
 }
 identity = digest_text(identity);
 const auto path_for = [&](CoconutImageNamespace source) {
  const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
  const auto suffix = original.empty() ? std::string{} : ".recovery-" + coconut_component_input_identity(identity, source, &*recovery);
  return cache.source_indexes(owner) / (std::string(coconut_namespace_name(source)) + suffix + ".normalized.bin");
 };
 std::vector<CoconutComponent> components;
 std::unordered_set<CoconutImageNamespace> reusable;
 bool complete = true;
 for (auto source : sources) {
  if (std::ranges::find(refreshed_sources, source) != refreshed_sources.end()) {
   complete = false;
   continue;
  }
  const auto key = std::pair{release.edition, source};
  auto cached = retained.components.find(key);
  if (cached == retained.components.end()) {
   auto admitted = load_coconut_component(path_for(source), release.edition, source, coconut_component_input_identity(identity, source, recovery ? &*recovery : nullptr), cancellation);
   if (!admitted) {
    complete = false;
    continue;
   }
   cached = retained.components.emplace(key, std::move(*admitted)).first;
   trace_benchmark_event(trace, "benchmark.annotations.component_admitted",
    [&] { return nlohmann::json{{"edition", release.edition}, {"source", coconut_namespace_name(source)}, {"images", cached->second.index.images.size()}}; });
  }
  reusable.insert(source);
  if (!metadata_only) {
   components.push_back(std::move(cached->second));
   retained.components.erase(cached);
  } else {
   const auto& full = cached->second;
   CoconutComponent membership;
   membership.edition = full.edition;
   membership.source = full.source;
   membership.input_identity = full.input_identity;
   membership.index.source = full.index.source;
   membership.index.split = full.index.split;
   membership.index.annotation_sha256 = full.index.annotation_sha256;
   membership.index.images = full.index.images;
   for (auto& image : membership.index.images) {
    image.first_box = 0;
    image.box_count = 0;
   }
   membership.inventory = full.inventory;
   components.push_back(std::move(membership));
  }
 }
 if (!complete) {
  prepared.annotation_cache_hit = false;
  auto cached_components = std::move(components);
  components.clear();
  progress.source_activity(BenchmarkDatasetSource::kCoconut, "Normalizing required " + std::string(release.name) + " annotations and masks", false);
  CoconutImportRequest request;
  request.edition = release.edition;
  request.metadata_only = metadata_only;
  std::vector<CoconutImageNamespace> retained_sources(reusable.begin(), reusable.end());
  request.retained_sources = retained_sources;
  request.input_identity = identity;
  request.recovery = recovery ? &*recovery : nullptr;
  request.physical_membership = &physical;
  request.expected_rows = release.expected_rows;
  request.cancellation = cancellation;
  request.rejected_object = [&](const CoconutPhysicalImage& physical_image, const CoconutRecord& record, const CoconutSegment& segment, std::string_view reason) {
   if (!request.recovery || request.recovery->original_identity(physical_image.source).empty()) failures.reject(physical_image, record.image_id, release.name, segment.id, segment.category_id, reason);
  };
  if (!metadata_only && progress.normalization_observer_enabled())
   request.progress = [&](std::uint64_t rows) {
    if (indexing) indexing->update(release_index, rows, progress);
   };
  for (const auto& artifact : downloads) {
   if (artifact.path.extension() == ".parquet")
    request.parquet_shards.push_back(artifact.path);
   else if (artifact.path.extension() == ".json")
    request.annotation_json = artifact.path;
   else
    request.mask_archive = artifact.path;
  }
  for (unsigned attempt = 1;; ++attempt) {
   try {
    components = import_coconut_annotations(request);
    for (auto& cached : cached_components) {
     const auto found = std::ranges::find(components, cached.source, &CoconutComponent::source);
     if (found != components.end())
      *found = std::move(cached);
     else
      components.push_back(std::move(cached));
    }
    std::ranges::sort(components, {}, &CoconutComponent::source);
    break;
   } catch (const CoconutPhysicalMembershipError&) { throw; } catch (const InsufficientBenchmarkStorage&) {
    throw;
   } catch (const std::exception& error) {
    throw_if_benchmark_cancelled(cancellation);
    if (attempt == 3) throw;
    bool repaired = false;
    std::string refreshed_identity = std::string(release.revision) + std::string(kCoconutNormalizationRevision);
    for (const auto source : sources) refreshed_identity += physical_identities.at(source);
    for (std::size_t i = 0; i < release.annotations.size(); ++i) {
     const auto& artifact = release.annotations[i];
     auto download_request = make_download_request(cache, owner, artifact);
     bool matches_expected = false;
     if (std::filesystem::is_regular_file(download_request.destination)) {
      const auto sha = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(download_request.destination, [&] { return cancellation.requested(); }));
      matches_expected = !artifact.expected_sha256.empty() && sha == artifact.expected_sha256;
      trace_benchmark_event(trace, "benchmark.download.failure_sha256",
       [&] { return nlohmann::json{{"artifact", artifact.artifact_id}, {"sha256", sha}, {"matches_expected", matches_expected}, {"reason", error.what()}}; });
     }
     if (!matches_expected) {
      // The failed importer and its row callbacks have unwound. Withdraw once
      // before replacing any artifact on which this release product depends.
      if (!repaired && indexing) indexing->invalidate(release_index, progress);
      invalidate_download_artifact(download_request, cancellation, trace);
      downloads[i] = acquire(artifact, owner, true);
      repaired = true;
     }
     refreshed_identity += downloads[i].identity;
    }
    if (!repaired) throw;
    for (const auto source : sources) retained.components.erase(std::pair{release.edition, source});
    reusable.clear();
    cached_components.clear();
    retained_sources.clear();
    request.retained_sources = {};
    identity = digest_text(refreshed_identity);
    request.input_identity = identity;
    progress.source_activity(BenchmarkDatasetSource::kCoconut, "Retrying required COCONut masks after source repair", false);
   }
  }
  for (const auto& component : components)
   if (std::ranges::find(sources, component.source) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
  for (const auto& component : components) {
   if (!metadata_only && !component.index.images.empty() && !reusable.contains(component.source)) store_coconut_component(path_for(component.source), component, cancellation);
  }
 }
 for (std::size_t i = 0; i < downloads.size(); ++i) prepared.manifest["artifacts"][first_artifact + i]["identity"] = downloads[i].identity;
 for (const auto& component : components)
  if (std::ranges::find(sources, component.source) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
 std::uint64_t admitted_rows = 0;
 for (const auto& component : components) admitted_rows = checked_add(admitted_rows, component.index.images.size(), "COCONut release count overflow");
 if (release.expected_rows != 0 && admitted_rows != release.expected_rows) throw std::runtime_error("COCONut cached release membership is incomplete");
 for (auto& component : components) {
  const auto index_path = path_for(component.source);
  if (!metadata_only || complete)
   for (const auto& path : {index_path, std::filesystem::path(index_path.string() + ".inventory"), std::filesystem::path(index_path.string() + ".complete.json")})
    prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, std::filesystem::file_size(path), "COCONut index storage overflow");
  prepared.manifest["components"].push_back({{"edition", release.edition}, {"revision", release.revision}, {"physical_source", coconut_namespace_name(component.source)},
   {"input_identity", component.input_identity}, {"recovery_policy", component.recovery_policy}, {"original_annotation_identity", component.original_annotation_identity},
   {"offered_images", component.index.images.size()}, {"annotation_source", coconut_annotation_source(component.source)}, {"imported_annotation_sha256", component.index.annotation_sha256},
   {"imported_index", index_path.lexically_relative(cache.root).string()},
   {"imported_index_identity", metadata_only && !complete ? nlohmann::json(nullptr) : read_json_file(index_path.string() + ".complete.json").at("identity")}});
  prepared.components.push_back(std::move(component));
 }
 return prepared;
}
}  // namespace
std::shared_ptr<CoconutRecipeInputs> acquire_coconut_recipe_inputs(const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog, ProgressReporter& progress, std::size_t workers,
 mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, std::size_t download_connections, std::span<const int> cpus,
 std::function<void(std::exception_ptr)> failed) {
 auto inputs = std::make_shared<CoconutRecipeInputs>(catalog.releases.size(), cancellation, std::move(failed));
 inputs->cache = &cache;
 inputs->catalog = &catalog;
 inputs->progress = &progress;
 inputs->trace = trace;
 inputs->connections = std::max<std::size_t>(1, download_connections ? download_connections : workers);
 inputs->reservations = std::make_unique<StorageReservationPool>(cache.root, trace);
 const auto lanes = std::min(workers, catalog.releases.size());
 if (lanes) {
  auto affinity = cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(cpus.begin(), cpus.end());
  inputs->pool = std::make_unique<mmltk::common::concurrency::WorkerPool>(lanes, std::move(affinity), "bench_release", lanes);
  try {
   for (std::size_t lane = 0; lane < lanes; ++lane)
    inputs->pool->enqueue_detached([owner = inputs.get()] {
     try {
      for (;;) {
       throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*owner));
       const auto index = owner->next.fetch_add(1, std::memory_order_relaxed);
       if (index >= owner->releases.size()) break;
       owner->acquire_release(index);
      }
     } catch (...) { owner->fail(std::current_exception()); }
    });
  } catch (...) {
   inputs->fail(std::current_exception());
   throw;
  }
 }
 return inputs;
}
CoconutRecipePreparation prepare_coconut_recipe(const BenchmarkCompilerConfig& config, const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog,
 std::span<const AdmittedRecipeArchive> acquired, const CoconutPhysicalMembership& physical, ProgressReporter& progress, CoconutFailureReport& failures, std::size_t workers,
 mmltk::common::concurrency::CancellationObservation external_cancellation, const BenchmarkTraceSink& trace, std::span<const CoconutImageNamespace> refreshed_sources, bool metadata_only,
 std::shared_ptr<CoconutRecipeInputs> inputs) {
 using namespace mmltk::common::math;
 const auto cancellation = external_cancellation;
 CoconutRecipePreparation prepared;
 const bool initial_preparation = !inputs || metadata_only;
 prepared.inputs = inputs ? std::move(inputs) : acquire_coconut_recipe_inputs(cache, catalog, progress, 0, cancellation, trace, workers);
 auto& retained = *prepared.inputs;
 auto& totals = progress.transfers();
 StorageReservationPool reservations(cache.root, trace);
 const auto acquisition_workers = std::max<std::size_t>(1, workers);
 const auto parse_workers = acquisition_workers;
 if (initial_preparation) progress.phase(DatasetCompilePhase::Downloading);
 std::map<CoconutImageNamespace, std::string> physical_identities;
 for (const auto& archive : acquired) physical_identities[archive.origin.source] += archive.download.identity;
 prepared.manifest = {{"dataset", "coconut"}, {"validation", config.selection.validation}, {"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& originals = retained.originals;
 std::optional<CocoAnnotationCache> annotations;
 if (config.selection.recover_dropped_masks || config.selection.validation == CoconutValidation::Stock) {
  if (!retained.originals_ready) {
   const CocoAnnotationRequest selection{config.selection.recover_dropped_masks ? CocoSplitAdmission::Optional : CocoSplitAdmission::Unselected,
    config.selection.validation == CoconutValidation::Stock ? CocoSplitAdmission::Required : CocoSplitAdmission::Optional};
   trace_benchmark_event(
    trace, "benchmark.annotations.originals_begin", [&] { return nlohmann::json{{"recover_dropped_masks", config.selection.recover_dropped_masks}, {"validation", config.selection.validation}}; });
   annotations.emplace(cache, catalog.stock_annotations, selection, 0, checked_cast<std::uint32_t>(catalog.coco_validation_images, "COCO validation count overflow"), static_cast<int>(parse_workers),
    cancellation, trace);
   annotations->discover(progress);
   if (annotations->pending_download()) {
    const auto& request = *annotations->pending_download();
    const auto reservation = reservations.reserve(additional_download_bytes(request.destination, request.expected_size), "additional COCO annotation download bytes");
    try {
     auto archive = download_artifacts({request}, acquisition_workers, cancellation,
      progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace)
                     .front();
     auto completed = annotations->completed_indexes();
     annotations->settle(std::move(archive), progress, parse_workers, completed, config.selection.recover_dropped_masks ? 2 : 1);
    } catch (const BenchmarkDownloadUnavailable& error) { annotations->download_unavailable(error, progress); }
   }
   originals = annotations->take_indexes();
   // Original indexes are now owned immutable values. Release the COCO cache
   // lifecycle before any wait on independently owned release work.
   annotations.reset();
   if (config.selection.recover_dropped_masks)
    retained.recovery_originals.emplace(originals.train ? &*originals.train : nullptr, originals.validation ? &*originals.validation : nullptr, cancellation);
   retained.originals_ready = true;
  }
  prepared.annotation_cache_hit = originals.cache_hit;
  prepared.annotation_storage_bytes = originals.retained_storage_bytes;
 }
 if (progress.normalization_observer_enabled() && !retained.indexing) {
  std::vector<std::uint64_t> rows;
  rows.reserve(catalog.releases.size());
  for (const auto& release : catalog.releases) rows.push_back(release.expected_rows);
  retained.indexing = progress.indexing(rows);
  if (!rows.empty()) retained.indexing->update(0, 0, progress);
 }
 retained.activate([&cache, &catalog, &physical, &progress, &failures, &retained, physical_identities, refreshed_sources, trace, metadata_only](std::size_t index) {
  const auto release_cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(retained);
  const auto& release = catalog.releases[index];
  auto& release_inputs = retained.releases[index].inputs;
  if (metadata_only) {
   auto metadata = prepare_coconut_release(cache, release, physical_identities, physical, progress, failures, retained.recovery_originals ? &*retained.recovery_originals : nullptr, release_inputs,
    release_cancellation, trace, refreshed_sources, true, retained.indexing, index);
   trace_benchmark_event(trace, "benchmark.annotations.release_metadata", [&] { return nlohmann::json{{"edition", release.edition}, {"cache_hit", metadata.annotation_cache_hit}}; });
   retained.metadata_ready(index, std::move(metadata));
  }
  if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MasksStarted);
  retained.releases[index].complete = prepare_coconut_release(cache, release, physical_identities, physical, progress, failures, retained.recovery_originals ? &*retained.recovery_originals : nullptr,
   release_inputs, release_cancellation, trace, refreshed_sources, false, retained.indexing, index);
  if (retained.indexing) retained.indexing->update(index, release.expected_rows, progress);
  trace_benchmark_event(
   trace, "benchmark.annotations.release_complete", [&] { return nlohmann::json{{"edition", release.edition}, {"cache_hit", retained.releases[index].complete->annotation_cache_hit}}; });
 });
 std::vector<std::optional<CoconutRecipePreparation>> releases(catalog.releases.size());
 if (metadata_only) {
  for (std::size_t count = 0; count < releases.size(); ++count) {
   auto [index, result] = retained.take_metadata();
   if (catalog.release_observer) catalog.release_observer(catalog.releases[index].edition, CoconutReleaseBoundary::MetadataConsumed);
   releases[index] = std::move(result);
  }
 } else {
  retained.finish();
  for (std::size_t index = 0; index < releases.size(); ++index) releases[index] = std::move(retained.releases[index].complete);
 }
 for (auto& release : releases) {
  prepared.annotation_cache_hit = prepared.annotation_cache_hit && release->annotation_cache_hit;
  prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, release->annotation_storage_bytes, "COCONut annotation storage overflow");
  for (auto& value : release->manifest["artifacts"]) prepared.manifest["artifacts"].push_back(std::move(value));
  for (auto& value : release->manifest["components"]) prepared.manifest["components"].push_back(std::move(value));
  prepared.components.insert(prepared.components.end(), std::make_move_iterator(release->components.begin()), std::make_move_iterator(release->components.end()));
 }
 prepared.duplicate_xl_images = reconcile_coconut_extensions(prepared.components, cancellation);
 std::erase_if(prepared.components, [](const CoconutComponent& component) { return component.index.images.empty(); });
 if (config.selection.validation == CoconutValidation::Stock) {
  if (metadata_only) {
   prepared.stock_validation.emplace();
   prepared.stock_validation->source = originals.validation->source;
   prepared.stock_validation->split = originals.validation->split;
   prepared.stock_validation->annotation_sha256 = originals.validation->annotation_sha256;
   prepared.stock_validation->images = originals.validation->images;
   for (auto& image : prepared.stock_validation->images) {
    image.first_box = 0;
    image.box_count = 0;
   }
  } else
   prepared.stock_validation = std::move(originals.validation);
  prepared.validation_images = prepared.stock_validation->images.size();
 }
 // COCO shares one physical ID domain across its archive subsets; Objects365 editions remain distinct.
 const auto split_key = [](const CoconutPhysicalImage& image) {
  const auto source = image.source == CoconutImageNamespace::CocoUnlabeled || image.source == CoconutImageNamespace::CocoValidation ? CoconutImageNamespace::CocoTrain : image.source;
  return std::pair{source, image.image_id};
 };
 constexpr auto namespace_count = static_cast<std::size_t>(CoconutImageNamespace::Objects365V2) + 1;
 std::array<std::unordered_set<std::uint64_t>, namespace_count> train_members;
 const auto contains_training = [&](const CoconutPhysicalImage& image) {
  const auto [source, id] = split_key(image);
  return train_members[static_cast<std::size_t>(source)].contains(id);
 };
 std::uint64_t coco_val_count = prepared.validation_images;
 for (const auto& component : prepared.components) {
  if (!coconut_validation_component(component.edition)) {
   const auto source = split_key(component.inventory.front().physical).first;
   auto& membership = train_members[static_cast<std::size_t>(source)];
   membership.reserve(checked_add(membership.size(), component.inventory.size(), "COCONut split membership overflow"));
   for (const auto& image : component.inventory) {
    throw_if_benchmark_cancelled(cancellation);
    if (!membership.insert(image.physical.image_id).second) throw std::runtime_error("COCONut training contains an unreconciled physical member");
   }
  } else {
   prepared.validation_images = checked_add(prepared.validation_images, component.index.images.size(), "COCONut validation count overflow");
   if (component.source == CoconutImageNamespace::CocoValidation) coco_val_count = component.index.images.size();
  }
 }
 if (prepared.stock_validation)
  for (const auto& image : prepared.stock_validation->images)
   if (train_members[static_cast<std::size_t>(CoconutImageNamespace::CocoTrain)].contains(image.source_image_id)) throw std::runtime_error("COCONut training reuses a stock validation physical ID");
 if (coco_val_count != catalog.coco_validation_images) throw std::runtime_error("COCONut COCO validation membership is incomplete");
 for (const auto& component : prepared.components) {
  if (coconut_validation_component(component.edition))
   for (const auto& image : component.inventory)
    if (contains_training(image.physical)) throw std::runtime_error("COCONut train and validation reuse a physical member");
 }
 for (auto& facts : prepared.manifest["components"]) {
  const auto found = std::ranges::find_if(
   prepared.components, [&](const CoconutComponent& component) { return facts["edition"] == component.edition && facts["physical_source"] == coconut_namespace_name(component.source); });
  const auto admitted = found == prepared.components.end() ? 0U : found->index.images.size();
  facts["admitted_images"] = admitted;
  facts["covered_by_large_images"] = facts["offered_images"].get<std::uint64_t>() - admitted;
  facts["selected_annotation_sha256"] = found == prepared.components.end() ? nlohmann::json(nullptr) : nlohmann::json(found->index.annotation_sha256);
 }
 for (const auto& admitted : acquired) {
  const auto& archive = admitted.origin;
  prepared.manifest["artifacts"].push_back(
   {{"artifact_id", archive.artifact.artifact_id}, {"url", archive.artifact.url}, {"filename", archive.artifact.filename}, {"expected_size", archive.artifact.expected_size},
    {"expected_sha256", archive.artifact.expected_sha256}, {"physical_source", coconut_namespace_name(archive.source)}, {"identity", admitted.download.identity}, {"bytes", admitted.download.size}});
 }
 if (config.selection.validation == CoconutValidation::Stock || config.selection.recover_dropped_masks)
  prepared.manifest["artifacts"].push_back({{"artifact_id", catalog.stock_annotations.artifact_id}, {"url", catalog.stock_annotations.url}, {"expected_size", catalog.stock_annotations.expected_size},
   {"expected_sha256", catalog.stock_annotations.expected_sha256}});
 std::uint64_t recovered = 0, unresolved = 0;
 for (const auto& component : prepared.components) {
  std::uint64_t component_recovered = 0;
  const bool eligible = component.source == CoconutImageNamespace::CocoTrain || component.source == CoconutImageNamespace::CocoValidation;
  std::uint64_t component_unresolved = config.selection.recover_dropped_masks && eligible && !component.recovery_policy ? component.index.rejected.degenerate_boxes : 0;
  for (std::size_t position = 0; position < component.recovery.size(); ++position) {
   const auto& image = component.recovery[position];
   const auto& inventory = component.inventory.at(position);
   if (inventory.physical.image_id != image.image_id) throw std::logic_error("recovery report image is absent from component inventory");
   for (const auto& object : image.omissions)
    failures.reject(inventory.physical, inventory.release_image_id, coconut_release_component(component.edition).name, object.annotation_id, object.source_category_id,
     "thing segment remains without mask pixels or an authoritative bbox");
   component_recovered = checked_add(component_recovered, image.objects.size(), "recovery count overflow");
   component_unresolved = checked_add(component_unresolved, image.unresolved, "recovery omission count overflow");
  }
  recovered = checked_add(recovered, component_recovered, "recovery count overflow");
  unresolved = checked_add(unresolved, component_unresolved, "recovery omission count overflow");
  for (auto& facts : prepared.manifest["components"])
   if (facts["edition"] == component.edition && facts["physical_source"] == coconut_namespace_name(component.source)) {
    facts["recovered_objects"] = component_recovered;
    facts["unresolved_objects"] = component_unresolved;
   }
 }
 prepared.manifest["recover_dropped_masks"] = config.selection.recover_dropped_masks;
 if (config.selection.recover_dropped_masks)
  prepared.manifest["original_annotations"] = {{"train", originals.train ? nlohmann::json(originals.train->annotation_sha256) : nlohmann::json(nullptr)},
   {"validation", prepared.stock_validation ? nlohmann::json(prepared.stock_validation->annotation_sha256)
                  : originals.validation    ? nlohmann::json(originals.validation->annotation_sha256)
                                            : nlohmann::json(nullptr)}};
 prepared.manifest["recovered_objects"] = recovered;
 prepared.manifest["unresolved_objects"] = unresolved;
 if (config.selection.recover_dropped_masks) progress.activity("COCONut mask recovery: " + std::to_string(recovered) + " recovered, " + std::to_string(unresolved) + " unresolved");
 prepared.manifest["duplicate_xl_images"] = prepared.duplicate_xl_images;
 prepared.manifest["validation_images"] = prepared.validation_images;
 return prepared;
}
}  // namespace mmltk::backend::data::benchmark_internal
