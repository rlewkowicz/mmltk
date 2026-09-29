#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_recipe.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/detail/benchmark_annotation_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/io/file_digest.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
struct CoconutReleaseInputs {
 std::unordered_map<std::string, DownloadResult> artifacts;
 std::shared_ptr<CoconutAnnotationRecords> records = std::make_shared<CoconutAnnotationRecords>();
 std::map<std::pair<CoconutEdition, CoconutImageNamespace>, CoconutComponent> components;
};
// Opaque outside this owner. A release holds its lifecycle lease only until its
// own parser finishes, never while waiting for another release's lease.
struct CoconutRecipeInputs {
 struct Release {
  CoconutReleaseInputs inputs;
  std::optional<CoconutRecipePreparation> metadata, complete;
  std::jthread masks;
 };
 CoconutRecipeInputs(std::size_t count, mmltk::common::concurrency::CancellationObservation cancellation, std::function<void(std::exception_ptr)> failure)
     : releases(count), external(cancellation), failed(std::move(failure)) {
  ready.reserve(count);
 }
 ~CoconutRecipeInputs() { settle(); }
 void settle() {
  {
   const std::lock_guard lock(mutex);
   retiring = true;
   stopped.store(true, std::memory_order_relaxed);
  }
  changed.notify_all();
  if (execution) execution->notify_admission_change();
  controllers.clear();
  for (auto& release : releases) release.masks = {};
  original_controller = {};
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
 void start_originals(std::function<void()> work) {
  if (originals_started) return;
  originals_started = true;
  original_controller = std::jthread([this, work = std::move(work)] {
   try {
    work();
    { const std::lock_guard lock(mutex); originals_ready = true; }
    changed.notify_all();
   } catch (...) { fail(std::current_exception()); }
  });
 }
 [[nodiscard]] bool originals_available() {
  const std::lock_guard lock(mutex);
  if (error) std::rethrow_exception(error);
  return originals_ready;
 }
 struct OriginalSplit {
  CocoAnnotationSplit state;
  std::shared_ptr<const CoconutRecoveryOriginals> recovery;
 };
 void original_split(CocoAnnotationSplit value, bool recover) {
  const bool withdrew = !value.index;
  const bool training = value.training;
  const auto generation = value.generation;
  std::shared_ptr<const CoconutRecoveryOriginals> recovery;
  const auto index = [&](std::size_t) {
   if (recover && value.index) recovery = std::make_shared<CoconutRecoveryOriginals>(value.training ? &*value.index : nullptr, value.training ? nullptr : &*value.index, external);
  };
  if (recover && value.index) { if (execution) execution->run(BenchmarkStage::Metadata, {}, index); else index(0); }
  {
   const std::lock_guard lock(mutex);
   auto& split = value.training ? train_originals : validation_originals;
   if (value.generation < split.state.generation) return;
   split.state = std::move(value);
   split.recovery = std::move(recovery);
  }
  if (execution) {
   const auto split = training ? "train2017" : "val2017";
   execution->original_generation(cache->source_images("coco") / split, generation, withdrew);
   execution->notify_admission_change();
  }
  changed.notify_all();
 }
 [[nodiscard]] OriginalSplit original(CoconutImageNamespace source, bool wait, std::stop_token stop = {}) {
  const std::stop_callback wake(stop, [this] { changed.notify_all(); });
  std::unique_lock lock(mutex);
  auto& split = source == CoconutImageNamespace::CocoTrain ? train_originals : validation_originals;
  if (wait) {
   changed.wait(lock, [&] { return split.state.terminal || error || cancelled() || stop.stop_requested(); });
   if (error) std::rethrow_exception(error);
   if (stop.stop_requested()) throw std::runtime_error("COCONut recovery input retired");
   throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
  }
  return split;
 }
 void acquire_release(std::size_t index) {
  const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
  const auto& release = catalog->releases[index];
  auto lease = ArtifactLease::acquire_charged(cache->locks / (std::string(release.name) + ".annotations.lifecycle.lock"), cancellation, execution,
   coco_annotation_resources());
  const auto& handles = lease->allowance();
  std::vector<DownloadRequest> requests;
  requests.reserve(release.annotations.size());
  for (const auto& artifact : release.annotations) requests.push_back(make_download_request(*cache, "coconut-" + std::string(release.name), artifact));
  auto results = download_artifacts(requests, connections, cancellation,
   progress->transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress->transfers().update(update, *progress); }} : DownloadProgressSink{},
   trace, {}, execution, handles, reservations.get());
  for (std::size_t i = 0; i < requests.size(); ++i) releases[index].inputs.artifacts.emplace(requests[i].artifact_id, std::move(results[i]));
  {
   std::unique_lock lock(mutex);
   changed.wait(lock, [&] { return activated || stopped.load(std::memory_order_relaxed); });
   throw_if_benchmark_cancelled(cancellation);
  }
  prepare(index, handles, false);
  // Metadata readers and download continuations have settled. Keep the actual
  // lifecycle lock, but let later release metadata use its unused promise.
  // Full readers declare their own complete physical/Arrow input envelope.
  handles.retire_continuation();
  // Metadata/download controllers return immediately. Fixed release-owned mask
  // controllers retain their own lease; only ready CPU jobs use shared lanes.
  releases[index].masks = std::jthread([this, index, lease = std::move(lease)]() mutable {
   try {
    prepare(index, lease->allowance(), true);
    lease = {};
    publish_labels(index);
    { const std::lock_guard lock(mutex); ++completed; }
    changed.notify_all();
   } catch (...) { fail(std::current_exception()); }
  });
 }
 void publish_labels(std::size_t index) {
  if (!execution || !physical) return;
  auto& release = *releases[index].complete;
  for (const auto& component : release.cached_label_inputs) for (std::size_t image = 0; image < component.index().image_count(); ++image) {
   const auto& input = component.inventory_image(image).physical;
   execution->labels_ready(physical->label_publication(component.edition(), input), input.image_id, component.labels(image),
    component.image_input_identity(image), component.original_generation());
  }
  release.cached_label_inputs.clear();
 }
 void activate(std::function<void(std::size_t, const BenchmarkAllowance&, bool)> work) {
  {
   const std::lock_guard lock(mutex);
   if (activated) return;
   prepare = std::move(work);
   activated = true;
  }
  changed.notify_all();
  // With one configured CPU there is no concurrent receiver or spare lane.
  // Execute serially instead of waiting for work in an absent pool.
  if (controllers.empty())
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
  controllers.clear();
  for (auto& release : releases) release.masks = {};
  original_controller = {};
  if (error) std::rethrow_exception(error);
  for (std::size_t index = 0; index < releases.size(); ++index) {
   auto& release = releases[index];
   if (!release.complete) continue;
   bool stale = false;
   std::uint64_t affected = 0;
   for (const auto& component : release.complete->components) {
    if (component.source() != CoconutImageNamespace::CocoTrain && component.source() != CoconutImageNamespace::CocoValidation) continue;
    const auto current = original(component.source(), false);
    if (component.original_generation() && component.original_generation() != current.state.generation) { stale = true; affected += component.index().image_count(); }
   }
   if (!stale) continue;
   for (const auto& component : release.complete->components)
    if (component.source() != CoconutImageNamespace::CocoTrain && component.source() != CoconutImageNamespace::CocoValidation)
     release.inputs.components.insert_or_assign({component.edition(), component.source()}, component);
   if (indexing) indexing->invalidate(index, *progress, affected);
   const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
   auto lease = ArtifactLease::acquire_charged(cache->locks / (std::string(catalog->releases[index].name) + ".annotations.lifecycle.lock"), cancellation, execution, coco_annotation_resources());
   prepare(index, lease->allowance(), true);
   lease = {};
   publish_labels(index);
  }
  prepare = {};
  source_transport.reset();
 }
 std::vector<Release> releases;
 CocoAnnotationIndexes originals;
 OriginalSplit train_originals, validation_originals;
 bool originals_ready = false, originals_started = false;
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
 std::function<void(std::size_t, const BenchmarkAllowance&, bool)> prepare;
 std::vector<std::size_t> ready;
 std::size_t consumed = 0, completed = 0;
 BenchmarkCompilePipeline* execution = nullptr;
 const CoconutPhysicalMembership* physical = nullptr;
 std::unique_ptr<BenchmarkCurl::Channel> source_transport;
 std::vector<std::jthread> controllers;
 std::jthread original_controller;
};
namespace {
std::string digest_text(std::string_view text) { return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()))); }
}  // namespace
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
   stream_
    << nlohmann::
        json{{"image", image.member}, {"image_id", image.image_id}, {"release_image_id", release_image_id}, {"source", coconut_namespace_name(image.source)}, {"release", release}, {"object_id", object_id}, {"category_id", category_id}, {"reason", reason}}
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
 catalog.images = {
  {CoconutImageNamespace::CocoTrain, 0, "train2017", coco_train_images_artifact()}, {CoconutImageNamespace::CocoUnlabeled, 0, "unlabeled2017", coconut_unlabeled_images_artifact()},
  {CoconutImageNamespace::CocoValidation, 0, "val2017", coco_val_images_artifact()}
 };
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
CoconutRecipePreparation prepare_coconut_release(const BenchmarkCacheLayout& cache, const CoconutReleaseComponent& release,
 const CoconutPhysicalMembership& physical, ProgressReporter& progress, CoconutFailureReport& failures, const CoconutOriginalProvider& original_provider, CoconutReleaseInputs& retained,
 mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, bool metadata_only,
 IndexingProgressTotals* indexing, std::size_t release_index, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
 using namespace mmltk::common::math;
 CoconutRecipePreparation prepared;
 prepared.manifest = {{"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& totals = progress.transfers();
 StorageReservationPool reservations(cache.root, trace, execution ? &execution->storage() : nullptr);
 constexpr std::size_t acquisition_workers = 1;
 std::optional<CoconutMaskRecovery> recovery;
 std::shared_ptr<const CoconutRecoveryOriginals> originals;
 const auto recovery_source = release.edition == CoconutEdition::Base ? CoconutImageNamespace::CocoTrain : CoconutImageNamespace::CocoValidation;
 const auto refresh_original = [&](bool wait) {
  if (!original_provider) return CoconutOriginalInput{};
  auto input = original_provider(recovery_source, wait, {});
  originals = input.originals;
  recovery.reset();
  if (originals) recovery.emplace(*originals);
  return input;
 };
 const auto initial_original = refresh_original(false);
 const auto acquire = [&](const CatalogArtifact& artifact, std::string_view owner, bool redownload = false) {
  if (!redownload) return retained.artifacts.at(artifact.artifact_id);
  auto request = make_download_request(cache, owner, artifact);
  request.redownload = true;
  auto result = download_artifacts({request}, acquisition_workers, cancellation,
   progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution, parent, &reservations)
                 .front();
  retained.artifacts.insert_or_assign(artifact.artifact_id, result);
  return result;
 };
 const auto sources = coconut_release_sources(release.edition);
 const auto owner = std::string("coconut-") + std::string(release.name);
 std::vector<DownloadResult> downloads;
 const auto first_artifact = prepared.manifest["artifacts"].size();
 std::string identity;
 for (const auto& artifact : release.annotations) {
  downloads.push_back(acquire(artifact, owner));
  prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, downloads.back().size, "COCONut annotation archive storage overflow");
  prepared.manifest["artifacts"].push_back(
   {{"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename}, {"expected_size", artifact.expected_size}, {"expected_sha256", artifact.expected_sha256},
    {"identity", downloads.back().identity}});
 }
 const auto annotation_identity = [&] {
  nlohmann::json artifacts = nlohmann::json::array();
  for (std::size_t i = 0; i < downloads.size(); ++i) artifacts.push_back({release.annotations[i].artifact_id, downloads[i].identity});
  return digest_text(nlohmann::json{{"domain", "coconut-annotation-artifacts-v1"}, {"revision", release.revision},
   {"normalization", kCoconutNormalizationRevision}, {"artifacts", std::move(artifacts)}}.dump());
 };
 identity = annotation_identity();
 const auto path_for = [&](CoconutImageNamespace source) {
  const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
  const auto suffix = original.empty() ? std::string{} : ".recovery-" + coconut_component_input_identity(identity, source, &*recovery);
  return cache.source_indexes(owner) / (std::string(coconut_namespace_name(source)) + suffix + ".normalized.bin");
 };
 std::vector<CoconutComponent> components, reusable_images;
 std::unordered_set<CoconutImageNamespace> reusable;
 bool complete = true;
 for (auto source : sources) {
  if (!metadata_only && original_provider && !initial_original.terminal && source == recovery_source) { complete = false; continue; }
  const auto key = std::pair{release.edition, source};
  auto cached = retained.components.find(key);
  if (cached != retained.components.end() && !cached->second.matches_inputs(identity, physical, recovery ? &*recovery : nullptr, false)) {
   retained.components.erase(cached);
   cached = retained.components.end();
  }
  if (cached == retained.components.end()) {
   auto admitted = load_coconut_component(path_for(source), release.edition, source, identity, cancellation, metadata_only, &physical, recovery ? &*recovery : nullptr, true);
   if (!admitted) {
    complete = false;
    continue;
   }
   cached = retained.components.emplace(key, std::move(*admitted)).first;
   trace_benchmark_event(trace, "benchmark.annotations.component_admitted",
    [&] { return nlohmann::json{{"edition", release.edition}, {"source", coconut_namespace_name(source)}, {"images", cached->second.index().image_count()}}; });
  }
  if (!cached->second.matches_inputs(identity, physical, recovery ? &*recovery : nullptr)) {
   complete = false;
   if (!metadata_only) { admit_coconut_component(cached->second, cancellation); reusable_images.push_back(cached->second); }
   continue;
  }
  reusable.insert(source);
  if (!metadata_only) {
   admit_coconut_component(cached->second, cancellation);
   if (original_provider && source == recovery_source) cached->second = cached->second.with_original(initial_original);
   components.push_back(cached->second);
   prepared.cached_label_inputs.push_back(cached->second);
  } else {
   components.push_back(cached->second.membership());
  }
 }
 if (!complete) {
  prepared.annotation_cache_hit = false;
  auto cached_components = std::move(components);
  components.clear();
  progress.source_activity(BenchmarkDatasetSource::kCoconut, "Normalizing required " + std::string(release.name) + " annotations and masks", false);
  CoconutImportRequest request;
  request.execution = execution;
  request.parent_allowance = parent;
  request.edition = release.edition;
  request.inventory_directory = path_for(sources.front()).parent_path();
  request.metadata_only = metadata_only;
  request.records = retained.records;
  std::vector<CoconutImageNamespace> retained_sources(reusable.begin(), reusable.end());
  request.retained_sources = retained_sources;
  request.reusable_images = reusable_images;
  request.input_identity = identity;
  request.recovery = recovery ? &*recovery : nullptr;
  std::atomic<std::uint64_t> used_original_generation{0};
  std::atomic<bool> original_changed{false};
  request.originals = metadata_only || !original_provider ? CoconutOriginalProvider{} : CoconutOriginalProvider{[&](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto input = original_provider(source, wait, stop);
   if (input.terminal) {
    auto expected = std::uint64_t{0};
    if (!used_original_generation.compare_exchange_strong(expected, input.generation) && expected != input.generation) { original_changed.store(true); throw CoconutOriginalChanged{source}; }
   }
   return input;
  }};
  request.physical_membership = &physical;
  request.expected_rows = release.expected_rows;
  request.cancellation = cancellation;
  request.rejected_object = [&](const CoconutPhysicalImage& physical_image, const CoconutRecord& record, const CoconutSegment& segment, std::string_view reason, std::uint32_t policy) {
   if (!policy) failures.reject(physical_image, record.image_id, release.name, segment.id, segment.category_id, reason);
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
    if (!metadata_only && original_provider) {
     const auto current_original = refresh_original(true);
     const bool generation_changed = original_changed.load() ||
      (used_original_generation.load() && used_original_generation.load() != current_original.generation);
     const auto stale = generation_changed || std::ranges::any_of(components, [&](const CoconutComponent& component) {
      return !component.matches_inputs(identity, physical, recovery ? &*recovery : nullptr);
     });
     if (stale) {
      for (auto& component : components) if (component.source() != recovery_source && !reusable.contains(component.source())) {
       reusable.insert(component.source()); retained_sources.push_back(component.source()); cached_components.push_back(std::move(component));
      }
      request.retained_sources = retained_sources;
      request.recovery = recovery ? &*recovery : nullptr;
      reusable_images.clear(); request.reusable_images = {};
      used_original_generation.store(0); original_changed.store(false);
      continue;
     }
    }
    for (auto& cached : cached_components) {
     const auto found = std::ranges::find(components, cached.source(), &CoconutComponent::source);
     if (found != components.end())
      *found = std::move(cached);
     else
      components.push_back(std::move(cached));
    }
    std::ranges::sort(components, {}, &CoconutComponent::source);
    break;
   } catch (const CoconutOriginalChanged&) {
    (void)refresh_original(true);
    request.recovery = recovery ? &*recovery : nullptr;
    reusable_images.clear(); request.reusable_images = {};
    used_original_generation.store(0); original_changed.store(false);
    --attempt;
    continue;
   } catch (const std::bad_alloc&) { throw; } catch (const PhysicalArchiveFailure&) { throw; } catch (const CoconutPhysicalMembershipError&) { throw; } catch (const InsufficientBenchmarkResources&) { throw; } catch (const InsufficientBenchmarkStorage&) {
    throw;
   } catch (const std::exception& error) {
    // The failed generation cannot retain input backing into artifact repair.
    // This also covers failures after a successful import returned its rows.
    retained.records->discard();
    throw_if_benchmark_cancelled(cancellation);
    if (attempt >= 3) throw;
    bool repaired = false;
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
      invalidate_download_artifact(download_request, cancellation, trace, execution, parent);
      downloads[i] = acquire(artifact, owner, true);
      repaired = true;
     }
    }
    if (!repaired) throw;
    for (const auto source : sources) retained.components.erase(std::pair{release.edition, source});
    reusable.clear();
    cached_components.clear();
    prepared.cached_label_inputs.clear();
    reusable_images.clear(); request.reusable_images = {};
    retained_sources.clear();
    request.retained_sources = {};
    identity = annotation_identity();
    request.input_identity = identity;
    progress.source_activity(BenchmarkDatasetSource::kCoconut, "Retrying required COCONut masks after source repair", false);
   }
  }
  for (const auto& component : components)
   if (std::ranges::find(sources, component.source()) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
  for (const auto& component : components) {
   if (!metadata_only && component.index().image_count() != 0 && !reusable.contains(component.source())) store_coconut_component(path_for(component.source()), component, cancellation, &reservations);
  }
 }
 for (std::size_t i = 0; i < downloads.size(); ++i) prepared.manifest["artifacts"][first_artifact + i]["identity"] = downloads[i].identity;
 for (const auto& component : components)
  if (std::ranges::find(sources, component.source()) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
 std::uint64_t admitted_rows = 0;
 for (const auto& component : components) admitted_rows = checked_add(admitted_rows, component.index().image_count(), "COCONut release count overflow");
 if (release.expected_rows != 0 && admitted_rows != release.expected_rows) throw std::runtime_error("COCONut cached release membership is incomplete");
 for (auto& component : components) {
  const auto index_path = path_for(component.source());
  if (!metadata_only || complete)
   prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, coconut_component_storage_bytes(component), "COCONut index storage overflow");
  prepared.manifest["components"].push_back(
   {{"edition", release.edition}, {"revision", release.revision}, {"physical_source", coconut_namespace_name(component.source())}, {"input_identity", component.input_identity()},
    {"recovery_policy", component.recovery_policy()}, {"original_annotation_identity", component.original_annotation_identity()}, {"offered_images", component.index().image_count()},
    {"annotation_source", coconut_annotation_source(component.source())}, {"imported_annotation_sha256", component.index().annotation_sha256},
    {"imported_index", index_path.lexically_relative(cache.root).string()},
    {"imported_index_identity", metadata_only && !complete ? nlohmann::json(nullptr) : nlohmann::json(component.completion()->identity)}});
  if (!metadata_only) retained.components.insert_or_assign({component.edition(), component.source()}, component);
  prepared.components.push_back(std::move(component));
 }
 return prepared;
}
}  // namespace
void retire_coconut_recipe_inputs(const std::shared_ptr<CoconutRecipeInputs>& inputs, const AdmittedRecipeArchive* changed) {
 if (!inputs) return;
 inputs->settle();
 for (std::size_t i = 0; i < inputs->releases.size(); ++i) {
  auto& release = inputs->releases[i];
  if (release.complete) for (auto& component : release.complete->components)
   release.inputs.components.insert_or_assign({component.edition(), component.source()}, std::move(component));
  release.metadata.reset(); release.complete.reset();
  if (!changed) continue;
  const auto affected = [&](const CoconutPhysicalImage& image) { return image.source == changed->origin.source && image.shard == changed->origin.shard; };
  std::uint64_t withdrawn = 0;
  auto& records = *release.inputs.records;
  for (const auto& [key, component] : release.inputs.components) {
   (void)key;
   for (const auto& image : component.inventory())
    withdrawn += affected(image.physical) && image.physical.archive_identity == changed->download.identity;
  }
  for (std::size_t row = 0; row < records.size(); ++row) if (const auto& chunk = records.row(row).native(); chunk && affected(chunk->inventory().physical)) {
   const auto& image = chunk->inventory().physical;
   bool counted = false;
   if (const auto full = release.inputs.components.find({chunk->lineage().component.edition, chunk->lineage().component.source}); full != release.inputs.components.end()) {
    const auto images = full->second.index().images();
    const auto row = std::ranges::lower_bound(images, image.image_id, {}, &NormalizedImage::source_image_id);
    counted = row != images.end() && (*row).source_image_id == image.image_id &&
     full->second.inventory_image(static_cast<std::size_t>(row - images.begin())).physical == image;
   }
   if (!counted && image.archive_identity == changed->download.identity) ++withdrawn;
   records.native(row, {});
  }
  if (inputs->indexing && withdrawn) inputs->indexing->invalidate(i, *inputs->progress, withdrawn);
 }
 inputs->prepare = {};
 inputs->ready.clear(); inputs->consumed = inputs->completed = 0; inputs->next.store(0);
 inputs->activated = false; inputs->error = {};
 inputs->originals_started = inputs->originals_ready;
 inputs->retiring = false; inputs->stopped.store(false);
}
std::shared_ptr<CoconutRecipeInputs> acquire_coconut_recipe_inputs(const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog, ProgressReporter& progress, std::size_t workers,
 mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, std::size_t download_connections, std::span<const int> cpus,
 std::function<void(std::exception_ptr)> failed, BenchmarkCompilePipeline* execution) {
 auto inputs = std::make_shared<CoconutRecipeInputs>(catalog.releases.size(), cancellation, std::move(failed));
 inputs->execution = execution;
 // Release controllers can retain lifecycle leases while awaiting activation.
 // Their transport must already be admitted before any controller starts.
 if (execution) inputs->source_transport = execution->curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources());
 inputs->cache = &cache;
 inputs->catalog = &catalog;
 inputs->progress = &progress;
 inputs->trace = trace;
 inputs->connections = std::max<std::size_t>(1, download_connections ? download_connections : workers);
 inputs->reservations = std::make_unique<StorageReservationPool>(cache.root, trace, execution ? &execution->storage() : nullptr);
 const auto lanes = execution ? catalog.releases.size() : std::min(workers, catalog.releases.size());
 if (lanes) {
  const auto affinity = cpus.empty() ? mmltk::common::system::allowed_cpu_set() : std::vector<int>(cpus.begin(), cpus.end());
  inputs->controllers.reserve(lanes);
  try {
   for (std::size_t lane = 0; lane < lanes; ++lane)
    inputs->controllers.emplace_back([owner = inputs.get(), affinity] {
     try {
      mmltk::common::system::set_thread_affinity(affinity);
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
 mmltk::common::concurrency::CancellationObservation external_cancellation, const BenchmarkTraceSink& trace, bool metadata_only,
 std::shared_ptr<CoconutRecipeInputs> inputs, BenchmarkCompilePipeline* execution, BenchmarkAllowance preparation) {
 using namespace mmltk::common::math;
 const auto cancellation = external_cancellation;
 CoconutRecipePreparation prepared;
 const bool initial_preparation = !inputs || metadata_only;
 auto source_transport = execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources()) : nullptr;
 if (execution && initial_preparation && !preparation) preparation = execution->reserve(BenchmarkResources::handles(0, true, coco_annotation_resources().descriptors + coco_annotation_resources().continuation_descriptors));
 prepared.inputs = inputs ? std::move(inputs) : acquire_coconut_recipe_inputs(cache, catalog, progress, 0, cancellation, trace, workers, {}, {}, execution);
 auto& retained = *prepared.inputs;
 retained.physical = &physical;
 const auto acquisition_workers = std::max<std::size_t>(1, workers);
 const auto parse_workers = acquisition_workers;
 if (initial_preparation) progress.phase(DatasetCompilePhase::Downloading);
 prepared.manifest = {{"dataset", "coconut"}, {"validation", config.selection.validation}, {"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& originals = retained.originals;
 const bool recover_train = config.selection.recover_dropped_masks && std::ranges::any_of(catalog.releases, [](const auto& release) { return release.edition == CoconutEdition::Base; });
 const bool recover_validation = config.selection.recover_dropped_masks && std::ranges::any_of(catalog.releases, [](const auto& release) { return release.edition == CoconutEdition::RelabeledValidation; });
 const bool needs_originals = config.selection.recover_dropped_masks || config.selection.validation == CoconutValidation::Stock;
 if (needs_originals) {
  const bool recover = config.selection.recover_dropped_masks;
  const bool stock = config.selection.validation == CoconutValidation::Stock;
  retained.start_originals([&retained, &cache, &catalog, &progress, execution, trace, recover, stock, recover_train, recover_validation, acquisition_workers, parse_workers, preparation = std::move(preparation)] {
   const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(retained);
   StorageReservationPool reservations(cache.root, trace, execution ? &execution->storage() : nullptr);
   // Declare fixed transport before retaining its dependent source lifecycle.
   auto transport = execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources()) : nullptr;
   trace_benchmark_event(trace, "benchmark.annotations.originals_wait", [] { return nlohmann::json::object(); });
   auto original_lease = ArtifactLease::acquire_charged(cache.locks / "coco-annotations.lifecycle.lock", cancellation, execution, coco_annotation_resources(), preparation);
   const CocoAnnotationRequest selection{
    recover ? CocoSplitAdmission::Optional : CocoSplitAdmission::Unselected,
    stock ? CocoSplitAdmission::Required : CocoSplitAdmission::Optional
   };
   trace_benchmark_event(trace, "benchmark.annotations.originals_begin", [&] { return nlohmann::json{{"recover_dropped_masks", recover}, {"validation", stock ? CoconutValidation::Stock : CoconutValidation::Coconut}}; });
   CocoAnnotationCache annotations(cache, catalog.stock_annotations, selection, 0, checked_cast<std::uint32_t>(catalog.coco_validation_images, "COCO validation count overflow"), static_cast<int>(parse_workers),
    cancellation, trace, execution, &reservations, std::move(original_lease));
   annotations.observe_splits([&retained, recover_train, recover_validation](CocoAnnotationSplit split) {
    const bool selected = split.training ? recover_train : recover_validation;
    retained.original_split(std::move(split), selected);
   },
    {retained.train_originals.state.generation, retained.validation_originals.state.generation});
   annotations.discover(progress);
   if (annotations.pending_download()) {
    const auto& request = *annotations.pending_download();
    try {
     auto archive = download_artifacts({request}, acquisition_workers, cancellation,
      progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress.transfers().update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution, annotations.allowance(), &reservations)
                     .front();
     auto completed = annotations.completed_indexes();
     annotations.settle(std::move(archive), progress, parse_workers, completed, recover ? 2 : 1);
    } catch (const BenchmarkDownloadUnavailable& error) { annotations.download_unavailable(error, progress); }
   }
   retained.originals = annotations.finish();
  });
 } else {
  const std::lock_guard lock(retained.mutex);
  retained.originals_ready = true;
 }
 if (progress.normalization_observer_enabled() && !retained.indexing) {
  std::vector<std::uint64_t> rows;
  rows.reserve(catalog.releases.size());
  for (const auto& release : catalog.releases) rows.push_back(release.expected_rows);
  retained.indexing = progress.indexing(rows);
  if (!rows.empty()) retained.indexing->update(0, 0, progress);
 }
 // Original preparation owns its prerequisite commitment. Release metadata
 // acquires only its own bounded source-consumer envelope and can start now.
 preparation = {};
 retained.activate([&cache, &catalog, &physical, &progress, &failures, &retained, trace, metadata_only, execution, recover = config.selection.recover_dropped_masks](std::size_t index, const BenchmarkAllowance& parent, bool masks) {
  const auto release_cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(retained);
  const auto& release = catalog.releases[index];
  auto& release_inputs = retained.releases[index].inputs;
  if (!masks) {
   if (!metadata_only) return;
   auto metadata = prepare_coconut_release(cache, release, physical, progress, failures, {}, release_inputs,
    release_cancellation, trace, true, retained.indexing, index, execution, parent);
   trace_benchmark_event(trace, "benchmark.annotations.release_metadata", [&] { return nlohmann::json{{"edition", release.edition}, {"cache_hit", metadata.annotation_cache_hit}}; });
   retained.metadata_ready(index, std::move(metadata));
   return;
  }
  const bool eligible_recovery = recover && (release.edition == CoconutEdition::Base || release.edition == CoconutEdition::RelabeledValidation);
  const CoconutOriginalProvider recovery = eligible_recovery ? CoconutOriginalProvider{[&retained](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto split = retained.original(source, wait, stop);
   return CoconutOriginalInput{split.recovery, split.state.terminal, split.state.generation};
  }} : CoconutOriginalProvider{};
  if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MasksStarted);
  retained.releases[index].complete = prepare_coconut_release(cache, release, physical, progress, failures, recovery,
   release_inputs, release_cancellation, trace, false, retained.indexing, index, execution, parent);
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
 if (needs_originals) {
  if (config.selection.validation == CoconutValidation::Stock) (void)retained.original(CoconutImageNamespace::CocoValidation, true);
  if (retained.originals_available()) {
   prepared.annotation_cache_hit = originals.cache_hit;
   prepared.annotation_storage_bytes = originals.retained_storage_bytes;
  } else prepared.annotation_cache_hit = false;
 }
 for (auto& release : releases) {
  prepared.annotation_cache_hit = prepared.annotation_cache_hit && release->annotation_cache_hit;
  prepared.annotation_storage_bytes = checked_add(prepared.annotation_storage_bytes, release->annotation_storage_bytes, "COCONut annotation storage overflow");
  for (auto& value : release->manifest["artifacts"]) prepared.manifest["artifacts"].push_back(std::move(value));
  for (auto& value : release->manifest["components"]) prepared.manifest["components"].push_back(std::move(value));
  prepared.components.insert(prepared.components.end(), std::make_move_iterator(release->components.begin()), std::make_move_iterator(release->components.end()));
 }
 prepared.duplicate_xl_images = reconcile_coconut_extensions(prepared.components, cancellation);
 std::erase_if(prepared.components, [](const CoconutComponent& component) { return component.index().image_count() == 0; });
 if (config.selection.validation == CoconutValidation::Stock) {
  auto original = retained.original(CoconutImageNamespace::CocoValidation, true);
  prepared.stock_validation = std::move(original.state.index);
  prepared.stock_validation_generation = original.state.generation;
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
  if (!coconut_validation_component(component.edition())) {
   const auto source = split_key(component.inventory().front().physical).first;
   auto& membership = train_members[static_cast<std::size_t>(source)];
   membership.reserve(checked_add(membership.size(), component.inventory().size(), "COCONut split membership overflow"));
   for (const auto& image : component.inventory()) {
    throw_if_benchmark_cancelled(cancellation);
    if (!membership.insert(image.physical.image_id).second) throw std::runtime_error("COCONut training contains an unreconciled physical member");
   }
  } else {
   prepared.validation_images = checked_add(prepared.validation_images, component.index().image_count(), "COCONut validation count overflow");
   if (component.source() == CoconutImageNamespace::CocoValidation) coco_val_count = component.index().image_count();
  }
 }
 if (prepared.stock_validation)
  for (const auto& image : prepared.stock_validation->images)
   if (train_members[static_cast<std::size_t>(CoconutImageNamespace::CocoTrain)].contains(image.source_image_id)) throw std::runtime_error("COCONut training reuses a stock validation physical ID");
 if (coco_val_count != catalog.coco_validation_images) throw std::runtime_error("COCONut COCO validation membership is incomplete");
 for (const auto& component : prepared.components) {
  if (coconut_validation_component(component.edition()))
   for (const auto& image : component.inventory())
    if (contains_training(image.physical)) throw std::runtime_error("COCONut train and validation reuse a physical member");
 }
 for (auto& facts : prepared.manifest["components"]) {
  const auto found = std::ranges::find_if(
   prepared.components, [&](const CoconutComponent& component) { return facts["edition"] == component.edition() && facts["physical_source"] == coconut_namespace_name(component.source()); });
  const auto admitted = found == prepared.components.end() ? 0U : found->index().image_count();
  facts["admitted_images"] = admitted;
  facts["covered_by_large_images"] = facts["offered_images"].get<std::uint64_t>() - admitted;
  facts["selected_annotation_sha256"] = found == prepared.components.end() ? nlohmann::json(nullptr) : nlohmann::json(found->index().annotation_sha256);
 }
 for (const auto& admitted : acquired) {
  const auto& archive = admitted.origin;
  prepared.manifest["artifacts"].push_back(
   {{"artifact_id", archive.artifact.artifact_id}, {"url", archive.artifact.url}, {"filename", archive.artifact.filename}, {"expected_size", archive.artifact.expected_size},
    {"expected_sha256", archive.artifact.expected_sha256}, {"physical_source", coconut_namespace_name(archive.source)}, {"identity", admitted.download.identity}, {"bytes", admitted.download.size}});
 }
 if (config.selection.validation == CoconutValidation::Stock || config.selection.recover_dropped_masks)
  prepared.manifest["artifacts"].push_back(
   {{"artifact_id", catalog.stock_annotations.artifact_id}, {"url", catalog.stock_annotations.url}, {"expected_size", catalog.stock_annotations.expected_size},
    {"expected_sha256", catalog.stock_annotations.expected_sha256}});
 std::uint64_t recovered = 0, unresolved = 0;
 for (const auto& component : prepared.components) {
  std::uint64_t component_recovered = 0;
  const bool eligible = component.source() == CoconutImageNamespace::CocoTrain || component.source() == CoconutImageNamespace::CocoValidation;
  std::uint64_t component_unresolved = config.selection.recover_dropped_masks && eligible && !component.recovery_policy() ? component.index().rejected.degenerate_boxes : 0;
  for (std::size_t position = 0; position < component.recovery().size(); ++position) {
   const auto& image = component.recovery()[position];
   const auto& inventory = component.inventory_image(position);
   if (inventory.physical.image_id != image.image_id) throw std::logic_error("recovery report image is absent from component inventory");
   for (const auto& object : image.omissions)
    failures.reject(inventory.physical, inventory.release_image_id, coconut_release_component(component.edition()).name, object.annotation_id, object.source_category_id,
     "thing segment remains without mask pixels or an authoritative bbox");
   component_recovered = checked_add(component_recovered, image.objects.size(), "recovery count overflow");
   component_unresolved = checked_add(component_unresolved, image.unresolved, "recovery omission count overflow");
  }
  recovered = checked_add(recovered, component_recovered, "recovery count overflow");
  unresolved = checked_add(unresolved, component_unresolved, "recovery omission count overflow");
  for (auto& facts : prepared.manifest["components"])
   if (facts["edition"] == component.edition() && facts["physical_source"] == coconut_namespace_name(component.source())) {
    facts["recovered_objects"] = component_recovered;
    facts["unresolved_objects"] = component_unresolved;
   }
 }
 prepared.manifest["recover_dropped_masks"] = config.selection.recover_dropped_masks;
 if (config.selection.recover_dropped_masks && retained.originals_available())
  prepared.manifest["original_annotations"] = {
   {"train", originals.train ? nlohmann::json(originals.train->annotation_sha256) : nlohmann::json(nullptr)},
   {"validation", prepared.stock_validation ? nlohmann::json(prepared.stock_validation->annotation_sha256)
                  : originals.validation    ? nlohmann::json(originals.validation->annotation_sha256)
                                            : nlohmann::json(nullptr)}
  };
 prepared.manifest["recovered_objects"] = recovered;
 prepared.manifest["unresolved_objects"] = unresolved;
 if (config.selection.recover_dropped_masks) progress.activity("COCONut mask recovery: " + std::to_string(recovered) + " recovered, " + std::to_string(unresolved) + " unresolved");
 prepared.manifest["duplicate_xl_images"] = prepared.duplicate_xl_images;
 prepared.manifest["validation_images"] = prepared.validation_images;
 return prepared;
}
}  // namespace mmltk::backend::data::benchmark_internal
