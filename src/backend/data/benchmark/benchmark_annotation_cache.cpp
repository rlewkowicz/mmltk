#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/detail/benchmark_annotation_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/staging_directory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include <unistd.h>
#include <atomic>
#include <future>
#include <mutex>
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
class AnnotationSourceUnavailable final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
[[nodiscard]] std::filesystem::path extract_manifest_path(const std::filesystem::path& path) { return path.string() + ".extract.json"; }
}  // namespace
namespace {
struct AnnotationMember {
 std::string suffix;
 std::filesystem::path output;
 std::string identity{};
 std::exception_ptr failure{};
 std::optional<std::uint64_t> encountered{};
};
void extract_annotation_members(const DownloadResult& archive, std::span<AnnotationMember> members, mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
 StorageReservationPool* storage, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent, const std::function<void(std::size_t, const BenchmarkAllowance&)>& ready) {
 // Keep the stream/output continuation available while independently ready
 // JSON parsers acquire their own index handles and scratch. Warm callbacks
 // cannot consume the descriptor pair needed by the still-missing sibling.
 auto stream_handles = execution ? execution->reserve(BenchmarkResources::handles(1, true, 2), parent) : BenchmarkAllowance{};
 std::size_t remaining = members.size();
 const auto complete = [&](std::size_t index, const BenchmarkAllowance& input) {
  --remaining;
  try {
   if (ready && !members[index].failure) ready(index, input);
  } catch (const AnnotationSourceUnavailable&) { members[index].failure = std::current_exception(); }
 };
 for (std::size_t i = 0; i < members.size(); ++i) {
  auto& member = members[i];
  const auto completion = extract_manifest_path(member.output);
  if (!std::filesystem::is_regular_file(member.output) || !std::filesystem::is_regular_file(completion)) continue;
  try {
   const auto value = read_json_file(completion);
   if (value.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && value.value("complete", false) && value.value("archive_identity", std::string{}) == archive.identity &&
       value.value("member", std::string{}) == member.suffix && value.value("size", 0ULL) == std::filesystem::file_size(member.output))
    member.identity = value.value("identity", std::string{});
  } catch (const std::exception& error) {
   if (is_benchmark_capacity_failure(error)) throw;
  }
  if (!member.identity.empty()) complete(i, parent);
 }
 if (!remaining) return;
 BenchmarkArchive reader(archive.path, execution, 0, stream_handles);
 while (remaining && reader.next(cancellation)) {
  const auto found = std::ranges::find_if(members, [&](const auto& member) { return reader.member().ends_with(member.suffix); });
  if (found == members.end()) continue;
  if (found->encountered && *found->encountered != reader.position()) throw AnnotationSourceUnavailable("benchmark annotation archive has conflicting required identities");
  found->encountered = reader.position();
  // An independently admitted extracted member can appear once while this
  // walk resolves its sibling. Only a second physical occurrence conflicts.
  if (!found->identity.empty() || found->failure) continue;
  const auto index = static_cast<std::size_t>(found - members.begin());
  auto& member = *found;
  try {
   reader.require_regular(std::numeric_limits<std::size_t>::max());
  } catch (const BenchmarkArchiveError& error) {
   member.failure = std::make_exception_ptr(AnnotationSourceUnavailable(error.what()));
   complete(index, reader.allowance());
   continue;
  }
  if (!reader.size()) {
   member.failure = std::make_exception_ptr(AnnotationSourceUnavailable("empty benchmark annotation archive member"));
   complete(index, reader.allowance());
   continue;
  }
  try {
   // The retained JSON is the required parse input; write it once. Only the
   // archive member is materialized, and publication precedes its parse callback.
   const auto bytes = reader.size();
   std::filesystem::create_directories(member.output.parent_path());
   StorageReservationPool destination(member.output, trace, storage);
   auto staging = BenchmarkStagedArtifact::create(destination, member.output, bytes, "extracted annotation staging");
   throw_if_benchmark_cancelled(cancellation);
   staging.preallocate(bytes);
   reader.consume(bytes, [&](std::span<const std::uint8_t> block, std::uint64_t offset) {
    staging.file().pwrite_all(block.data(), block.size(), offset);
    staging.reconcile();
   }, cancellation);
   staging.file().sync_data();
   staging.publish(member.output, cancellation);
   const auto material = archive.identity + "\n" + member.suffix + "\n" + std::to_string(bytes);
   member.identity = common_io::sha256_hex(common_io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
   write_json_atomically(extract_manifest_path(member.output),
    nlohmann::json{
     {"schema_version", kBenchmarkCacheSchemaVersion}, {"complete", true}, {"archive_identity", archive.identity}, {"member", member.suffix}, {"size", bytes}, {"identity", member.identity},
     {"integrity_mode", "archive_structure_size"}
    },
    cancellation, &destination);
  } catch (const BenchmarkArchiveError& error) { throw AnnotationSourceUnavailable(error.what()); }
  complete(index, reader.allowance());
 }
 for (auto& member : members)
  if (member.identity.empty() && !member.failure) member.failure = std::make_exception_ptr(AnnotationSourceUnavailable("benchmark annotation archive does not contain " + member.suffix));
}
}  // namespace
std::string extract_archive_member(const std::filesystem::path& archive_path, std::string_view suffix, const std::filesystem::path& output_path, std::string_view identity,
 const std::filesystem::path& lock_path, mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, StorageReservationPool* storage,
 BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
 auto lease = ArtifactLease::acquire_charged(lock_path, cancellation, execution, BenchmarkResources::handles(1, true, 3), parent);
 AnnotationMember member{std::string(suffix), output_path};
 DownloadResult archive;
 archive.path = archive_path;
 archive.identity = identity;
 try {
  extract_annotation_members(archive, std::span(&member, 1), cancellation, trace, storage, execution, lease->allowance(), {});
 } catch (const BenchmarkArchiveError& error) { throw AnnotationSourceUnavailable(error.what()); }
 if (member.failure) std::rethrow_exception(member.failure);
 return member.identity;
}
[[nodiscard]] std::optional<NormalizedAnnotationIndex> discover_cached_index(const std::filesystem::path& path, const BenchmarkDatasetSource source, const std::string_view split,
 mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace) {
 try {
  std::uint64_t extent = 0;
  const nlohmann::json manifest = read_json_file(path.string() + ".complete.json", &extent);
  const std::string digest = manifest.at("annotation_sha256").get<std::string>();
  return load_normalized_annotation_index(path, source, split, digest, cancel_requested, trace, &manifest, false, extent);
 } catch (const std::exception& error) {
  if (is_benchmark_capacity_failure(error)) throw;
  throw_if_benchmark_cancelled(cancel_requested);
  return std::nullopt;
 }
}
[[nodiscard]] NormalizedAnnotationIndex load_or_build_index(const BenchmarkCacheLayout& cache, const std::filesystem::path& path, const BenchmarkDatasetSource source, const std::string_view split,
 const std::string_view annotation_sha256, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace,
 const std::function<NormalizedAnnotationIndex(const BenchmarkAllowance&)>& builder, StorageReservationPool* storage, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
 auto lease = ArtifactLease::acquire_charged(
  cache.locks / (std::string(benchmark_source_name(source)) + "-" + std::string(split) + ".index.lock"), cancel_requested, execution, BenchmarkResources::handles(2), parent);
 if (auto cached = load_normalized_annotation_index(path, source, split, annotation_sha256, cancel_requested, trace)) { return std::move(*cached); }
 NormalizedAnnotationIndex index = builder(lease->allowance());
 index.completion = store_normalized_annotation_index(path, index, cancel_requested, trace, storage);
 return index;
}
// Runs an annotation indexing step with up to three attempts, invoking repair (cache invalidation
// plus artifact re-download) between attempts. Cancellation always rethrows immediately.
void retry_annotation_indexing(mmltk::common::concurrency::CancellationObservation cancel_requested, const std::function<void()>& body, const std::function<void(const std::exception&)>& repair) {
 for (std::uint32_t attempt = 1U; attempt <= 3U; ++attempt) {
  try {
   body();
   break;
  } catch (const InsufficientBenchmarkResources&) { throw; } catch (const InsufficientBenchmarkStorage&) {
   throw;
  } catch (const std::exception& error) {
   throw_if_benchmark_cancelled(cancel_requested);
   if (attempt == 3U) { throw; }
   repair(error);
  }
 }
}
std::vector<DownloadResult> repair_annotation_artifacts(std::vector<DownloadRequest> requests, BenchmarkDatasetSource source, std::string_view reason, ProgressReporter& progress,
 ArtifactProgressTotals& totals, std::size_t workers, mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, BenchmarkCompilePipeline* execution,
 const BenchmarkAllowance& parent, StorageReservationPool* storage) {
 for (auto& request : requests) {
  if (trace) try {
    if (std::filesystem::is_regular_file(request.destination)) {
     const auto sha = common_io::sha256_hex(common_io::sha256_file(request.destination, [&] { return cancellation.requested(); }));
     trace_benchmark_event(trace, "benchmark.download.failure_sha256", [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", sha}, {"reason", reason}}; });
    }
   } catch (...) { throw_if_benchmark_cancelled(cancellation); }
  invalidate_download_artifact(request, cancellation, trace, execution, parent);
  request.redownload = true;
 }
 progress.source_activity(source, "Redownloading annotation metadata after structural validation failure");
 return download_artifacts(requests, requests.size() == 1U ? workers : std::min<std::size_t>({3U, requests.size(), workers}), cancellation,
  progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution, parent,
  storage);
}
BenchmarkResources coco_annotation_resources() { return BenchmarkResources::handles(1, true, 3); }
CocoAnnotationCache::CocoAnnotationCache(const BenchmarkCacheLayout& cache, const CatalogArtifact& artifact, CocoAnnotationRequest selection, std::uint32_t train_count, std::uint32_t validation_count,
 int parse_workers, mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, BenchmarkCompilePipeline* execution, StorageReservationPool* storage,
 std::shared_ptr<ArtifactLease> custody)
    : cache_(cache),
      request_(make_download_request(cache, "coco", artifact)),
      selection_(selection),
      train_count_(train_count),
      validation_count_(validation_count),
      parse_workers_(parse_workers),
      cancellation_(cancellation),
      trace_(trace),
      source_transport_(execution ? execution->curl().channel(BenchmarkCurl::Class::Artifact, cancellation, coco_annotation_resources()) : nullptr),
      lease_(custody ? std::move(custody) : ArtifactLease::acquire_charged(cache.locks / "coco-annotations.lifecycle.lock", cancellation, execution, coco_annotation_resources())),
      execution_(execution),
      storage_(execution ? &execution->storage() : storage) {
 indexes_.train_path = cache.source_indexes("coco") / "train2017.normalized.bin";
 indexes_.validation_path = cache.source_indexes("coco") / "val2017.normalized.bin";
}
void CocoAnnotationCache::publish_split(bool training, bool terminal) {
 if (split_sink_) split_sink_({training, split_generations_[training ? 0 : 1], terminal, training ? indexes_.train : indexes_.validation});
}
void CocoAnnotationCache::withdraw_split(bool training) {
 (training ? indexes_.train : indexes_.validation).reset();
 ++split_generations_[training ? 0 : 1];
 publish_split(training, false);
}
void CocoAnnotationCache::discover(ProgressReporter& progress) {
 std::array<std::future<void>, 2> readers;
 for (const bool training : {true, false}) {
  if ((training ? selection_.train : selection_.validation) == CocoSplitAdmission::Unselected) continue;
  readers[training ? 0 : 1] = std::async(std::launch::async, [&, training] {
   progress.source_activity(BenchmarkDatasetSource::kCoco2017, training ? "Validating cached COCO train annotation index" : "Validating cached COCO validation annotation index");
   auto& index = training ? indexes_.train : indexes_.validation;
   const auto read = [&](std::size_t) {
    index = discover_cached_index(training ? indexes_.train_path : indexes_.validation_path, BenchmarkDatasetSource::kCoco2017, training ? "train2017" : "val2017", cancellation_, trace_);
   };
   if (execution_) {
    auto input = execution_->reserve(BenchmarkResources::handles(1), lease_->allowance());
    execution_->run(BenchmarkStage::Metadata, {}, read, input);
   } else
    read(0);
   if (index) publish_split(training, true);
  });
 }
 std::exception_ptr failure;
 for (auto& reader : readers)
  if (reader.valid()) {
   try {
    reader.get();
   } catch (...) {
    if (!failure) failure = std::current_exception();
   }
  }
 if (failure) std::rethrow_exception(failure);
 indexes_.cache_hit =
  (selection_.train == CocoSplitAdmission::Unselected || indexes_.train.has_value()) && (selection_.validation == CocoSplitAdmission::Unselected || indexes_.validation.has_value());
 if (!indexes_.cache_hit) pending_ = request_;
}
std::uint64_t CocoAnnotationCache::completed_indexes() const noexcept { return static_cast<std::uint64_t>(indexes_.train.has_value()) + static_cast<std::uint64_t>(indexes_.validation.has_value()); }
std::filesystem::path CocoAnnotationCache::source_json(bool training) const {
 return cache_.source_indexes("coco") / "source-json" / (training ? "instances_train2017.json" : "instances_val2017.json");
}
void CocoAnnotationCache::build_split(bool training, const std::string& digest, ProgressReporter& progress, const BenchmarkAllowance& input) {
 auto& index = training ? indexes_.train : indexes_.validation;
 if (index) return;
 const std::string split = training ? "train2017" : "val2017";
 const std::string label = training ? "train" : "validation";
 const auto json_path = source_json(training);
 std::filesystem::create_directories(json_path.parent_path());
 index = load_or_build_index(
  cache_, training ? indexes_.train_path : indexes_.validation_path, BenchmarkDatasetSource::kCoco2017, split, digest, cancellation_, trace_, [&](const BenchmarkAllowance& input_allowance) {
  progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO " + label + " annotations");
  if (input_observer_) input_observer_(training, CocoAnnotationInputBoundary::Parsing);
  try {
   return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
    AnnotationParseOptions{BenchmarkDatasetSource::kCoco2017, split, training ? train_count_ : validation_count_, parse_workers_, !training, cancellation_, trace_, execution_, input_allowance});
  } catch (const AnnotationDocumentRejected& error) {
   throw_if_benchmark_cancelled(cancellation_);
   throw AnnotationSourceUnavailable(error.what());
  }
 }, storage_, execution_, input);
 publish_split(training, true);
}
void CocoAnnotationCache::invalidate_missing() {
 for (const bool training : {true, false}) {
  if (((training ? selection_.train : selection_.validation) == CocoSplitAdmission::Unselected) || (training ? indexes_.train.has_value() : indexes_.validation.has_value())) continue;
  const auto json = source_json(training);
  const auto& path = training ? indexes_.train_path : indexes_.validation_path;
  remove_cache_path(json.string() + ".extract.json");
  remove_cache_path(json);
  remove_normalized_annotation_index(path);
 }
}
void CocoAnnotationCache::settle(DownloadResult archive, ProgressReporter& progress, std::size_t workers, std::uint64_t& completed, std::uint64_t total) {
 if (!pending_) throw std::logic_error("COCO annotation cache has no pending download");
 for (unsigned attempt = 1; attempt <= 3; ++attempt) {
  std::vector<AnnotationMember> members;
  std::vector<bool> training;
  for (const bool train : {false, true}) {
   if ((train ? selection_.train : selection_.validation) == CocoSplitAdmission::Unselected || (train ? indexes_.train : indexes_.validation)) continue;
   members.push_back({std::string("annotations/instances_") + (train ? "train2017" : "val2017") + ".json", source_json(train)});
   training.push_back(train);
  }
  if (members.empty()) break;
  std::exception_ptr stream_error;
  std::atomic<std::uint64_t> completed_count{completed};
  std::mutex completion_mutex;
  std::vector<std::future<void>> parsers(members.size());
  try {
   for (const bool train : training) {
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, std::string("Extracting COCO ") + (train ? "train" : "validation") + " annotations");
    if (input_observer_) input_observer_(train, CocoAnnotationInputBoundary::Extracting);
   }
   extract_annotation_members(archive, members, cancellation_, trace_, storage_, execution_, lease_->allowance(), [&](std::size_t i, const BenchmarkAllowance& input) {
    const bool train = training[i];
    const auto identity = members[i].identity;
    parsers[i] = std::async(std::launch::async, [&, train, identity, input] {
     build_split(train, identity, progress, input);
     const std::lock_guard lock(completion_mutex);
     progress.phase(DatasetCompilePhase::Indexing, completed_count.fetch_add(1) + 1, total);
    });
   });
  } catch (const BenchmarkArchiveError& error) { stream_error = std::make_exception_ptr(AnnotationSourceUnavailable(error.what())); } catch (const AnnotationSourceUnavailable&) {
   stream_error = std::current_exception();
  }
  // The stream has closed before joining a parser waiting for oversized scratch.
  // Every future settles before split/index ownership can unwind or repair.
  std::exception_ptr fatal;
  for (std::size_t i = 0; i < parsers.size(); ++i)
   if (parsers[i].valid()) {
    try {
     parsers[i].get();
    } catch (const AnnotationSourceUnavailable&) { members[i].failure = std::current_exception(); } catch (...) {
     if (!fatal) fatal = std::current_exception();
    }
   }
  completed = completed_count.load();
  if (fatal) std::rethrow_exception(fatal);
  std::exception_ptr failure = stream_error;
  for (const auto& member : members)
   if (member.failure) failure = member.failure;
  if (!failure) break;
  throw_if_benchmark_cancelled(cancellation_);
  const auto withdraw_attempt = [&] {
   // Only splits absent at the beginning of this archive attempt can depend
   // on its bytes. Joined parsers cannot republish a withdrawn generation.
   for (const bool train : training) {
    auto& index = train ? indexes_.train : indexes_.validation;
    if (index) {
     withdraw_split(train);
     --completed;
    }
   }
   invalidate_missing();
  };
  if (stream_error) withdraw_attempt();
  if (attempt == 3) {
   if ((selection_.train == CocoSplitAdmission::Required && !indexes_.train) || (selection_.validation == CocoSplitAdmission::Required && !indexes_.validation)) std::rethrow_exception(failure);
   warn_unavailable(progress);
   break;
  }
  invalidate_missing();
  try {
   try {
    std::rethrow_exception(failure);
   } catch (const std::exception& error) {
    archive =
     repair_annotation_artifacts({request_}, BenchmarkDatasetSource::kCoco2017, error.what(), progress, progress.transfers(), workers, cancellation_, trace_, execution_, lease_->allowance(), storage_)
      .front();
   }
  } catch (const BenchmarkDownloadUnavailable& error) {
   download_unavailable(error, progress);
   break;
  }
  // Successful repair replaces the shared archive even when the consumed
  // stream was valid and only a sibling parser rejected its member. A valid
  // newly built sibling must be read from these replacement bytes as well.
  if (!stream_error) withdraw_attempt();
 }
 settle_missing_splits();
}
void CocoAnnotationCache::warn_unavailable(ProgressReporter& progress) {
 if (warned_unavailable_) return;
 warned_unavailable_ = true;
 progress.activity("Optional COCO originals unavailable; dropped masks remain omitted");
}
void CocoAnnotationCache::download_unavailable(const BenchmarkDownloadUnavailable& error, ProgressReporter& progress) {
 throw_if_benchmark_cancelled(cancellation_);
 if ((selection_.train == CocoSplitAdmission::Required && !indexes_.train) || (selection_.validation == CocoSplitAdmission::Required && !indexes_.validation)) throw error;
 settle_missing_splits();
 warn_unavailable(progress);
}
void CocoAnnotationCache::settle_missing_splits() {
 pending_.reset();
 for (const bool training : {true, false})
  if ((training ? selection_.train : selection_.validation) != CocoSplitAdmission::Unselected && !(training ? indexes_.train : indexes_.validation)) publish_split(training, true);
}
CocoAnnotationIndexes CocoAnnotationCache::finish() {
 if (pending_ || (selection_.validation == CocoSplitAdmission::Required && !indexes_.validation) || (selection_.train == CocoSplitAdmission::Required && !indexes_.train))
  throw std::logic_error("COCO annotation indexes are not settled");
 std::uint64_t storage = 0;
 const auto account = [&](const std::filesystem::path& path) {
  if (std::filesystem::is_regular_file(path)) storage = common_math::checked_add(storage, std::filesystem::file_size(path), "stock annotation storage overflow");
 };
 account(request_.destination);
 account(request_.destination.string() + ".download.json");
 for (const bool training : {true, false}) {
  if ((training ? selection_.train : selection_.validation) == CocoSplitAdmission::Unselected) continue;
  const auto& path = training ? indexes_.train_path : indexes_.validation_path;
  const auto json = source_json(training);
  const auto& index = training ? indexes_.train : indexes_.validation;
  if (index && index->completion)
   storage = common_math::checked_add(storage, common_math::checked_add(index->completion->size, index->completion->proof_bytes, "stock index storage overflow"), "stock annotation storage overflow");
  else {
   account(path);
   account(path.string() + ".complete.json");
  }
  account(json);
  account(json.string() + ".extract.json");
 }
 indexes_.retained_storage_bytes = storage;
 auto result = std::move(indexes_);
 lease_.reset();
 source_transport_.reset();
 return result;
}
}  // namespace mmltk::backend::data::benchmark_internal
