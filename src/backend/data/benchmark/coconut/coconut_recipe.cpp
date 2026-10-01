#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
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
#include "src/common/io/staging_directory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include <array>
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
using mmltk::backend::data::detail::InsufficientCompileResources;
struct CoconutReleaseInputs {
 std::unordered_map<std::string, DownloadResult> artifacts;
 std::shared_ptr<ArtifactLease> lease;
 std::shared_ptr<CoconutAnnotationRecords> records = std::make_shared<CoconutAnnotationRecords>();
 std::map<std::pair<CoconutEdition, CoconutImageNamespace>, CoconutComponent> components;
};
// Opaque outside this owner. A release holds its lifecycle lease only until its
// own parser finishes, never while waiting for another release's lease.
struct CoconutRecipeInputs {
 struct Part {
  CoconutReleaseInputs inputs;
  std::optional<CoconutRecipePreparation> complete;
  std::jthread worker;
  std::uint64_t completed_rows = 0;
  bool metadata_ready = false;
 };
 struct Release {
  CoconutReleaseInputs inputs;
  std::vector<std::unique_ptr<Part>> parts;
  std::optional<CoconutRecipePreparation> complete;
  std::jthread masks;
  std::size_t annotation_descriptors = 1;
  bool labels_published = false, metadata_published = false, unavailable = false, release_cache_selected = false;
  std::shared_ptr<std::mutex> observation_mutex = std::make_shared<std::mutex>();
  std::jthread metadata_reader, mask_discovery;
  std::shared_ptr<BenchmarkArchive> discovered_masks;
 };
 CoconutRecipeInputs(std::size_t count, mmltk::common::concurrency::CancellationObservation cancellation, std::function<void(std::exception_ptr)> failure)
     : releases(count), external(cancellation), failed(std::move(failure)) {}
 ~CoconutRecipeInputs() { settle(); }
 using OriginalChanges = std::array<std::shared_ptr<CompileExecution::CompletionSource>, 2>;
 OriginalChanges stop_locked() {
  stopped.store(true, std::memory_order_relaxed);
  return {std::move(train_originals.changed), std::move(validation_originals.changed)};
 }
 void notify_stopped(const OriginalChanges& stop_changes) {
  for (const auto& notification : stop_changes)
   if (notification) notification->settle();
  changed.notify_all();
  if (execution) execution->execution().notify_admission_change();
 }
 void settle() {
  OriginalChanges stop_changes;
  {
   const std::lock_guard lock(mutex);
   retiring = true;
   stop_changes = stop_locked();
  }
  notify_stopped(stop_changes);
  join_inputs();
 }
 void join_inputs() {
  controllers.clear();
  for (auto& release : releases) {
   release.masks = {};
   release.metadata_reader = {};
   release.mask_discovery = {};
   for (auto& part : release.parts) part->worker = {};
  }
  original_controller = {};
 }
 [[nodiscard]] bool cancelled() const noexcept { return stopped.load(std::memory_order_relaxed) || external.requested(); }
 [[nodiscard]] int descriptor() const noexcept { return external.descriptor(); }
 void fail(std::exception_ptr value) {
  bool first = false;
  OriginalChanges stop_changes;
  {
   const std::lock_guard lock(mutex);
   if (retiring) return;
   first = !error;
   if (first) error = value;
   stop_changes = stop_locked();
  }
  notify_stopped(stop_changes);
  if (first && failed) failed(value);
 }
 void start_originals(std::function<void()> work) {
  if (originals_started) return;
  originals_started = true;
  original_controller = std::jthread([this, work = std::move(work)] {
   try {
    work();
    {
     const std::lock_guard lock(mutex);
     originals_ready = true;
    }
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
  // Snapshots retain their captured completion through replacement, failure,
  // and retirement; the owner detaches it with the facts and settles unlocked.
  std::shared_ptr<CompileExecution::CompletionSource> changed = std::make_shared<CompileExecution::CompletionSource>();
 };
 void original_split(CocoAnnotationSplit value, bool recover) {
  const bool withdrew = !value.index;
  const bool training = value.training;
  const auto generation = value.generation;
  const auto published_index = stock_validation && !training ? value.index : std::nullopt;
  std::shared_ptr<const CoconutRecoveryOriginals> recovery;
  const auto prepare_recovery = [&](std::size_t) {
   if (recover && value.index) recovery = std::make_shared<CoconutRecoveryOriginals>(value.training ? &*value.index : nullptr, value.training ? nullptr : &*value.index, external);
  };
  if (recover && value.index) {
   if (execution)
    execution->execution().run(CompileTaskKind::Metadata, {}, prepare_recovery);
   else
    prepare_recovery(0);
  }
  auto notification = std::make_shared<CompileExecution::CompletionSource>();
  {
   const std::lock_guard lock(mutex);
   const auto& split = training ? train_originals : validation_originals;
   if (value.generation < split.state.generation) return;
  }
  std::exception_ptr failure;
  try {
   // The new snapshot must not authorize recovery before its pipeline generation
   // and stock labels are ready. Retirement can join work, so keep it unlocked.
   if (execution) {
    execution->original_generation(physical->identity_root(training ? CoconutImageNamespace::CocoTrain : CoconutImageNamespace::CocoValidation), generation, withdrew);
    if (published_index) {
     const NormalizedAnnotationReadView index(*published_index);
     const auto root = physical->identity_root(CoconutImageNamespace::CocoValidation);
     for (std::size_t row = 0; row < index.image_count(); ++row) {
      const auto id = index.image(row).source_image_id;
      execution->labels_ready(execution->source_publication(root, {}, id), id, BenchmarkLabelInput(index, row), index.annotation_sha256, generation, 1);
     }
    }
    execution->execution().notify_admission_change();
   }
  } catch (...) { failure = std::current_exception(); }
  {
   const std::lock_guard lock(mutex);
   auto& split = training ? train_originals : validation_originals;
   if (!failure) {
    if (generation < split.state.generation) return;
    split.state = std::move(value);
    split.recovery = std::move(recovery);
   }
   notification = std::exchange(split.changed, std::move(notification));
  }
  if (notification) notification->settle(failure);
  changed.notify_all();
  if (failure) std::rethrow_exception(failure);
 }
 [[nodiscard]] OriginalSplit original(CoconutImageNamespace source, bool wait, std::stop_token stop = {}) {
  const std::stop_callback wake(stop, [this] { changed.notify_all(); });
  std::unique_lock lock(mutex);
  auto& split = source == CoconutImageNamespace::CocoTrain ? train_originals : validation_originals;
  if (error) std::rethrow_exception(error);
  throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
  if (wait) {
   changed.wait(lock, [&] { return split.state.terminal || error || cancelled() || stop.stop_requested(); });
   if (error) std::rethrow_exception(error);
   if (stop.stop_requested()) throw std::runtime_error("COCONut recovery input retired");
   throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
  }
  return split;
 }
 CompileResources release_controls(std::size_t index) const {
  const auto readers = releases[index].annotation_descriptors;
  return CompileResources::handles(1, false, std::max(coco_annotation_resources().continuation_descriptors, readers));
 }
 void acquire_release(std::size_t index) {
  const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
  const auto& release = catalog->releases[index];
  const auto lifecycle = cache->locks / (std::string(release.name) + ".annotations.lifecycle.lock");
  auto lease = ArtifactLease::acquire_charged(lifecycle, cancellation, (execution ? &execution->execution() : nullptr), coco_annotation_resources());
  std::vector<DownloadRequest> requests;
  requests.reserve(release.annotations.size());
  for (const auto& artifact : release.annotations) requests.push_back(make_download_request(*cache, "coconut-" + std::string(release.name), artifact));
  auto& state = releases[index];
  const bool shard_stream = std::ranges::all_of(release.annotations, [](const auto& artifact) { return std::filesystem::path(artifact.filename).extension() == ".parquet"; });
  if (shard_stream) {
   state.parts.reserve(requests.size());
   for (std::size_t part = 0; part < requests.size(); ++part) state.parts.push_back(std::make_unique<Part>());
  }
  const DownloadReadySink ready = [&](DownloadReady value) {
   if (!shard_stream) {
    if (value.artifact.path.extension() == ".json")
     state.metadata_reader = std::jthread([this, index, artifact = std::move(value.artifact), lease] {
      try {
       {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return activated || cancelled(); });
        throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
       }
       CoconutImportRequest request;
       request.reject_invalid_identities = true;
       request.execution = execution;
       request.edition = catalog->releases[index].edition;
       request.metadata_only = true;
       request.inventory_directory = cache->source_indexes("coconut-" + std::string(catalog->releases[index].name));
       request.records = releases[index].inputs.records;
       request.input_identity = artifact.identity;
       request.annotation_json = artifact.path;
       request.physical_membership = physical;
       request.parent_allowance = lease->allowance();
       request.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
       (void)import_coconut_annotations(request);
      } catch (const CoconutInputError&) { /* The complete generation performs bounded repair. */
      } catch (...) { fail(std::current_exception()); }
     });
    else if (release.edition != CoconutEdition::XLarge)
     state.mask_discovery = std::jthread([this, index, artifact = std::move(value.artifact), lease](std::stop_token stop) {
      try {
       CoconutImportRequest request;
       request.reject_invalid_identities = true;
       request.execution = execution;
       request.edition = catalog->releases[index].edition;
       request.mask_archive = artifact.path;
       request.parent_allowance = lease->allowance();
       request.cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
       request.preparation_progress = [&](CoconutPreparationStage, std::uint64_t entries) {
        trace_benchmark_event(trace, "benchmark.annotations.mask_discovery", [&] { return nlohmann::json{{"edition", request.edition}, {"entries", entries}}; });
       };
       releases[index].discovered_masks = discover_coconut_masks(request, stop);
      } catch (const BenchmarkArchiveError&) { /* The complete consumer owns bounded source repair. */
      } catch (...) { fail(std::current_exception()); }
     });
    return;
   }
   auto& part = *state.parts.at(value.request_index);
   part.inputs.artifacts.emplace(requests[value.request_index].artifact_id, std::move(value.artifact));
   part.inputs.lease = lease;
   part.worker = std::jthread([this, index, shard = value.request_index] {
    try {
     {
      std::unique_lock lock(mutex);
      changed.wait(lock, [&] { return activated || cancelled(); });
      throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
     }
     const auto parent = releases[index].parts[shard]->inputs.lease->allowance();
     releases[index].parts[shard]->complete = prepare_part(index, shard, parent);
    } catch (...) { fail(std::current_exception()); }
   });
  };
  const DownloadProgressSink transfer_progress =
   progress->transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress->transfers().update(update, *progress); }} : DownloadProgressSink{};
  std::vector<DownloadResult> results;
  // Old v3 releases have no part caches. Probe only that layout, with ordinary
  // nonblocking artifact admission: a held sibling immediately leaves the
  // independent ready callbacks in charge. Modern part caches keep priority.
  bool release_cache = false, part_cache = false;
  const auto cache_directory = cache->source_indexes("coconut-" + std::string(release.name));
  if (shard_stream && std::filesystem::is_directory(cache_directory)) {
   for (const auto& entry : std::filesystem::directory_iterator(cache_directory)) {
    const auto name = entry.path().filename().string();
    if (!name.ends_with(".normalized.bin.complete.json")) continue;
    if (name.find(".part-") != std::string::npos)
     part_cache = true;
    else
     release_cache = true;
   }
  }
  if (release_cache && !part_cache) {
   for (const auto& request : requests) {
    auto cached = try_cached_download(request, cancellation, trace, execution, lease->allowance(), transfer_progress);
    if (!cached) {
     results.clear();
     break;
    }
    results.push_back(std::move(*cached));
   }
   if (results.size() == requests.size()) {
    for (std::size_t i = 0; i < results.size(); ++i) state.inputs.artifacts.insert_or_assign(requests[i].artifact_id, results[i]);
    {
     std::unique_lock lock(mutex);
     changed.wait(lock, [&] { return activated || cancelled(); });
     throw_if_benchmark_cancelled(cancellation);
    }
    const auto parent = lease->allowance();
    state.inputs.lease = std::move(lease);
    auto cached = prepare_cached_release(index, parent);
    if (cached.annotation_cache_hit) {
     state.complete = std::move(cached);
     state.release_cache_selected = true;
     state.parts.clear();
    } else {
     lease = state.inputs.lease ? std::move(state.inputs.lease) : ArtifactLease::acquire_charged(lifecycle, cancellation, (execution ? &execution->execution() : nullptr), coco_annotation_resources());
     for (std::size_t i = 0; i < results.size(); ++i) ready({i, results[i]});
    }
   }
  }
  if (results.empty())
   results = download_artifacts(
    requests, connections, cancellation, transfer_progress, trace, ready, execution, lease->allowance(), reservations.get(), [&](std::size_t artifact, const BenchmarkDownloadUnavailable& failure) {
    progress->warning("Unavailable COCONut annotation artifact " + requests[artifact].artifact_id + ": " + failure.what());
    const std::lock_guard lock(mutex);
    state.unavailable = true;
   });
  for (std::size_t i = 0; i < requests.size(); ++i) {
   if (results[i].path.empty()) continue;
   if (results[i].path.extension() != ".json" && results[i].path.extension() != ".parquet")
    state.annotation_descriptors = std::max(state.annotation_descriptors, BenchmarkArchive::input_requirement(results[i].path, 0).descriptors);
   state.inputs.artifacts.insert_or_assign(requests[i].artifact_id, std::move(results[i]));
  }
  state.metadata_reader = {};
  state.mask_discovery.request_stop();
  state.mask_discovery = {};
  if (state.discovered_masks) state.inputs.records->archive = std::move(state.discovered_masks);
  {
   const std::lock_guard lock(mutex);
   if (error) std::rethrow_exception(error);
  }
  // Acquisition controllers return after handing the admitted generation to
  // its release worker. Discovery and normalization share that one import.
  if (state.parts.empty())
   state.inputs.lease = std::move(lease);
  else
   lease.reset();
  // Parts own their input read custody. Once parsing has made immutable native
  // products, it must not keep the acquisition controller's lease alive while
  // final physical joins wait for unrelated archives.
  for (auto& part : state.parts) part->worker = {};
  releases[index].masks = std::jthread([this, index] {
   try {
    {
     std::unique_lock lock(mutex);
     changed.wait(lock, [&] { return activated || cancelled(); });
     throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation::Borrow(*this));
    }
    const auto parent = releases[index].inputs.lease ? releases[index].inputs.lease->allowance() : CompileAllowance{};
    if (!releases[index].release_cache_selected) prepare(index, parent);
    releases[index].inputs.lease.reset();
    trace_benchmark_event(
     trace, "benchmark.annotations.release_complete", [&] { return nlohmann::json{{"edition", catalog->releases[index].edition}, {"cache_hit", releases[index].complete->annotation_cache_hit}}; });
    progress->label_plan_completed(index);
    {
     const std::lock_guard lock(mutex);
     releases[index].labels_published = true;
     ++completed;
    }
    changed.notify_all();
   } catch (...) { fail(std::current_exception()); }
  });
 }
 void activate(std::function<void(std::size_t, const CompileAllowance&)> work) {
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
 void finish() {
  {
   std::unique_lock lock(mutex);
   changed.wait(lock, [&] { return error || completed == releases.size(); });
  }
  join_inputs();
  if (error) std::rethrow_exception(error);
  for (std::size_t index = 0; index < releases.size(); ++index) {
   auto& release = releases[index];
   if (!release.complete) continue;
   bool stale = false;
   std::uint64_t affected = 0;
   for (const auto& component : release.complete->components) {
    if (component.source() != CoconutImageNamespace::CocoTrain && component.source() != CoconutImageNamespace::CocoValidation) continue;
    const auto current = original(component.source(), false);
    if (component.original_generation() && component.original_generation() != current.state.generation) {
     stale = true;
     affected += component.index().image_count();
    }
   }
   if (!stale) continue;
   for (const auto& component : release.complete->components)
    if (component.source() != CoconutImageNamespace::CocoTrain && component.source() != CoconutImageNamespace::CocoValidation)
     release.inputs.components.insert_or_assign({component.edition(), component.source()}, component);
   if (indexing) indexing->invalidate(index, *progress, affected);
   const auto cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(*this);
   auto lease = ArtifactLease::acquire_charged(
    cache->locks / (std::string(catalog->releases[index].name) + ".annotations.lifecycle.lock"), cancellation, (execution ? &execution->execution() : nullptr), release_controls(index));
   prepare(index, lease->allowance());
   lease = {};
  }
  prepare = {};
  source_transport.reset();
 }
 std::vector<Release> releases;
 CocoAnnotationIndexes originals;
 OriginalSplit train_originals, validation_originals;
 bool stock_validation = false;
 bool originals_ready = false, originals_started = false;
 const BenchmarkCacheLayout* cache = nullptr;
 const CoconutRecipeCatalog* catalog = nullptr;
 ProgressReporter* progress = nullptr;
 BenchmarkTraceSink trace;
 std::size_t connections = 1;
 std::unique_ptr<detail::StorageReservationPool> reservations;
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
 std::function<void(std::size_t, const CompileAllowance&)> prepare;
 std::function<CoconutRecipePreparation(std::size_t, std::size_t, const CompileAllowance&)> prepare_part;
 std::function<CoconutRecipePreparation(std::size_t, const CompileAllowance&)> prepare_cached_release;
 std::size_t completed = 0;
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
CoconutFailureReport::~CoconutFailureReport() { flush(); }
void CoconutFailureReport::flush_unlocked() {
 if (!dirty_.load(std::memory_order_relaxed)) return;
 if (!pending_.empty()) {
  if (stream_) stream_.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
  pending_.clear();
 }
 if (stream_) stream_.flush();
 dirty_.store(false, std::memory_order_release);
}
void CoconutFailureReport::flush() noexcept {
 if (!dirty_.load(std::memory_order_acquire)) return;
 try {
  const std::lock_guard lock(mutex_);
  flush_unlocked();
 } catch (...) {}
}
void CoconutFailureReport::reject(
 const CoconutPhysicalImage& image, std::uint64_t release_image_id, std::string_view release, std::uint64_t object_id, std::uint64_t category_id, std::string_view reason) noexcept {
 try {
  auto record = nlohmann::json{
   {"image", image.member}, {"image_id", image.image_id}, {"release_image_id", release_image_id}, {"source", coconut_namespace_name(image.source)}, {"release", release}, {"object_id", object_id},
   {"category_id", category_id}, {"reason", reason}
  }.dump();
  record.push_back('\n');
  bool warn = false, writable = false;
  {
   const std::lock_guard lock(mutex_);
   if (!attempted_) {
    attempted_ = true;
    stream_.open(path_, std::ios::app);
   }
   constexpr std::size_t kBatchBytes = 64U * 1024U;
   if (record.size() > kBatchBytes - pending_.size()) flush_unlocked();
   if (stream_) {
    if (record.size() >= kBatchBytes) {
     dirty_.store(true, std::memory_order_release);
     stream_.write(record.data(), static_cast<std::streamsize>(record.size()));
    } else {
     // One lazy batch allocation, reused until report destruction. The logical
     // pending bound is 64 KiB; actual retained storage is pending_.capacity().
     // JSON and serialized record storage remain local to each active producer.
     if (pending_.capacity() < kBatchBytes) pending_.reserve(kBatchBytes);
     pending_.append(record);
     dirty_.store(true, std::memory_order_release);
    }
   }
   writable = static_cast<bool>(stream_);
   warn = !std::exchange(warned_, true);
  }
  if (warn) progress_.warning(std::string("Skipping invalid COCONut objects; ") + (writable ? "details: " : "cannot write failure report: ") + path_.string());
 } catch (...) {
  // I/O, allocation and warning failures never make an omitted object fatal.
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
struct FlushReports {
 CoconutFailureReport& report;
 ~FlushReports() { report.flush(); }
};
void record_coconut_component(
 CoconutRecipePreparation& result, const CoconutReleaseComponent& release, const CoconutComponent& component, const std::filesystem::path& path, const BenchmarkCacheLayout& cache) {
 nlohmann::json incomplete = nlohmann::json::array();
 for (std::size_t i = 0; i < component.index().image_count(); ++i)
  if (!component.source_ordinals_complete(i)) incomplete.push_back(component.index().image(i).source_image_id);
 const auto completion = component.completion();
 if (completion) result.annotation_storage_bytes = mmltk::common::math::checked_add(result.annotation_storage_bytes, coconut_component_storage_bytes(component), "COCONut index storage overflow");
 result.manifest["components"].push_back(
  {{"edition", release.edition}, {"revision", release.revision}, {"physical_source", coconut_namespace_name(component.source())}, {"input_identity", component.input_identity()},
   {"recovery_policy", component.recovery_policy()}, {"original_annotation_identity", component.original_annotation_identity()}, {"offered_images", component.index().image_count()},
   {"annotation_source", coconut_annotation_source(component.source())}, {"incomplete_ordinal_images", std::move(incomplete)}, {"imported_annotation_sha256", component.index().annotation_sha256},
   {"imported_index", path.lexically_relative(cache.root).string()}, {"imported_index_identity", completion ? nlohmann::json(completion->identity) : nlohmann::json(nullptr)}});
}
CoconutRecipePreparation prepare_coconut_release(const BenchmarkCacheLayout& cache, const CoconutReleaseComponent& release, const CoconutPhysicalMembership& physical, ProgressReporter& progress,
 CoconutFailureReport& failures, const CoconutOriginalProvider& original_provider, CoconutReleaseInputs& retained, mmltk::common::concurrency::CancellationObservation cancellation,
 const BenchmarkTraceSink& trace, IndexingProgressTotals* indexing, std::size_t release_index, BenchmarkCompilePipeline* execution, const CompileAllowance& parent, std::size_t annotation_descriptors,
 const std::function<void(CoconutRecipePreparation)>& membership_ready = {}, const std::function<void()>& masks_started = {}, std::string_view cache_tag = {},
 const std::function<void(std::uint64_t)>& row_progress = {}, bool accept_partial = false, const std::function<void(std::uint64_t)>& row_withdrawn = {}, bool cache_only = false) {
 const FlushReports flush_reports{failures};
 using namespace mmltk::common::math;
 CoconutRecipePreparation prepared;
 CompileAllowance consumer_allowance;
 const auto lifecycle = cache.locks / (std::string(release.name) + ".annotations.lifecycle.lock");
 std::weak_ptr<ArtifactLease> source_lease = retained.lease;
 const auto retain_inputs = [&] {
  if (!retained.lease) {
   // EOF returns idle input custody while physical joins settle. Acquisition
   // or a sibling may still own this same lease: share it again instead of
   // waiting on our own release's exclusive lock before publishing this part.
   retained.lease = source_lease.lock();
   if (retained.lease) {
    trace_benchmark_event(trace, "benchmark.annotations.inputs_reused", [&] { return nlohmann::json{{"release", release.name}, {"part", cache_tag}}; });
    return;
   }
   trace_benchmark_event(trace, "benchmark.annotations.inputs_wait", [&] { return nlohmann::json{{"release", release.name}, {"part", cache_tag}}; });
   retained.lease = ArtifactLease::acquire_charged(lifecycle, cancellation, (execution ? &execution->execution() : nullptr), coco_annotation_resources(), parent);
   source_lease = retained.lease;
   trace_benchmark_event(trace, "benchmark.annotations.inputs_acquired", [&] { return nlohmann::json{{"release", release.name}, {"part", cache_tag}}; });
  }
 };
 const auto release_inputs = [&] {
  // A normalized cache hit no longer consumes the discovery archive. Close
  // that retained inode before waiting for independent physical generations;
  // releasing only its lease would leave the reader's descriptor charged.
  retained.records->archive.reset();
  consumer_allowance.release_unused_continuation();
  // A sibling acquisition can stay blocked after this reader reaches EOF.
  // Retain its physical lease, but return idle promises so unrelated inputs
  // can finish. Later readers admit their own complete continuation.
  if (retained.lease) retained.lease->allowance().release_unused_continuation();
  retained.lease.reset();
 };
 retain_inputs();
 prepared.manifest = {{"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& totals = progress.transfers();
 detail::StorageReservationPool reservations(cache.root, storage_observer(trace), execution ? &execution->storage() : nullptr);
 constexpr std::size_t acquisition_workers = 1;
 std::optional<CoconutMaskRecovery> recovery;
 std::shared_ptr<const CoconutRecoveryOriginals> originals;
 const auto recovery_source = release.edition == CoconutEdition::Base ? CoconutImageNamespace::CocoTrain : CoconutImageNamespace::CocoValidation;
 const auto refresh_original = [&](bool wait) {
  if (!original_provider) return CoconutOriginalReadiness{};
  auto input = original_provider(recovery_source, wait, {});
  originals = input.originals;
  recovery.reset();
  if (originals) recovery.emplace(*originals);
  return input;
 };
 const auto initial_original = refresh_original(cache_only);
 const auto acquire = [&](const CatalogArtifact& artifact, std::string_view owner, bool redownload = false) {
  if (!redownload) return retained.artifacts.at(artifact.artifact_id);
  auto request = make_download_request(cache, owner, artifact);
  request.redownload = true;
  auto result = download_artifacts({request}, acquisition_workers, cancellation,
   progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution,
   consumer_allowance, &reservations)
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
  return digest_text(
   nlohmann::json{{"domain", "coconut-annotation-artifacts-v1"}, {"revision", release.revision}, {"normalization", kCoconutNormalizationRevision}, {"artifacts", std::move(artifacts)}}.dump());
 };
 identity = annotation_identity();
 const auto path_for = [&](CoconutImageNamespace source) {
  const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
  const auto suffix = original.empty() ? std::string{} : ".recovery-" + coconut_component_input_identity(identity, source, &*recovery);
  return cache.source_indexes(owner) / (std::string(coconut_namespace_name(source)) + suffix + std::string(cache_tag) + ".normalized.bin");
 };
 std::vector<CoconutComponent> components, reusable_images;
 const auto materialize = [&](std::vector<CoconutComponent> values) {
  auto result = prepared;
  std::uint64_t rows = 0;
  for (const auto& component : values) {
   if (std::ranges::find(sources, component.source()) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
   rows = checked_add(rows, component.index().image_count(), "COCONut cached row count overflow");
   record_coconut_component(result, release, component, path_for(component.source()), cache);
  }
  // Raw import checks offered rows before source-local omissions. Final
  // eligible rows can be a healthy subset of that immutable release inventory.
  result.components = std::move(values);
  return result;
 };
 std::unordered_set<CoconutImageNamespace> reusable;
 bool complete = true;
 std::uint64_t cached_rows = 0;
 const auto observe_rows = [&](std::uint64_t rows) {
  if (row_progress)
   row_progress(rows);
  else if (indexing)
   indexing->update(release_index, rows, progress);
 };
 const auto withdraw_cached = [&](const CoconutComponent& admitted, std::size_t count) {
  if (execution)
   for (std::size_t i = 0; i < count; ++i) {
    const auto& image = admitted.inventory_image(i).physical;
    execution->retire_labels(physical.identity_root(image.source), image.image_id, admitted.image_input_identity(i));
   }
  cached_rows -= count;
  if (row_withdrawn)
   row_withdrawn(count);
  else if (indexing)
   indexing->invalidate(release_index, progress, count);
 };
 for (auto source : sources) {
  if (original_provider && !initial_original.terminal && source == recovery_source) {
   complete = false;
   continue;
  }
  const auto key = std::pair{release.edition, source};
  auto cached = retained.components.find(key);
  if (cached != retained.components.end() && !cached->second.matches_inputs(identity, physical, recovery ? &*recovery : nullptr, false)) {
   retained.components.erase(cached);
   cached = retained.components.end();
  }
  const auto publish_cached = [&](const CoconutComponent& admitted, std::size_t image) {
   const auto bound = original_provider && source == recovery_source ? admitted.with_original(initial_original) : admitted;
   observe_rows(++cached_rows);
   const auto& physical_image = bound.inventory_image(image).physical;
   if (execution)
    execution->labels_ready(physical.label_publication(release.edition, physical_image), physical_image.image_id, bound.labels(image), bound.image_input_identity(image), bound.original_generation(),
     release.edition == CoconutEdition::Large ? 2U : 1U);
   trace_benchmark_event(trace, "benchmark.annotations.cache_row",
    [&] { return nlohmann::json{{"edition", release.edition}, {"source", coconut_namespace_name(source)}, {"image_id", physical_image.image_id}, {"position", image}}; });
  };
  const bool retained_cache = cached != retained.components.end();
  if (cached == retained.components.end()) {
   auto admitted =
    load_coconut_component(path_for(source), release.edition, source, identity, cancellation, false, &physical, recovery ? &*recovery : nullptr, true, {publish_cached, withdraw_cached});
   if (!admitted) {
    complete = false;
    continue;
   }
   cached = retained.components.emplace(key, std::move(*admitted)).first;
   trace_benchmark_event(trace, "benchmark.annotations.component_admitted",
    [&] { return nlohmann::json{{"edition", release.edition}, {"source", coconut_namespace_name(source)}, {"images", cached->second.index().image_count()}}; });
  }
  if (retained_cache)
   for (std::size_t image = 0; image < cached->second.index().image_count(); ++image) publish_cached(cached->second, image);
  if (physical.discovery_complete() && !cached->second.matches_inputs(identity, physical, recovery ? &*recovery : nullptr)) {
   withdraw_cached(cached->second, cached->second.index().image_count());
   complete = false;
   admit_coconut_component(cached->second, cancellation);
   reusable_images.push_back(cached->second);
   continue;
  }
  reusable.insert(source);
  admit_coconut_component(cached->second, cancellation);
  if (original_provider && source == recovery_source) cached->second = cached->second.with_original(initial_original);
  components.push_back(cached->second);
 }
 if (!complete && cache_only) {
  // A release probe is one attempt. A failed namespace cannot leave another
  // namespace's global-ordinal rows active while independent parts take over.
  for (const auto& component : components) withdraw_cached(component, component.index().image_count());
  retained.components.clear();
  prepared.annotation_cache_hit = false;
  return prepared;
 }
 const bool settle_extent = !cache_tag.empty() && complete && std::ranges::any_of(components, [](const auto& component) { return !component.source_extent_complete(); });
 if (!complete || settle_extent) {
  prepared.annotation_cache_hit = complete;
  auto cached_components = std::move(components);
  components.clear();
  progress.source_activity(BenchmarkDatasetSource::kCoconut, (settle_extent ? "Settling source extents for " : "Normalizing required ") + std::string(release.name) + " annotations", false);
  CoconutImportRequest request;
  request.reject_invalid_identities = true;
  request.execution = execution;
  request.edition = release.edition;
  request.inventory_directory = path_for(sources.front()).parent_path();
  request.records = retained.records;
  request.input_finished = release_inputs;
  request.rejected_input = [&](std::string_view reason) {
   prepared.manifest["partial"] = true;
   progress.warning("COCONut " + std::string(release.name) + " retained its healthy annotation prefix: " + std::string(reason));
  };
  std::vector<CoconutImageNamespace> retained_sources(reusable.begin(), reusable.end());
  request.retained_sources = retained_sources;
  request.reusable_images = reusable_images;
  if (retained.records->document_complete && std::ranges::any_of(downloads, [&](const auto& item) { return item.path.extension() == ".json" && retained.records->identity == item.identity; }))
   retained.records->identity = identity;
  request.input_identity = identity;
  request.recovery = recovery ? &*recovery : nullptr;
  std::atomic<std::uint64_t> used_original_generation{0};
  std::atomic<bool> original_changed{false};
  request.originals = !original_provider ? CoconutOriginalProvider{} : CoconutOriginalProvider{[&](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto input = original_provider(source, wait, stop);
   if (input.terminal) {
    auto expected = std::uint64_t{0};
    if (!used_original_generation.compare_exchange_strong(expected, input.generation) && expected != input.generation) {
     original_changed.store(true);
     throw CoconutOriginalChanged{source};
    }
   }
   return input;
  }};
  request.physical_membership = &physical;
  request.trace = trace;
  request.expected_rows = release.expected_rows;
  request.cancellation = cancellation;
  if (membership_ready)
   request.membership_ready = [&](std::vector<CoconutComponent> membership) {
    for (const auto& cached : cached_components) {
     const auto found = std::ranges::find(membership, cached.source(), &CoconutComponent::source);
     if (found != membership.end())
      *found = cached.membership();
     else
      membership.push_back(cached.membership());
    }
    std::ranges::sort(membership, {}, &CoconutComponent::source);
    membership_ready(materialize(std::move(membership)));
   };
  request.image_terminal = [&] { failures.flush(); };
  request.rejected_object = [&](const CoconutPhysicalImage& physical_image, const CoconutRecord& record, const CoconutSegment& segment, std::string_view reason, std::uint32_t policy) {
   if (!policy) failures.reject(physical_image, record.image_id, release.name, segment.id, segment.category_id, reason);
  };
  if (progress.normalization_observer_enabled() || trace) {
   request.preparation_progress = [&](CoconutPreparationStage stage, std::uint64_t completed) {
    const auto activity = [stage]() -> std::string_view {
     switch (stage) {
      case CoconutPreparationStage::Archive: return "Scanning annotation archive";
      case CoconutPreparationStage::Images: return "Reading image metadata";
      case CoconutPreparationStage::Annotations: return "Reading annotation metadata";
      case CoconutPreparationStage::Membership: return "Resolving source images";
     }
     std::unreachable();
    }();
    const bool archive = stage == CoconutPreparationStage::Archive;
    const auto total = archive ? 0 : release.expected_rows;
    const auto unit = archive ? "archive entries" : "rows";
    progress.source_activity(BenchmarkDatasetSource::kCoconut,
     std::string(activity) + " for " + std::string(release.name) + ": " + std::to_string(completed) + " / " + (total ? std::to_string(total) : "?") + " " + unit, false, false);
    trace_benchmark_event(
     trace, "benchmark.annotations.preparation", [&] { return nlohmann::json{{"edition", release.edition}, {"stage", activity}, {"completed", completed}, {"total", total}, {"unit", unit}}; });
   };
  }
  if (progress.normalization_observer_enabled() || trace || row_progress)
   request.progress = [&](std::uint64_t rows) {
    if (row_progress) row_progress(rows);
    if (indexing) indexing->update(release_index, rows, progress);
    trace_benchmark_event(trace, "benchmark.annotations.progress", [&] { return nlohmann::json{{"edition", release.edition}, {"completed_rows", rows}, {"total_rows", release.expected_rows}}; });
   };
  for (const auto& artifact : downloads) {
   if (artifact.path.extension() == ".parquet")
    request.parquet_shards.push_back(artifact.path);
   else if (artifact.path.extension() == ".json")
    request.annotation_json = artifact.path;
   else
    request.mask_archive = artifact.path;
  }
  if (masks_started) masks_started();
  const auto retain_consumer = [&] {
   retain_inputs();
   // Discovery and consumption share the source's complete continuation.
   // A paused archive still owns its file draw from that same reservation;
   // admitting a second full envelope would count its input twice.
   consumer_allowance = retained.lease->allowance();
   request.parent_allowance = consumer_allowance;
  };
  for (unsigned attempt = 1;; ++attempt) {
   try {
    retain_consumer();
    request.accept_partial = accept_partial || attempt >= 3;
    components = import_coconut_annotations(request);
    if (original_provider) {
     const auto current_original = refresh_original(true);
     const bool generation_changed = original_changed.load() || (used_original_generation.load() && used_original_generation.load() != current_original.generation);
     const auto stale =
      generation_changed || std::ranges::any_of(components, [&](const CoconutComponent& component) { return !component.matches_inputs(identity, physical, recovery ? &*recovery : nullptr); });
     if (stale) {
      for (auto& component : components)
       if (component.source() != recovery_source && !reusable.contains(component.source())) {
        reusable.insert(component.source());
        retained_sources.push_back(component.source());
        cached_components.push_back(std::move(component));
       }
      request.retained_sources = retained_sources;
      request.recovery = recovery ? &*recovery : nullptr;
      reusable_images.clear();
      request.reusable_images = {};
      used_original_generation.store(0);
      original_changed.store(false);
      continue;
     }
    }
    break;
   } catch (const CoconutOriginalChanged&) {
    (void)refresh_original(true);
    request.recovery = recovery ? &*recovery : nullptr;
    reusable_images.clear();
    request.reusable_images = {};
    used_original_generation.store(0);
    original_changed.store(false);
    --attempt;
    continue;
   } catch (const std::bad_alloc&) { throw; } catch (const CoconutPhysicalMembershipError&) {
    throw;
   } catch (const InsufficientCompileResources&) { throw; } catch (const detail::InsufficientStorage&) {
    throw;
   } catch (const CoconutInputError& error) {
    // The failed generation cannot retain input backing into artifact repair.
    // This also covers failures after a successful import returned its rows.
    throw_if_benchmark_cancelled(cancellation);
    // A terminal membership observer can reject after EOF released the input
    // lease. Repair reacquires the real source custody before changing files.
    if (!retained.lease) retain_consumer();
    if (attempt >= 3) {
     progress.warning("COCONut annotation artifact exhausted repair: " + std::string(error.what()));
     request.accept_partial = true;
     request.finish_retained = true;
     components = import_coconut_annotations(request);
     break;
    }
    bool repaired = false, unavailable = false;
    mmltk::common::io::StagingDirectory previous(cache.source_downloads(owner) / "annotation", ".", ".repair.XXXXXX", "cannot retain failed annotation input");
    for (std::size_t i = 0; i < release.annotations.size(); ++i) {
     const auto& artifact = release.annotations[i];
     auto download_request = make_download_request(cache, owner, artifact);
     bool matches_expected = false;
     const auto diagnose = [&] {
      const auto& digest = download_failure_sha256(retained.artifacts.at(artifact.artifact_id), cancellation);
      matches_expected = !artifact.expected_sha256.empty() && digest == artifact.expected_sha256;
      trace_benchmark_event(trace, "benchmark.download.failure_sha256",
       [&] { return nlohmann::json{{"artifact", artifact.artifact_id}, {"sha256", digest}, {"matches_expected", matches_expected}, {"reason", error.what()}}; });
     };
     if (!artifact.expected_sha256.empty())
      diagnose();
     else if (trace)
      try {
       diagnose();
      } catch (...) { throw_if_benchmark_cancelled(cancellation); }
     if (!matches_expected) {
      // The failed importer and its row callbacks have unwound. Withdraw once
      // before replacing any artifact on which this release product depends.
      if (!repaired) {
       if (row_withdrawn)
        row_withdrawn(std::numeric_limits<std::uint64_t>::max());
       else if (indexing)
        indexing->invalidate(release_index, progress);
       if (execution)
        for (const auto& row : retained.records->rows()) {
         if (!row.native()) continue;
         const auto& image = row.native()->inventory().physical;
         const auto dependency = coconut_image_input_identity(identity, image, recovery ? recovery->original_identity(image.source) : std::string_view{});
         execution->retire_labels(physical.identity_root(image.source), image.image_id, dependency);
         execution->retire_labels(physical.identity_root(image.source), image.image_id, dependency + ".provisional");
        }
      }
      const auto backup = previous.path() / std::to_string(i);
      std::filesystem::create_hard_link(download_request.destination, backup);
      invalidate_download_artifact(download_request, cancellation, trace, execution, consumer_allowance);
      try {
       downloads[i] = acquire(artifact, owner, true);
       repaired = true;
      } catch (const BenchmarkDownloadUnavailable& unavailable_error) {
       auto mutation =
        ArtifactLease::acquire_charged(download_request.lock_path, cancellation, (execution ? &execution->execution() : nullptr), CompileResources::handles(1, true), consumer_allowance);
       std::filesystem::create_hard_link(backup, download_request.destination);
       progress.warning("COCONut annotation repair unavailable; retaining readable rows: " + std::string(unavailable_error.what()));
       unavailable = true;
       break;
      }
     }
    }
    if (!repaired || unavailable) {
     identity = annotation_identity();
     request.input_identity = identity;
     request.accept_partial = true;
     try {
      components = import_coconut_annotations(request);
     } catch (const CoconutInputError& remaining) {
      progress.warning("COCONut annotation artifact is partially unavailable: " + std::string(remaining.what()));
      request.finish_retained = true;
      components = import_coconut_annotations(request);
     }
     break;
    }
    complete = false;
    prepared.annotation_cache_hit = false;
    for (const auto source : sources) retained.components.erase(std::pair{release.edition, source});
    reusable.clear();
    cached_components.clear();
    reusable_images.clear();
    request.reusable_images = {};
    retained_sources.clear();
    request.retained_sources = {};
    identity = annotation_identity();
    request.input_identity = identity;
    progress.source_activity(BenchmarkDatasetSource::kCoconut, "Retrying required COCONut masks after source repair", false);
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
  for (const auto& component : components)
   if (std::ranges::find(sources, component.source()) == sources.end()) throw std::runtime_error("COCONut release references a namespace outside its selected recipe");
  retain_inputs();
  for (const auto& component : components) {
   if (component.index().image_count() != 0 && !reusable.contains(component.source())) store_coconut_component(path_for(component.source()), component, cancellation, &reservations, trace);
  }
 }
 if (complete) {
  if (row_progress) {
   std::uint64_t rows = 0, admitted = 0;
   for (const auto& component : components) {
    rows = std::max(rows, component.source_extent().first);
    admitted = checked_add(admitted, component.index().image_count(), "COCONut admitted row count overflow");
   }
   row_progress(std::max(rows, admitted));
  }
  release_inputs();
  physical.wait_sources(release.edition);
  for (const auto& component : components)
   for (const auto& image : component.inventory()) {
    try {
     (void)physical.resolve(release.edition, image.physical);
    } catch (const CoconutPhysicalMembershipError&) { /* Final membership excludes missing or ambiguous images. */
    }
   }
  if (std::ranges::any_of(components, [&](const auto& component) { return !component.matches_inputs(identity, physical, recovery ? &*recovery : nullptr); }))
   return prepare_coconut_release(cache, release, physical, progress, failures, original_provider, retained, cancellation, trace, indexing, release_index, execution, parent, annotation_descriptors,
    membership_ready, masks_started, cache_tag, row_progress, accept_partial, row_withdrawn, cache_only);
 }
 if (complete && membership_ready) {
  std::vector<CoconutComponent> membership;
  for (const auto& component : components) membership.push_back(component.membership());
  membership_ready(materialize(std::move(membership)));
 }
 if (complete && masks_started) masks_started();
 for (std::size_t i = 0; i < downloads.size(); ++i) prepared.manifest["artifacts"][first_artifact + i]["identity"] = downloads[i].identity;
 for (const auto& component : components) retained.components.insert_or_assign({component.edition(), component.source()}, component);
 release_inputs();
 return materialize(std::move(components));
}
}  // namespace
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
 inputs->reservations = std::make_unique<detail::StorageReservationPool>(cache.root, storage_observer(trace), execution ? &execution->storage() : nullptr);
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
 mmltk::common::concurrency::CancellationObservation external_cancellation, const BenchmarkTraceSink& trace, std::shared_ptr<CoconutRecipeInputs> inputs, BenchmarkCompilePipeline* execution,
 CompileAllowance preparation, CocoAnnotationInputObserver stock_input) {
 const FlushReports flush_reports{failures};
 using namespace mmltk::common::math;
 const auto cancellation = external_cancellation;
 CoconutRecipePreparation prepared;
 const bool initial_preparation = !inputs;
 auto source_transport = execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources()) : nullptr;
 if (execution && initial_preparation && !preparation)
  preparation = execution->execution().reserve(CompileResources::handles(0, true, coco_annotation_resources().descriptors + coco_annotation_resources().continuation_descriptors));
 prepared.inputs = inputs ? std::move(inputs) : acquire_coconut_recipe_inputs(cache, catalog, progress, 0, cancellation, trace, workers, {}, {}, execution);
 auto& retained = *prepared.inputs;
 retained.physical = &physical;
 const auto acquisition_workers = std::max<std::size_t>(1, workers);
 const auto parse_workers = acquisition_workers;
 if (initial_preparation) progress.phase(DatasetCompilePhase::Downloading);
 prepared.manifest = {{"dataset", "coconut"}, {"validation", config.selection.validation}, {"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}};
 auto& originals = retained.originals;
 const bool recover_train = config.selection.recover_dropped_masks && std::ranges::any_of(catalog.releases, [](const auto& release) { return release.edition == CoconutEdition::Base; });
 const bool recover_validation =
  config.selection.recover_dropped_masks && std::ranges::any_of(catalog.releases, [](const auto& release) { return release.edition == CoconutEdition::RelabeledValidation; });
 const bool needs_originals = config.selection.recover_dropped_masks || config.selection.validation == CoconutValidation::Stock;
 progress.label_plans(catalog.releases.size());
 retained.stock_validation = config.selection.validation == CoconutValidation::Stock;
 if (progress.normalization_observer_enabled() && !retained.indexing) {
  std::vector<std::uint64_t> rows;
  rows.reserve(catalog.releases.size());
  for (const auto& release : catalog.releases) rows.push_back(release.expected_rows);
  retained.indexing = progress.indexing(rows);
  if (!rows.empty()) retained.indexing->update(0, 0, progress);
 }
 if (needs_originals) {
  const bool recover = config.selection.recover_dropped_masks;
  const bool stock = config.selection.validation == CoconutValidation::Stock;
  retained.start_originals([&retained, &cache, &catalog, &progress, execution, trace, recover, stock, recover_train, recover_validation, acquisition_workers, parse_workers,
                            preparation = std::move(preparation), stock_input = std::move(stock_input)] {
   const auto original_cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(retained);
   detail::StorageReservationPool reservations(cache.root, storage_observer(trace), execution ? &execution->storage() : nullptr);
   // Declare fixed transport before retaining its dependent source lifecycle.
   auto transport = execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, original_cancellation, coco_annotation_resources()) : nullptr;
   trace_benchmark_event(trace, "benchmark.annotations.originals_wait", [] { return nlohmann::json::object(); });
   auto original_lease =
    ArtifactLease::acquire_charged(cache.locks / "coco-annotations.lifecycle.lock", original_cancellation, (execution ? &execution->execution() : nullptr), coco_annotation_resources(), preparation);
   const CocoAnnotationRequest selection{recover ? CocoSplitAdmission::Optional : CocoSplitAdmission::Unselected, stock ? CocoSplitAdmission::Required : CocoSplitAdmission::Optional};
   trace_benchmark_event(
    trace, "benchmark.annotations.originals_begin", [&] { return nlohmann::json{{"recover_dropped_masks", recover}, {"validation", stock ? CoconutValidation::Stock : CoconutValidation::Coconut}}; });
   CocoAnnotationCache annotations(cache, catalog.stock_annotations, selection, 0, checked_cast<std::uint32_t>(catalog.coco_validation_images, "COCO validation count overflow"),
    static_cast<int>(parse_workers), original_cancellation, trace, execution, &reservations, std::move(original_lease));
   annotations.observe_input(stock_input);
   annotations.observe_splits([&retained, recover_train, recover_validation](CocoAnnotationSplit split) {
    const bool selected = split.training ? recover_train : recover_validation;
    retained.original_split(std::move(split), selected);
   }, {retained.train_originals.state.generation, retained.validation_originals.state.generation});
   annotations.discover(progress);
   if (annotations.pending_download()) {
    const auto& request = *annotations.pending_download();
    try {
     auto archive = download_artifacts({request}, acquisition_workers, original_cancellation,
      progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { progress.transfers().update(update, progress); }} : DownloadProgressSink{}, trace, {},
      execution, annotations.allowance(), &reservations)
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
 // Original preparation owns its prerequisite commitment. Release metadata
 // acquires only its own bounded source-consumer envelope and can start now.
 preparation = {};
 retained.prepare_part = [&cache, &catalog, &physical, &progress, &failures, &retained, trace, execution, recover = config.selection.recover_dropped_masks](
                          std::size_t index, std::size_t shard, const CompileAllowance& parent) {
  auto release = catalog.releases[index];
  release.annotations = {release.annotations.at(shard)};
  release.expected_rows = 0;
  const bool recovery_selected = recover && (release.edition == CoconutEdition::Base || release.edition == CoconutEdition::RelabeledValidation);
  const CoconutOriginalProvider original_provider = recovery_selected ? CoconutOriginalProvider{[&retained](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto value = retained.original(source, wait, stop);
   return CoconutOriginalReadiness{value.recovery, value.state.terminal, value.state.generation, value.changed->completion()};
  }}
                                                                      : CoconutOriginalProvider{};
  auto& part = *retained.releases[index].parts[shard];
  progress.label_plan_started(index);
  auto result = prepare_coconut_release(cache, release, physical, progress, failures, original_provider, part.inputs, mmltk::common::concurrency::CancellationObservation::Borrow(retained), trace,
   nullptr, index, execution, parent, 1, [&](CoconutRecipePreparation) {
   bool ready = false;
   {
    const std::lock_guard lock(retained.mutex);
    part.metadata_ready = true;
    auto& state = retained.releases[index];
    if (!state.metadata_published && std::ranges::all_of(state.parts, [](const auto& peer) { return peer->metadata_ready; })) {
     state.metadata_published = true;
     ready = true;
    }
   }
   if (ready) {
    trace_benchmark_event(trace, "benchmark.annotations.release_metadata", [&] { return nlohmann::json{{"edition", release.edition}}; });
    if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MetadataConsumed);
   }
  }, [&] {
   if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MasksStarted);
  }, ".part-" + std::to_string(shard), [&](std::uint64_t completed) {
   const std::lock_guard lock(*retained.releases[index].observation_mutex);
   part.completed_rows = std::max(part.completed_rows, completed);
   std::uint64_t total = 0;
   for (const auto& peer : retained.releases[index].parts) total = mmltk::common::math::checked_add(total, peer->completed_rows, "COCONut release progress overflow");
   if (retained.indexing) retained.indexing->update(index, total, progress);
  }, false, [&](std::uint64_t rows) {
   const std::lock_guard lock(*retained.releases[index].observation_mutex);
   const auto withdrawn = std::min(rows, part.completed_rows);
   part.completed_rows -= withdrawn;
   if (retained.indexing) retained.indexing->invalidate(index, progress, withdrawn);
  });
  return result;
 };
 retained.prepare_cached_release = [&cache, &catalog, &physical, &progress, &failures, &retained, trace, execution, recover = config.selection.recover_dropped_masks](
                                    std::size_t index, const CompileAllowance& parent) {
  const auto& release = catalog.releases[index];
  const bool selected = recover && (release.edition == CoconutEdition::Base || release.edition == CoconutEdition::RelabeledValidation);
  const CoconutOriginalProvider original_provider = selected ? CoconutOriginalProvider{[&retained](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto value = retained.original(source, wait, stop);
   return CoconutOriginalReadiness{value.recovery, value.state.terminal, value.state.generation, value.changed->completion()};
  }}
                                                             : CoconutOriginalProvider{};
  progress.label_plan_started(index);
  auto result = prepare_coconut_release(cache, release, physical, progress, failures, original_provider, retained.releases[index].inputs,
   mmltk::common::concurrency::CancellationObservation::Borrow(retained), trace, retained.indexing, index, execution, parent, 1, [&](CoconutRecipePreparation) {
   retained.releases[index].metadata_published = true;
   trace_benchmark_event(trace, "benchmark.annotations.release_metadata", [&] { return nlohmann::json{{"edition", release.edition}}; });
   if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MetadataConsumed);
  }, [&] {
   if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MasksStarted);
  }, {}, {}, false, {}, true);
  if (result.annotation_cache_hit && retained.indexing) {
   std::uint64_t extent = 0, admitted = 0;
   for (const auto& component : result.components) {
    extent = std::max(extent, component.source_extent().first);
    admitted = checked_add(admitted, component.index().image_count(), "COCONut cached release row count overflow");
   }
   retained.indexing->settle(index, std::max(extent, admitted), progress);
  }
  return result;
 };
 retained.activate(
  [&cache, &catalog, &physical, &progress, &failures, &retained, trace, execution, recover = config.selection.recover_dropped_masks](std::size_t index, const CompileAllowance& parent) {
  const auto release_cancellation = mmltk::common::concurrency::CancellationObservation::Borrow(retained);
  const auto& release = catalog.releases[index];
  progress.label_plan_started(index);
  auto& release_inputs = retained.releases[index].inputs;
  const bool eligible_recovery = recover && (release.edition == CoconutEdition::Base || release.edition == CoconutEdition::RelabeledValidation);
  const CoconutOriginalProvider recovery = eligible_recovery ? CoconutOriginalProvider{[&retained](CoconutImageNamespace source, bool wait, std::stop_token stop) {
   auto split = retained.original(source, wait, stop);
   return CoconutOriginalReadiness{split.recovery, split.state.terminal, split.state.generation, split.changed->completion()};
  }}
                                                             : CoconutOriginalProvider{};
  const auto membership = [&](CoconutRecipePreparation metadata) {
   if (retained.releases[index].metadata_published) return;
   retained.releases[index].metadata_published = true;
   trace_benchmark_event(trace, "benchmark.annotations.release_metadata", [&] { return nlohmann::json{{"edition", release.edition}, {"cache_hit", metadata.annotation_cache_hit}}; });
   if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MetadataConsumed);
  };
  auto& parts = retained.releases[index].parts;
  if (retained.releases[index].unavailable && parts.empty()) {
   auto available = release;
   std::erase_if(available.annotations, [&](const auto& artifact) { return !release_inputs.artifacts.contains(artifact.artifact_id); });
   available.expected_rows = 0;
   CoconutRecipePreparation partial;
   partial.annotation_cache_hit = false;
   partial.manifest = {{"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}, {"unavailable", true}};
   if (std::ranges::any_of(available.annotations, [](const auto& artifact) { return std::filesystem::path(artifact.filename).extension() == ".json"; }))
    partial = prepare_coconut_release(cache, available, physical, progress, failures, recovery, release_inputs, release_cancellation, trace, retained.indexing, index, execution, parent,
     retained.releases[index].annotation_descriptors, membership, {}, {}, {}, true);
   partial.manifest["unavailable"] = true;
   membership(partial);
   if (retained.indexing) retained.indexing->settle(index, release_inputs.records->size(), progress);
   retained.releases[index].complete = std::move(partial);
   return;
  }
  CoconutRecipePreparation partial;
  partial.annotation_cache_hit = false;
  partial.manifest = {{"components", nlohmann::json::array()}, {"artifacts", nlohmann::json::array()}, {"partial", true}};
  bool rejected_identity = false;
  if (!parts.empty()) {
   for (std::size_t shard = 0; shard < parts.size(); ++shard) {
    auto& part = *parts[shard];
    if (!part.complete) continue;
    if (std::ranges::any_of(part.complete->components, [&](const auto& component) {
     return !component.matches_physical(physical) || (recovery && component.original_generation() && component.original_generation() != recovery(component.source(), true, {}).generation);
    }))
     part.complete = retained.prepare_part(index, shard, parent);
   }
   std::vector<CoconutComponentPart> products;
   std::optional<std::uint64_t> row_base = 0, segment_base = 0;
   std::uint64_t readable_rows = 0;
   for (const auto& part : parts) {
    // Repair belongs to the independent artifact controller. Its final
    // DownloadResult, rather than the earlier acquisition snapshot, seals the
    // release identity and the next run's release-cache admission.
    for (const auto& [id, artifact] : part->inputs.artifacts) release_inputs.artifacts.insert_or_assign(id, artifact);
    products.push_back({part->complete ? part->complete->components : std::vector<CoconutComponent>{}, row_base, segment_base});
    if (!part->complete) {
     row_base.reset();
     segment_base.reset();
     continue;
    }
    const auto& records = *part->inputs.records;
    std::uint64_t rows = records.size(), segments = 0;
    bool rows_known = bool(records.parquet) || records.document_complete;
    bool segments_known = rows_known && std::ranges::all_of(records.rows(), [](const auto& row) { return row.complete(); });
    if (segments_known)
     for (const auto& row : records.rows()) segments = mmltk::common::math::checked_add(segments, std::uint64_t{row.record().segments.size()}, "COCONut artifact segment count overflow");
    if (!rows)
     for (const auto& component : part->complete->components) {
      const auto extent = component.source_extent();
      rows = std::max(rows, extent.first);
      rows_known = rows_known || rows != 0 || component.source_extent_complete();
      if (component.source_extent_complete()) {
       segments = std::max(segments, extent.second);
       segments_known = true;
      }
     }
    readable_rows = mmltk::common::math::checked_add(readable_rows, rows, "COCONut readable row count overflow");
    if (rows_known && segments_known) products.back().source_extent = std::pair{rows, segments};
    // No zero extent is inferred from a wholly unreadable artifact. Later
    // healthy products retain their own ordinals with explicit provenance.
    if (!rows_known)
     row_base.reset();
    else if (row_base)
     *row_base = mmltk::common::math::checked_add(*row_base, rows, "COCONut artifact row count overflow");
    if (!segments_known)
     segment_base.reset();
    else if (segment_base)
     *segment_base = mmltk::common::math::checked_add(*segment_base, segments, "COCONut artifact segment count overflow");
   }
   if (release.expected_rows && (!row_base || *row_base != release.expected_rows))
    progress.warning("COCONut " + std::string(release.name) + " retained " + std::to_string(readable_rows) + " readable rows from " + std::to_string(release.expected_rows) + " offered rows");
   nlohmann::json artifacts = nlohmann::json::array();
   for (const auto& artifact : release.annotations)
    artifacts.push_back({artifact.artifact_id, release_inputs.artifacts.contains(artifact.artifact_id) ? release_inputs.artifacts.at(artifact.artifact_id).identity : "unavailable"});
   const auto identity = digest_text(
    nlohmann::json{{"domain", "coconut-annotation-artifacts-v1"}, {"revision", release.revision}, {"normalization", kCoconutNormalizationRevision}, {"artifacts", std::move(artifacts)}}.dump());
   const auto directory = cache.source_indexes("coconut-" + std::string(release.name));
   auto merged = merge_coconut_components(
    products, identity, physical, directory, release_cancellation, execution ? &execution->storage() : nullptr, execution, [&](const CoconutInventoryImage& image, std::string_view reason) {
    rejected_identity = true;
    failures.reject(image.physical, image.release_image_id, release.name, 0, 0, reason);
   }, trace);
   failures.flush();
   auto publication = ArtifactLease::acquire_charged(
    cache.locks / (std::string(release.name) + ".annotations.lifecycle.lock"), release_cancellation, (execution ? &execution->execution() : nullptr), coco_annotation_resources());
   release_inputs.components.clear();
   for (auto& component : merged) {
    const auto source_original = recovery ? recovery(component.source(), true, {}) : CoconutOriginalReadiness{};
    std::optional<CoconutMaskRecovery> source_recovery;
    if (source_original.originals) source_recovery.emplace(*source_original.originals);
    const auto suffix =
     component.original_annotation_identity().empty() ? std::string{} : ".recovery-" + coconut_component_input_identity(identity, component.source(), source_recovery ? &*source_recovery : nullptr);
    const auto path = directory / (std::string(coconut_namespace_name(component.source())) + suffix + ".normalized.bin");
    store_coconut_component(path, component, release_cancellation, execution ? &execution->storage() : nullptr, trace);
    record_coconut_component(partial, release, component, path, cache);
    release_inputs.components.insert_or_assign({component.edition(), component.source()}, std::move(component));
   }
  }
  if (!parts.empty()) {
   partial.annotation_cache_hit = std::ranges::all_of(parts, [](const auto& part) { return part->complete && part->complete->annotation_cache_hit; });
   partial.manifest["partial"] =
    rejected_identity || retained.releases[index].unavailable || std::ranges::any_of(parts, [](const auto& part) { return !part->complete || part->complete->manifest.value("partial", false); });
   for (const auto& artifact : release.annotations) {
    const auto found = release_inputs.artifacts.find(artifact.artifact_id);
    partial.manifest["artifacts"].push_back(
     {{"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename},
      {"identity", found == release_inputs.artifacts.end() ? nlohmann::json(nullptr) : nlohmann::json(found->second.identity)}});
   }
   for (const auto& [key, component] : release_inputs.components) {
    (void)key;
    partial.components.push_back(component);
   }
   membership(partial);
   if (retained.indexing) {
    std::uint64_t rows = 0;
    for (const auto& part : parts) rows = checked_add(rows, part->completed_rows, "COCONut release completed row overflow");
    retained.indexing->settle(index, rows, progress);
   }
   retained.releases[index].complete = std::move(partial);
   return;
  }
  retained.releases[index].complete = prepare_coconut_release(cache, release, physical, progress, failures, recovery, release_inputs, release_cancellation, trace, retained.indexing, index, execution,
   parent, retained.releases[index].annotation_descriptors, membership, [&] {
   if (catalog.release_observer) catalog.release_observer(release.edition, CoconutReleaseBoundary::MasksStarted);
  });
  if (retained.indexing) {
   std::uint64_t rows = release_inputs.records->size();
   for (const auto& component : retained.releases[index].complete->components) rows = std::max(rows, component.source_extent().first);
   retained.indexing->settle(index, rows, progress);
  }
 });
 std::vector<std::optional<CoconutRecipePreparation>> releases(catalog.releases.size());
 retained.finish();
 // A later missing-image repair may replace a source shared by releases that
 // already assembled. Reuse their parsed caches while rebinding only stale
 // products; never publish mixed physical generations.
 for (;;) {
  bool refreshed = false;
  for (std::size_t index = 0; index < releases.size(); ++index)
   if (std::ranges::any_of(retained.releases[index].complete->components, [&](const auto& component) { return !component.matches_physical(physical); })) {
    retained.prepare(index, {});
    progress.label_plan_completed(index);
    refreshed = true;
   }
  if (!refreshed) break;
 }
 for (std::size_t index = 0; index < releases.size(); ++index) releases[index] = std::move(retained.releases[index].complete);
 if (needs_originals) {
  if (config.selection.validation == CoconutValidation::Stock) (void)retained.original(CoconutImageNamespace::CocoValidation, true);
  if (retained.originals_available()) {
   prepared.annotation_cache_hit = originals.cache_hit;
   prepared.annotation_storage_bytes = originals.retained_storage_bytes;
  } else
   prepared.annotation_cache_hit = false;
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
 if (coco_val_count > catalog.coco_validation_images) throw std::runtime_error("COCONut COCO validation membership exceeds the release");
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
 physical.wait_sources();
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
  std::uint64_t incomplete_images = 0;
  for (std::size_t position = 0; position < component.index().image_count(); ++position) {
   if (component.source_ordinals_complete(position)) continue;
   ++incomplete_images;
   const auto& inventory = component.inventory_image(position);
   failures.reject(inventory.physical, inventory.release_image_id, coconut_release_component(component.edition()).name, 0, 0,
    "source ordinals remain local to a known annotation extent; an earlier source extent is unavailable (incomplete provenance)");
  }
  if (incomplete_images) {
   failures.flush();
   progress.warning(
    "COCONut " + std::string(coconut_namespace_name(component.source())) + ": " + std::to_string(incomplete_images) + " healthy inventory images have incomplete source-ordinal provenance");
  }
  for (std::size_t position = 0; position < component.recovery().size(); ++position) {
   const auto& image = component.recovery_image(position);
   const auto& inventory = component.inventory_image(position);
   if (inventory.physical.image_id != image.image_id) throw std::logic_error("recovery report image is absent from component inventory");
   for (const auto& object : image.omissions)
    failures.reject(inventory.physical, inventory.release_image_id, coconut_release_component(component.edition()).name, object.annotation_id, object.source_category_id,
     "thing segment remains without mask pixels or an authoritative bbox");
   if (!image.omissions.empty()) failures.flush();
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
