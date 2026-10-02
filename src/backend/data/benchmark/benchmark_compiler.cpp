#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include <list>
#include <set>
#include <ranges>
#include <condition_variable>
#include <deque>
#include <stop_token>
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include "src/backend/data/benchmark/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_file.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/concurrency.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/path_publication.h"
#include "src/common/io/staging_directory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/types/utf8.h"
// CLEANUP-IGNORE: This benchmark compiler imports and includes the concrete owners directly used by its implementation.
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_sampling.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_compiler.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_recipe.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/backend/data/benchmark/detail/benchmark_cancellation.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/open_images_acquisition.h"
#include "src/backend/data/detail/mask_rle_utils.h"
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileExecutionLimits;
using mmltk::backend::data::detail::CompileResources;
using mmltk::backend::data::detail::CompileTaskKind;
using mmltk::backend::data::detail::InsufficientCompileResources;
namespace mmltk::backend::data {
namespace common_concurrency = mmltk::common::concurrency;
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace common_system = mmltk::common::system;
namespace benchmark_internal {
BenchmarkTraceSink make_trace_sink(const BenchmarkTraceCallback& callback) noexcept {
 if (!callback) { return {}; }
 try {
  const auto mutex = std::make_shared<std::mutex>();
  return [callback, mutex](const std::string_view event, const nlohmann::json& fields) noexcept {
   std::string serialized;
   try {
    if (fields.is_object()) { serialized = fields.dump(); }
   } catch (...) {
    // Empty fields retain encoding failure rather than claiming success.
   }
   try {
    const std::lock_guard lock(*mutex);
    callback(event, serialized);
   } catch (...) {
    // Delivery is attempted once, even if the callback throws.
   }
  };
 } catch (...) {
  try {
   callback({}, {});
  } catch (...) {}
  return {};
 }
}
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kArchivePipelineConcurrency = 4U;
struct TracePath final {
 std::string text;
 bool truncated = false;
 bool utf8_replaced = false;
};
[[nodiscard]] TracePath project_trace_path(const std::string_view path) {
 // Each value uses at most 6 KiB after JSON escaping. The two values and
 // fixed fields fit below 13 KiB, leaving over 3 KiB for the runtime envelope.
 constexpr std::size_t kEscapedPathBudget = 6U * 1024U;
 TracePath result;
 result.text.reserve(std::min(path.size(), kEscapedPathBudget));
 std::size_t escaped_size = 0U;
 std::size_t offset = 0U;
 while (offset < path.size()) {
  const auto byte = static_cast<unsigned char>(path[offset]);
  const auto length = mmltk::common::types::utf8_prefix_length(path.substr(offset));
  const bool valid = length != 0U;
  // Invalid native bytes become U+FFFD; valid UTF-8 is kept whole. The
  // replacement flag distinguishes this lossy diagnostic from the path.
  const std::string_view token = valid ? path.substr(offset, length) : std::string_view{"\xef\xbf\xbd"};
  const std::size_t escaped_cost = byte < 0x20U ? 6U : byte == '"' || byte == '\\' ? 2U : token.size();
  if (escaped_cost > kEscapedPathBudget - escaped_size) { break; }
  result.text.append(token);
  result.utf8_replaced = result.utf8_replaced || !valid;
  escaped_size += escaped_cost;
  offset += valid ? length : 1U;
 }
 result.truncated = offset != path.size();
 return result;
}
[[nodiscard]] std::string output_lock_identity(const std::filesystem::path& normalized_output) {
 const std::string identity_material = normalized_output.generic_string();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
}
[[nodiscard]] bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
 auto parent_iterator = parent.begin();
 auto child_iterator = child.begin();
 for (; parent_iterator != parent.end() && child_iterator != child.end(); ++parent_iterator, ++child_iterator) {
  if (*parent_iterator != *child_iterator) { return false; }
 }
 return parent_iterator == parent.end();
}
DownloadResult acquire_physical_archive(const AdmittedRecipeArchive& admitted, const BenchmarkCacheLayout& cache, ProgressReporter& progress, ArtifactProgressTotals& totals,
 detail::StorageReservationPool& storage, std::size_t workers, common_concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, BenchmarkCompilePipeline* execution) {
 const auto& artifact = admitted.origin.artifact;
 const auto owner = benchmark_source_name(artifact.source);
 auto request = make_download_request(cache, owner, artifact);
 return download_artifacts({request}, workers, cancellation,
  progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const auto& update) { totals.update(update, progress); }} : DownloadProgressSink{}, trace, {}, execution, {}, &storage)
  .front();
}
class RequiredImageDecodeError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
[[nodiscard]] CachedImageDirectory acquire_archive_images(const BenchmarkCacheLayout& cache, const BenchmarkDatasetSource source, std::string shard, const std::vector<std::uint64_t>& expected_ids,
 const CatalogArtifact& artifact, mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress, ArtifactProgressTotals* transfer_progress,
 detail::StorageReservationPool* storage_reservations, const std::uint64_t source_total_images, const std::size_t decompression_workers, const std::size_t download_connections,
 const BenchmarkTraceSink& trace, const std::optional<ImageDecodeProbe> decode_probe = std::nullopt, const DownloadResult* initial_download = nullptr,
 std::shared_ptr<ArtifactLease> initial_lease = {}, BenchmarkCompilePipeline* execution = nullptr) {
 if (expected_ids.empty()) { throw std::runtime_error("benchmark archive extraction cannot have an empty image selection"); }
 const std::string source_name(benchmark_source_name(source));
 const std::string archive_name = source_name + " " + shard;
 const std::filesystem::path image_root = cache.source_images(source_name) / shard;
 progress->source_activity(source, "Waiting for " + archive_name + " extraction lock");
 auto extraction_lease = initial_lease ? std::move(initial_lease)
                                       : ArtifactLease::acquire_charged(
                                          cache.locks / (source_name + "-" + shard + ".images.lock"), cancel_requested, (execution ? &execution->execution() : nullptr), archive_image_resources());
 const auto& source_handle = extraction_lease->allowance();
 const auto publication =
  execution ? execution->source_publication(image_root, extraction_lease, decode_probe ? std::optional(decode_probe->image_id) : std::nullopt, true) : BenchmarkSourcePublication{};
 const CachedImageReadySink publish_image = [&, publication](const CachedImageReady& ready) {
  if (decode_probe && ready.image_id == decode_probe->image_id && ready.payload) {
   // Bound writers report decode health through the same physical completion
   // window as ordinary extraction. A source controller must remain free to
   // quiesce its archive scratch while an exceptional replacement is admitted.
   if (execution && execution->image_slot(image_root, ready.image_id)) {
    publication(ready);
    return;
   }
   try {
    if (!publication.consume(ready)) {
     if (execution) {
      BenchmarkImageDecoder decoder;
      const auto header = decoder.read_header(ready.payload->encoded(), decode_probe->expected_width, decode_probe->expected_height);
      execution->execution()
       .submit(
        {.kind = detail::CompileTaskKind::Pixels,
         .resources = {.bytes = BenchmarkImageDecoder::plan(header).bytes(), .supplied_workspace = true, .opaque_bytes = BenchmarkImageDecoder::plan(header).opaque_bytes},
         .work = [&](std::size_t, const CompileAllowance& allowance) { (void)decoder.decode_rgb(ready.payload->encoded(), header, allowance.workspace()); },
         .parent = ready.payload->allowance(),
         .finite_consumer = true,
         .retain_workspace = false})
       .wait();
     } else {
      BenchmarkImageValidator validator;
      validator.validate_decodable(ready.payload->encoded(), decode_probe->expected_width, decode_probe->expected_height);
     }
    }
   } catch (const BenchmarkImageReadError& error) { throw InvalidImageError(error.what()); } catch (const BenchmarkImageError& error) {
    throw InvalidImageError(error.what());
   }
  } else
   publication(ready);
 };
 const auto completion = image_root / ".complete.json";
 if (decode_probe) {
  if (!std::ranges::binary_search(expected_ids, decode_probe->image_id)) { throw std::runtime_error("benchmark archive decode probe ID is not selected"); }
  transfer_progress->images(source, artifact.artifact_id, 0, source_total_images, *progress);
  progress->source_activity(source, "Invalidating failed " + archive_name + " JPEG under the extraction lock");
  invalidate_cached_image_proofs(image_root);
  remove_cache_path(cached_image_path(image_root, decode_probe->image_id));
 }
 const std::string source_identity = source_name + ":" + shard + ":" + artifact.artifact_id + ":" + std::string(kBenchmarkCatalogRevision);
 progress->source_activity(source, "Checking " + archive_name + " cache completion status");
 const bool quarantine_unavailable = archive_selection_allows_quarantine(source, shard);
 std::uint64_t cached_image_bytes = 0U;
 std::vector<CachedImageRejection> cached_quarantine;
 if (validate_cached_image_group(image_root, completion, source_identity, expected_ids, &cached_image_bytes, cancel_requested, trace, quarantine_unavailable ? &cached_quarantine : nullptr)) {
  std::vector<std::uint64_t> available_ids;
  available_ids.reserve(expected_ids.size() - cached_quarantine.size());
  for (const std::uint64_t image_id : expected_ids) {
   if (!std::ranges::binary_search(cached_quarantine, image_id, {}, &CachedImageRejection::image_id)) { available_ids.push_back(image_id); }
  }
  bool healthy = true;
  BenchmarkCompletionWindow pixels(execution ? std::clamp<std::size_t>(execution->execution().workers() * 2, 2, 64) : 1);
  for (const auto id : available_ids) {
   pixels.make_room();
   const auto generation = execution ? execution->image_generation(image_root, id) : 0;
   publish_image({id});
   pixels.push(execution ? execution->image_completion(image_root, id) : detail::CompileExecution::Completion{}, [&, id, generation](std::exception_ptr error) {
    if (execution && execution->image_generation(image_root, id) != generation) throw std::runtime_error("custom cached publication generation retired");
    if (!error) return;
    try {
     std::rethrow_exception(error);
    } catch (const BenchmarkImageReadError&) {
     healthy = false;
     execution->retire_image(image_root, id);
     invalidate_cached_image_proofs(image_root);
     remove_cache_path(cached_image_path(image_root, id));
    }
   });
  }
  pixels.drain();
  if (healthy) {
   transfer_progress->images(source, artifact.artifact_id, expected_ids.size(), source_total_images, *progress, "Reusing cached " + archive_name + " images");
   return CachedImageDirectory{
    source_name, std::move(shard), image_root, source_identity, cached_image_selection_digest(available_ids), available_ids.size(), cached_image_bytes, true, std::move(cached_quarantine)
   };
  }
 }
 DownloadRequest request = make_download_request(cache, source_name, artifact);
 // Unknown source sizes keep the prior source-specific staging estimate.
 request.storage_estimate = artifact.expected_size ? 0 : kArchiveScratchBytes;
 std::optional<DownloadResult> retained = initial_download ? std::optional(*initial_download) : std::nullopt;
 std::exception_ptr last_error;
 const std::uint32_t extraction_attempts = 3U;
 for (std::uint32_t attempt = 1U; attempt <= extraction_attempts; ++attempt) {
  throw_if_benchmark_cancelled(cancel_requested);
  std::uint64_t attempt_reported = 0U;
  try {
   if (!retained) {
    progress->source_activity(source, "Downloading " + archive_name + " image archive with " + std::to_string(download_connections) + " download connections");
    retained = download_artifacts({request}, download_connections, cancel_requested,
     progress->transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { transfer_progress->update(update, *progress); }} : DownloadProgressSink{}, trace, {}, execution,
     source_handle, storage_reservations)
                .front();
   }
   progress->source_activity(source, "Opening " + archive_name + " image archive");
   BenchmarkImageDecoder image_validator;
   CachedImageDirectory extracted = extract_selected_archive_images(ArchiveExtractionRequest{
    .archive_path = retained->path,
    .source_identity = source_identity,
    .output_root = image_root,
    .source = source_name,
    .shard = shard,
    .selected_image_ids = expected_ids,
    .image_id_parser = benchmark_archive_image_candidate,
    .cancel_requested = cancel_requested,
    .progress =
     [&](const std::uint64_t completed, const std::uint64_t) {
    if (completed < attempt_reported) { throw std::logic_error("archive attempt image progress regressed"); }
    attempt_reported = completed;
    if (progress->transfer_observer_enabled()) { transfer_progress->images(source, artifact.artifact_id, completed, source_total_images, *progress, "Resolving " + archive_name + " selected images"); }
    trace_benchmark_event(trace, "benchmark.images.progress",
     [&] { return nlohmann::json{{"source", source_name}, {"shard", shard}, {"attempt", attempt}, {"resolved_images", completed}, {"total_images", expected_ids.size()}}; });
   },
    .validator =
     [&](const std::uint64_t image_id, const std::span<const std::uint8_t> encoded) {
    if (!has_complete_image_markers(encoded)) {
     if (decode_probe && decode_probe->image_id == image_id && !quarantine_unavailable) {
      throw RequiredImageDecodeError("required archive image " + std::to_string(image_id) + " remains incomplete after bounded repair");
     }
     throw std::runtime_error("selected archive image " + std::to_string(image_id) + " is not a complete JPEG or PNG");
    }
    try {
     if (decode_probe && decode_probe->image_id == image_id) {
      return image_validator.read_header(encoded, decode_probe->expected_width, decode_probe->expected_height);
     } else {
      const auto dimensions = image_validator.read_header(encoded);
      publication.geometry_ready(image_id, {dimensions.width, dimensions.height});
      return dimensions;
     }
    } catch (const BenchmarkImageError& error) {
     if (decode_probe && decode_probe->image_id == image_id && !quarantine_unavailable) {
      throw RequiredImageDecodeError("required archive image " + std::to_string(image_id) + " remains undecodable after bounded repair: " + error.what());
     }
     throw InvalidImageError("selected archive image " + std::to_string(image_id) + ": " + error.what());
    }
   },
    .trace = trace,
    .quarantine_unavailable = quarantine_unavailable,
    .decompression_workers = decompression_workers,
    .activity = [&](const std::string_view activity) { progress->source_activity(source, archive_name + ": " + std::string(activity)); },
    .completion_path = completion,
    .image_ready = publish_image,
    .execution = execution,
    .storage = storage_reservations,
    .parent_allowance = source_handle,
    .retain_encoded = !decode_probe.has_value(),
   });
   progress->source_activity(source, "Retaining completed " + archive_name + " archive in cache");
   extracted.cache_hit = false;
   return extracted;
  } catch (const RequiredImageDecodeError&) { throw; } catch (const std::bad_alloc&) {
   throw;
  } catch (const InsufficientCompileResources&) { throw; } catch (const detail::InsufficientStorage&) {
   throw;
  } catch (...) {
   last_error = std::current_exception();
   std::string failure_reason = "non-standard exception";
   try {
    std::rethrow_exception(last_error);
   } catch (const std::exception& error) { failure_reason = error.what(); } catch (...) {
    failure_reason = "non-standard exception";
   }
   if (attempt_reported != 0U) { transfer_progress->images(source, artifact.artifact_id, 0, source_total_images, *progress, "Withdrawing failed " + archive_name + " attempt"); }
   throw_if_benchmark_cancelled(cancel_requested);
   if (trace) try {
     if (std::filesystem::is_regular_file(request.destination)) {
      const auto digest = common_io::sha256_hex(common_io::sha256_file(request.destination, [&] { return cancel_requested.requested(); }));
      trace_benchmark_event(trace, "benchmark.download.failure_sha256", [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", digest}, {"reason", failure_reason}}; });
     }
    } catch (...) { throw_if_benchmark_cancelled(cancel_requested); }
   if (attempt == 3U) { break; }
   progress->source_activity(source, "Repairing " + archive_name + " extraction for retry " + std::to_string(attempt + 1U));
   invalidate_download_artifact(request, cancel_requested, trace, execution, source_handle);
   retained.reset();
   request.redownload = true;
   // The root is shared by selections. Archive corruption invalidates
   // proof metadata, not otherwise valid JPEGs retained in this root.
   invalidate_cached_image_proofs(image_root);
   trace_benchmark_event(trace, "benchmark.images.archive_retry", [&] { return nlohmann::json{{"source", source_name}, {"shard", shard}, {"attempt", attempt}, {"reason", failure_reason}}; });
  }
 }
 std::rethrow_exception(last_error);
}
class RetiredBenchmarkLabelGeneration : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
struct SourceCompileCount {
 BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
 std::uint64_t selected_images = 0U;
 std::uint64_t compiled_images = 0U;
 std::uint64_t compiled_boxes = 0U;
 std::uint64_t dropped_boxes = 0U;
};
using BenchmarkImageRepair = std::function<bool(const std::filesystem::path&, const BenchmarkImageReadError&)>;
template <std::ranges::random_access_range Directories>
SourceCompileCount append_source_plan(BenchmarkCompilePipeline& execution, const BenchmarkImageRepair& repair, const NormalizedAnnotationReadView& index, const Directories& directories,
 const std::vector<std::uint64_t>* unavailable_image_ids, const bool require_every_image, PreparedBenchmarkSplit* split, BenchmarkSplitWriter& writer,
 mmltk::common::concurrency::CancellationObservation cancel_requested, std::uint64_t original_generation) {
 if (directories.empty()) { throw std::runtime_error("benchmark source has no cached image directories"); }
 if (unavailable_image_ids != nullptr && (!std::ranges::is_sorted(*unavailable_image_ids) || std::ranges::adjacent_find(*unavailable_image_ids) != unavailable_image_ids->end())) {
  throw std::runtime_error("benchmark unavailable image IDs must be sorted and unique");
 }
 const std::size_t source_base = split->sources.size();
 for (const CachedImageDirectory& directory : directories) split->sources.push_back({directory.path});
 std::array<std::size_t, 51> objects_shards{};
 objects_shards.fill(std::numeric_limits<std::size_t>::max());
 if (index.source == BenchmarkDatasetSource::kObjects365V2) {
  for (std::size_t directory_index = 0U; directory_index < directories.size(); ++directory_index) {
   constexpr std::string_view prefix = "patch-";
   if (!std::string_view(directories[directory_index].shard).starts_with(prefix)) { throw std::runtime_error("Objects365 cache has an invalid shard identity"); }
   std::uint32_t shard = 0U;
   const std::string_view number = std::string_view(directories[directory_index].shard).substr(prefix.size());
   const auto parsed = std::from_chars(number.data(), number.data() + number.size(), shard);
   if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || shard >= objects_shards.size() || objects_shards[shard] != std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("Objects365 cache shard map is invalid");
   }
   objects_shards[shard] = directory_index;
  }
 }
 struct ImageLabels {
  std::size_t local_source = 0;
  std::string_view dependency;
  bool available = true;
  std::shared_ptr<const BenchmarkLabelChunk> product;
 };
 std::vector<ImageLabels> chunks(index.image_count());
 if (unavailable_image_ids) {
  std::size_t missing = 0;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
   const auto id = index.image(i).source_image_id;
   while (missing < unavailable_image_ids->size() && (*unavailable_image_ids)[missing] < id) ++missing;
   chunks[i].available = missing == unavailable_image_ids->size() || (*unavailable_image_ids)[missing] != id;
  }
 }
 const auto publish = [&](std::size_t image_index) {
  throw_if_benchmark_cancelled(cancel_requested);
  const auto& image = index.image(image_index);
  if (!chunks[image_index].available) {
   if (require_every_image) throw std::runtime_error("required benchmark image is unavailable");
   return;
  }
  std::size_t local_source = 0;
  if (index.source == BenchmarkDatasetSource::kObjects365V2) {
   local_source = objects_shards[image.source_shard];
   if (local_source == std::numeric_limits<std::size_t>::max()) throw std::runtime_error("Objects365 image references an unacquired shard");
  }
  if (local_source >= directories.size()) throw std::runtime_error("benchmark component references an unacquired directory");
  auto& chunk = chunks[image_index];
  chunk.local_source = local_source;
  chunk.dependency = index.annotation_sha256;
  const auto& root = directories[local_source].path;
  if (!execution.has_image_labels(root, image.source_image_id, chunk.dependency, original_generation) &&
      !execution.labels_ready(
       execution.source_publication(root, {}, image.source_image_id), image.source_image_id, BenchmarkLabelInput(index, image_index), std::string(chunk.dependency), original_generation))
   throw RetiredBenchmarkLabelGeneration("benchmark required label generation retired before admission");
 };
 // Completion order refills the bounded window. Canonical source order is
 // applied only after all selected physical label claims have settled.
 SourceCompileCount counts{index.source, index.image_count(), 0U, 0U};
 const auto collect = [&](std::size_t image_index) {
  auto& chunk = chunks[image_index];
  if (!chunk.available) return;
  const auto& image = index.image(image_index);
  const auto local_source = chunk.local_source;
  std::shared_ptr<const BenchmarkLabelChunk> product_owner;
  for (;;) {
   try {
    publish(image_index);
    product_owner = execution.wait_image_labels(directories[local_source].path, image.source_image_id, chunk.dependency, original_generation);
    break;
   } catch (const BenchmarkImageReadError& error) {
    if (!repair(directories[local_source].path, error)) {
     if (require_every_image) throw;
     chunk.available = false;
     return;
    }
   }
  }
  if (!product_owner) throw RetiredBenchmarkLabelGeneration("benchmark required label generation retired before assembly");
  chunk.product = std::move(product_owner);
 };
 BenchmarkCompletionWindow labels(std::clamp<std::size_t>(execution.execution().workers() * 2, 2, 64));
 for (std::size_t i = 0; i < chunks.size(); ++i) {
  labels.make_room();
  publish(i);
  if (!chunks[i].available) continue;
  const auto& image = index.image(i);
  const auto& root = directories[chunks[i].local_source].path;
  auto completion = (!image.width || !image.height) ? execution.image_completion(root, image.source_image_id) : execution.label_completion(root, image.source_image_id, chunks[i].dependency);
  labels.push(std::move(completion), [&, i](std::exception_ptr) { collect(i); });
 }
 labels.drain();
 for (std::size_t image_index = 0; image_index < chunks.size(); ++image_index) {
  const auto& chunk = chunks[image_index];
  if (!chunk.available) continue;
  const auto& image = index.image(image_index);
  const auto local_source = chunk.local_source;
  const auto& product = *chunk.product;
  const auto physical_slot = execution.image_slot(directories[local_source].path, image.source_image_id);
  if (!physical_slot) throw std::logic_error("benchmark label product has no physical slot");
  const auto stored = writer.entry(*physical_slot);
  counts.dropped_boxes = common_math::checked_add(counts.dropped_boxes, product.dropped, "benchmark dropped box count overflow");
  if (!stored.num_instances && !require_every_image) continue;
  EncodedImageRecord record{
   image.source_image_id, product.width, product.height, stored.num_instances, common_math::checked_cast<std::uint16_t>(source_base + local_source, "benchmark cached source index overflow"),
   (index.source == BenchmarkDatasetSource::kCoco2017       ? AnnotationSource::Coco
    : index.source == BenchmarkDatasetSource::kObjects365V2 ? AnnotationSource::Objects365
                                                            : AnnotationSource::OpenImages)
  };
  split->images.push_back(record);
  split->labels = common_math::checked_add(split->labels, static_cast<std::size_t>(stored.num_instances), "benchmark label count overflow");
  split->runs = common_math::checked_add(split->runs, static_cast<std::size_t>(stored.num_runs), "benchmark run count overflow");
  counts.compiled_images = common_math::checked_add(counts.compiled_images, 1U, "benchmark compiled image count overflow");
  counts.compiled_boxes = common_math::checked_add(counts.compiled_boxes, stored.num_instances, "benchmark compiled box count overflow");
 }
 return counts;
}
[[nodiscard]] PreparedBenchmarkSplit make_split(const std::string& name) {
 PreparedBenchmarkSplit split;
 split.name = name;
 split.class_names.reserve(coco80_class_names().size());
 for (const std::string_view class_name : coco80_class_names()) { split.class_names.emplace_back(class_name); }
 return split;
}
[[nodiscard]] std::uint64_t estimate_output_bytes(const std::uint64_t images, const std::uint64_t boxes, const std::uint32_t resolution, const std::uint64_t mask_rle_pairs = 0U) {
 const auto stride = common_math::checked_multiply<std::size_t>(
  common_math::checked_multiply<std::size_t>(resolution, resolution, "benchmark output resolution overflow"), 3U * sizeof(float), "benchmark image stride overflow");
 const auto layout = compute_file_layout(common_math::checked_cast<std::uint32_t>(images, "benchmark image count overflow"), stride);
 const auto pixels = common_math::checked_multiply(images, static_cast<std::uint64_t>(layout.pixel_pitch), "benchmark occupied pixels overflow");
 const auto labels = common_math::checked_add(common_math::checked_multiply(boxes, sizeof(PackedInstance), "benchmark label estimate overflow"),
  common_math::checked_multiply(mask_rle_pairs, sizeof(RLEPair), "benchmark run estimate overflow"), "benchmark annotation estimate overflow");
 const auto padding = common_math::checked_multiply(images, std::uint64_t{PAGE_SIZE + sizeof(RLEPair)}, "benchmark label page estimate overflow");
 return common_math::checked_add(common_math::checked_add(pixels, labels, "benchmark occupied estimate overflow"),
  common_math::checked_add(padding, static_cast<std::uint64_t>(layout.index_size + sizeof(FileHeader) + PAGE_SIZE), "benchmark metadata estimate overflow"), "benchmark output estimate overflow");
}
void sync_directory(const std::filesystem::path& path) {
 const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
 if (descriptor < 0) { throw common_io::errno_error("cannot open benchmark directory for sync", path.string()); }
 common_io::ScopedFd directory(descriptor);
 if (::fsync(directory.get()) != 0) { throw common_io::errno_error("cannot sync benchmark directory", path.string()); }
}
void publish_dataset_directory(const std::filesystem::path& staging, const std::filesystem::path& output, const bool overwrite, const mmltk::common::concurrency::CancellationObservation cancellation,
 const BenchmarkTraceSink& trace) {
 throw_if_benchmark_cancelled(cancellation);
 const bool output_exists = std::filesystem::exists(output);
 trace_benchmark_event(trace, "benchmark.publication.started", [&] { return nlohmann::json{{"output", output.string()}, {"replacing", output_exists}}; });
 if (output_exists && !overwrite) {
  if (!std::filesystem::is_empty(output)) { throw std::runtime_error("benchmark output directory is no longer empty"); }
  // rmdir refuses a directory populated since the check. No-replace publication
  // then refuses any new occupant appearing after the empty directory retires.
  if (::rmdir(output.c_str()) != 0) throw common_io::errno_error("cannot remove empty benchmark output directory", output.string());
 }
 throw_if_benchmark_cancelled(cancellation);
 const auto publication = common_io::publish_staged_path_atomically(staging, output, overwrite);
 if (publication.replaced) {
  trace_benchmark_event(trace, "benchmark.publication.old_generation_cleanup",
   [&] { return nlohmann::json{{"path", staging.string()}, {"removed", !publication.cleanup_error}, {"error", publication.cleanup_error ? publication.cleanup_error.message() : std::string{}}}; });
 }
}
void write_split_with_progress(BenchmarkSplitWriter& writer, const BenchmarkWriteRequest& request, ProgressReporter* progress, const BenchmarkTraceSink& trace) {
 progress->phase(DatasetCompilePhase::Pixels);
 progress->activity("Decoding and resizing " + request.split.name + " JPEG pixels");
 const Clock::time_point started = trace ? Clock::now() : Clock::time_point{};
 BenchmarkWriteRequest observed = request;
 if (progress->pixel_observer_enabled()) {
  observed.progress = {.context = progress, .image_completed = [](void* context) {
   static_cast<ProgressReporter*>(context)->pixel_completed();
  }, .images_invalidated = [](void* context, std::uint64_t count) { static_cast<ProgressReporter*>(context)->invalidate_pixels(count); }};
 }
 writer.write_remaining(observed);
 trace_benchmark_event(trace, "benchmark.pixel_compile.complete", [&] {
  const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return nlohmann::json{
   {"split", request.split.name}, {"completed_images", request.split.images.size()}, {"elapsed_seconds", elapsed_seconds},
   {"images_per_second", elapsed_seconds > 0.0 ? static_cast<double>(request.split.images.size()) / elapsed_seconds : 0.0}, {"eta_seconds", 0.0}
  };
 });
}
[[nodiscard]] nlohmann::json mapping_manifest() {
 nlohmann::json mappings;
 mappings["coco"] = nlohmann::json::array();
 for (const NumericCategoryMapping& mapping : coco_category_mappings()) {
  mappings["coco"].push_back({{"source_id", mapping.source_id}, {"source_name", mapping.expected_name}, {"target_id", mapping.target_id}, {"target_name", coco80_class_names()[mapping.target_id]}});
 }
 mappings["objects365"] = nlohmann::json::array();
 for (const NumericCategoryMapping& mapping : objects365_category_mappings()) {
  mappings["objects365"].push_back(
   {{"source_id", mapping.source_id}, {"source_name", mapping.expected_name}, {"target_id", mapping.target_id}, {"target_name", coco80_class_names()[mapping.target_id]}});
 }
 mappings["open_images"] = nlohmann::json::array();
 for (const StringCategoryMapping& mapping : open_images_category_mappings()) {
  mappings["open_images"].push_back(
   {{"source_id", mapping.source_id}, {"source_name", mapping.expected_name}, {"target_id", mapping.target_id}, {"target_name", coco80_class_names()[mapping.target_id]}});
 }
 return mappings;
}
[[nodiscard]] nlohmann::json source_catalog_manifest(const CustomRecipeCatalog& catalog) {
 nlohmann::json artifacts = nlohmann::json::array();
 const auto append = [&](const CatalogArtifact& artifact) {
  nlohmann::json record{{"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename}, {"expected_size", artifact.expected_size}};
  if (!artifact.expected_sha256.empty()) { record["expected_sha256"] = artifact.expected_sha256; }
  artifacts.push_back(std::move(record));
 };
 append(catalog.coco_annotations);
 append(catalog.coco_train_images);
 append(catalog.coco_val_images);
 append(catalog.objects_annotations);
 for (const CatalogArtifact& artifact : catalog.objects_images) { append(artifact); }
 append(catalog.open_images_boxes);
 append(catalog.open_images_classes);
 return nlohmann::json{{"artifacts", std::move(artifacts)}, {"open_images_train_image_url_template", open_images_train_image_url_template()}};
}
}  // namespace
bool archive_selection_allows_quarantine(const BenchmarkDatasetSource source, const std::string_view split) noexcept { return source != BenchmarkDatasetSource::kCoco2017 || split == "train2017"; }
void publish_benchmark_manifest(const BenchmarkCompilerConfig& config, const std::filesystem::path& staging_dir, const std::filesystem::path& cache_root, nlohmann::json facts,
 const common_concurrency::CancellationObservation cancelled, detail::StorageReservationPool* storage) {
 facts["schema_version"] = 3U;
 facts["compiled_format_version"] = FORMAT_VERSION;
 facts["normalized_annotation_version"] = kNormalizedAnnotationIndexVersion;
 facts["resize_mode"] = config.resize_mode == mmltk::backend::imaging::resample::ImageResizeMode::Stretch ? "stretch" : "letterbox";
 facts["resampling"] = {{"perceptual_downscale", config.perceptual_downscale}, {"version", 1U}};
 facts["catalog_revision"] = kBenchmarkCatalogRevision;
 facts["mapping_revision"] = kBenchmarkMappingRevision;
 facts["resolution"] = config.resolution;
 facts["cache_root"] = cache_root.string();
 facts["mappings"] = mapping_manifest();
 if (config.selection.dataset == BenchmarkDatasetVariant::Coconut) {
  facts["source_catalog"] = {{"artifacts", facts.at("recipe").at("artifacts")}};
  facts["mapping_revision"] = kCoconutNormalizationRevision;
  facts["mappings"].erase("objects365");
  facts["mappings"].erase("open_images");
 } else {
  if (!facts.contains("source_catalog")) facts["source_catalog"] = source_catalog_manifest(custom_recipe_catalog());
  facts["recipe"] = {{"dataset", "coco-custom"}};
 }
 write_json_atomically(staging_dir / "benchmark_manifest.json", facts, cancelled, storage);
}
}  // namespace benchmark_internal
namespace benchmark_internal {
namespace {
void compile_coconut_sources(const BenchmarkCompilerConfig& config, const BenchmarkCacheLayout& cache, const CoconutRecipeCatalog& catalog, bool explicit_catalog, BenchmarkCompilePipeline& pipeline,
 ProgressReporter& progress, CoconutFailureReport& failures, common_concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, const BenchmarkImageReadObserver& image_opened,
 CocoAnnotationInputObserver stock_input, const std::function<void(std::exception_ptr)>& fail, const std::function<void(BenchmarkDatasetSource, std::string_view)>& labels_started,
 const std::function<void()>& selection_started) {
 common_io::StagingDirectory staging(config.output_dir, ".", ".benchmark.tmp.XXXXXX", "cannot create benchmark dataset staging directory");
 auto train = make_split("train"), validation = make_split("val");
 const BenchmarkWriteProgressEvent pixel_progress = progress.pixel_observer_enabled()
                                                     ? BenchmarkWriteProgressEvent{
                                                        .context = &progress,
                                                        .image_completed = [](void* owner) { static_cast<ProgressReporter*>(owner)->pixel_completed(); },
                                                        .images_invalidated = [](void* owner, std::uint64_t count) { static_cast<ProgressReporter*>(owner)->invalidate_pixels(count); }
                                                       }
                                                     : BenchmarkWriteProgressEvent{};
 const auto request = [&](const PreparedBenchmarkSplit& split) {
  auto result = BenchmarkWriteRequest{
   split, staging.path() / (split.name + ".bin"), config.resolution, static_cast<int>(pipeline.execution().workers()), pipeline.execution().cpus(), false, cancellation, pixel_progress,
   config.perceptual_downscale, config.resize_mode, image_opened, {}, &pipeline
  };
  result.trace = trace;
  if (progress.pixel_observer_enabled()) result.compaction = [&, val = split.name == "val"](std::size_t done, std::size_t total) { progress.compacting(val ? 1U : 0U, done, total); };
  return result;
 };
 BenchmarkSplitWriter train_writer(request(train), true), validation_writer(request(validation), true);
 std::vector<AdmittedRecipeArchive> admitted;
 admitted.reserve(catalog.images.size());
 for (const auto& archive : catalog.images) admitted.push_back({archive, {}});
 std::mutex source_work_mutex;
 std::condition_variable source_work_changed;
 std::deque<std::pair<AdmittedRecipeArchive*, std::string>> source_work;
 for (auto& archive : admitted) source_work.emplace_back(&archive, std::string{});
 detail::StorageReservationPool storage(cache.root, storage_observer(trace), &pipeline.storage());
 CoconutPhysicalMembership membership(
  admitted, catalog, explicit_catalog, cache, storage, pipeline, cancellation, [&](CoconutEdition, const AdmittedRecipeArchive& archive, std::uint64_t id, std::uint64_t entries) {
  progress.transfers().images(archive.origin.artifact.source, archive.origin.artifact.artifact_id, entries, 0, progress);
  trace_benchmark_event(trace, "benchmark.archive.image_ready", [&] { return nlohmann::json{{"artifact", archive.origin.artifact.artifact_id}, {"image_id", id}, {"entries", entries}}; });
 }, [&](AdmittedRecipeArchive& archive, std::string reason) {
  {
   const std::lock_guard lock(source_work_mutex);
   source_work.emplace_back(&archive, std::move(reason));
  }
  source_work_changed.notify_one();
 });
 BenchmarkCompilePipeline::Attempt retire(pipeline);
 for (const auto source : {CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoValidation, CoconutImageNamespace::Objects365V1, CoconutImageNamespace::Objects365V2}) {
  const bool val = source == CoconutImageNamespace::CocoValidation || source == CoconutImageNamespace::Objects365V1;
  pipeline.bind_source(membership.identity_root(source), val ? validation_writer : train_writer,
   source == CoconutImageNamespace::CocoValidation && config.selection.validation == CoconutValidation::Stock ? AnnotationSource::Coco : coconut_annotation_source(source));
 }
 if (trace)
  pipeline.label_configuration(config.resolution, config.resize_mode, {}, [&](const std::filesystem::path& root, std::uint64_t id) {
   const bool is_validation = root == membership.identity_root(CoconutImageNamespace::CocoValidation) || root == membership.identity_root(CoconutImageNamespace::Objects365V1);
   trace_benchmark_event(trace, "benchmark.labels.placed", [&] { return nlohmann::json{{"split", is_validation ? "val" : "train"}, {"image_id", id}}; });
  });
 pipeline.membership_ready();
 if (progress.pixel_observer_enabled()) progress.pixels(0, 0, false);
 std::mutex rejection_mutex;
 nlohmann::json omitted = nlohmann::json::array();
 const auto reject = [&](const CoconutPhysicalImage& image, std::string_view reason) {
  trace_benchmark_event(trace, "benchmark.coconut.image_rejected",
   [&] { return nlohmann::json{{"source", coconut_namespace_name(image.source)}, {"image_id", image.image_id}, {"member", image.member}, {"reason", reason}}; });
  failures.reject(image, image.image_id, "physical", 0, 0, reason);
  const std::lock_guard lock(rejection_mutex);
  omitted.push_back({{"source", coconut_namespace_name(image.source)}, {"image_id", image.image_id}, {"reason", reason}});
 };
 std::vector<std::jthread> sources;
 std::exception_ptr source_error;
 std::mutex source_error_mutex;
 // I/O controllers are independent of the CPU lane count. Even one assigned
 // CPU can consume a ready archive while another connection is held.
 const auto source_count = std::min<std::size_t>(kArchivePipelineConcurrency, admitted.size());
 auto annotations = acquire_coconut_recipe_inputs(
  cache, catalog, progress, catalog.releases.size(), cancellation, trace, pipeline.curl().limit(BenchmarkCurl::Class::Artifact), pipeline.execution().cpus(), fail, &pipeline);
 for (std::size_t lane = 0; lane < source_count; ++lane)
  sources.emplace_back([&](std::stop_token stop) {
   const std::stop_callback wake(stop, [&] { source_work_changed.notify_all(); });
   try {
    for (;;) {
     std::pair<AdmittedRecipeArchive*, std::string> work;
     {
      std::unique_lock lock(source_work_mutex);
      source_work_changed.wait(lock, [&] { return stop.stop_requested() || !source_work.empty(); });
      if (stop.stop_requested()) return;
      work = std::move(source_work.front());
      source_work.pop_front();
     }
     auto& archive = *work.first;
     bool replacement = false;
     const auto invalidate = [&](std::string_view reason) {
      const auto download_request = make_download_request(cache, benchmark_source_name(archive.origin.artifact.source), archive.origin.artifact);
      bool matches_expected = false;
      const auto diagnose = [&] {
       auto lease = ArtifactLease::acquire_charged(download_request.lock_path, cancellation, &pipeline.execution(), CompileResources::handles(1, false, 1));
       auto digest_handle = pipeline.execution().reserve(CompileResources::handles(1), lease->allowance());
       const auto& digest = download_failure_sha256(archive.download, cancellation);
       matches_expected = !archive.origin.artifact.expected_sha256.empty() && digest == archive.origin.artifact.expected_sha256;
       trace_benchmark_event(trace, "benchmark.download.failure_sha256",
        [&] { return nlohmann::json{{"artifact", archive.origin.artifact.artifact_id}, {"sha256", digest}, {"matches_expected", matches_expected}, {"reason", reason}}; });
      };
      if (!archive.origin.artifact.expected_sha256.empty() && std::filesystem::is_regular_file(archive.download.path))
       diagnose();
      else if (trace)
       try {
        diagnose();
       } catch (...) { throw_if_benchmark_cancelled(cancellation); }
      if (matches_expected) {
       progress.warning("Rejected image in verified COCONut source " + archive.origin.artifact.artifact_id + ": " + std::string(reason));
       membership.unavailable(archive);
       return false;
      }
      invalidate_download_artifact(download_request, cancellation, trace, &pipeline);
      replacement = true;
      return true;
     };
     if (work.second.empty()) {
      if (membership.consume(archive, reject, true)) continue;
     } else if (!invalidate(work.second))
      continue;
     while (membership.can_retry(archive)) {
      try {
       auto download = acquire_physical_archive(archive, cache, progress, progress.transfers(), storage, pipeline.curl().limit(BenchmarkCurl::Class::Artifact), cancellation, trace, &pipeline);
       membership.admitted(archive, std::move(download), replacement);
       membership.consume(archive, reject);
       break;
      } catch (const BenchmarkDownloadUnavailable& error) {
       progress.warning("Unavailable COCONut image source " + archive.origin.artifact.artifact_id + ": " + error.what());
       membership.unavailable(archive);
       break;
      } catch (const BenchmarkArchiveError& error) {
       if (!membership.can_retry(archive)) {
        progress.warning("Rejected COCONut image source " + archive.origin.artifact.artifact_id + ": " + error.what());
        membership.unavailable(archive);
        break;
       }
       // A repair that is unavailable must retain readable rows from this input.
       // Generation withdrawal occurs only after replacement bytes are admitted.
       if (!invalidate(error.what())) break;
      }
     }
    }
   } catch (...) {
    {
     const std::lock_guard lock(source_error_mutex);
     if (!source_error) source_error = std::current_exception();
    }
    fail(std::current_exception());
   }
  });
 std::optional<CoconutRecipePreparation> recipe;
 try {
  recipe =
   prepare_coconut_recipe(config, cache, catalog, admitted, membership, progress, failures, pipeline.execution().workers(), cancellation, trace, annotations, &pipeline, {}, std::move(stock_input));
  if (recipe->stock_validation)
   for (const auto& image : recipe->stock_validation->images) {
    CoconutRecord record;
    record.image_id = image.source_image_id;
    record.file_name = std::format("val2017/{:012}.jpg", image.source_image_id);
    record.namespace_hint = CoconutImageNamespace::CocoValidation;
    try {
     (void)membership.resolve(CoconutEdition::RelabeledValidation, record);
    } catch (const CoconutPhysicalMembershipError&) { /* Final selection reports the unavailable image. */
    }
   }
 } catch (...) {
  fail(std::current_exception());
  sources.clear();
  annotations.reset();
  throw;
 }
 sources.clear();
 if (source_error) std::rethrow_exception(source_error);
 // Every producer has joined. Settle sibling claims before final selection so
 // an active conversion cannot later replace a replay or borrow its ordinal base.
 pipeline.drain();
 if (selection_started) selection_started();
 std::array<std::vector<std::uint32_t>, 2> selected;
 std::array<std::uint64_t, 2> boxes{}, runs{};
 std::map<BenchmarkDatasetSource, std::uint64_t> eligible_by_source;
 BenchmarkCompletionWindow restored(std::clamp<std::size_t>(pipeline.execution().workers() * 2, 2, 64));
 const auto select = [&](const NormalizedAnnotationReadView& index, std::size_t position, const CoconutPhysicalImage& physical, const CoconutComponent* component, std::uint64_t original_generation,
                      bool val) {
  const auto& image = index.image(position);
  const auto physical_member = membership.find(physical.source, physical.image_id);
  if (!physical_member || *physical_member != physical) {
   reject(physical, "ambiguous or unverified physical identity");
   return;
  }
  const auto root = membership.identity_root(physical.source);
  auto& writer = val ? validation_writer : train_writer;
  const auto slot = pipeline.image_slot(root, physical.image_id);
  if (!slot || !writer.image_complete(*slot)) {
   reject(physical, "unreadable required pixels after bounded repair");
   return;
  }
  const auto actual = writer.dimensions(*slot);
  if (actual != std::pair{image.width, image.height}) {
   reject(physical, "annotation canvas contradicts the image header");
   return;
  }
  if (component && !component->required_labels_valid(position)) {
   reject(physical, "all required foreground labels were rejected");
   return;
  }
  restored.make_room();
  const auto dependency = component ? component->image_input_identity(position) : index.annotation_sha256;
  const auto retained = pipeline.has_image_labels(root, physical.image_id, dependency, original_generation);
  if (!retained && !pipeline.labels_ready(pipeline.source_publication(root, {}, physical.image_id), physical.image_id, component ? component->labels(position) : BenchmarkLabelInput(index, position),
                    dependency, original_generation, component && component->edition() == CoconutEdition::Large ? 2U : 1U))
   throw std::runtime_error("selected COCONut label generation retired before admission");
  const auto final_position = selected[val].size();
  selected[val].push_back(std::numeric_limits<std::uint32_t>::max());
  const auto source = physical.source == CoconutImageNamespace::Objects365V1 ? BenchmarkDatasetSource::kObjects365V1 : index.source;
  restored.push(pipeline.label_completion(root, physical.image_id, dependency),
   [&, root, physical, dependency, original_generation, retained, component, position, val, final_position, source, slot](std::exception_ptr error) {
   if (error) std::rethrow_exception(error);
   if (!pipeline.has_image_labels(root, physical.image_id, dependency, original_generation)) throw std::logic_error("selected COCONut label claim does not occupy its physical slot");
   auto& target = val ? validation_writer : train_writer;
   if (retained && component) target.settle_labels(*slot, component->compiled_ordinal_base(position), !component->source_ordinals_complete(position));
   const auto entry = target.entry(*slot);
   if (!entry.eligible()) {
    reject(physical, "required label product is unavailable");
    return;
   }
   selected[val][final_position] = *slot;
   ++eligible_by_source[source];
   boxes[val] += entry.num_instances;
   runs[val] += entry.num_runs;
  });
 };
 for (const auto& component : recipe->components) {
  if (labels_started) labels_started(component.index().source, component.index().split);
  const bool val = coconut_validation_component(component.edition());
  for (std::size_t position = 0; position < component.index().image_count(); ++position)
   select(component.index(), position, component.inventory_image(position).physical, &component, component.original_generation(), val);
 }
 if (recipe->stock_validation) {
  const NormalizedAnnotationReadView index(*recipe->stock_validation);
  for (std::size_t position = 0; position < index.image_count(); ++position) {
   const auto id = index.image(position).source_image_id;
   const auto* physical = membership.find(CoconutImageNamespace::CocoValidation, id);
   if (physical)
    select(index, position, *physical, nullptr, recipe->stock_validation_generation, true);
   else
    reject({CoconutImageNamespace::CocoValidation, id, 0, {}, {}}, "stock validation image is unavailable");
  }
 }
 pipeline.drain();
 restored.drain();
 for (auto& slots : selected) std::erase(slots, std::numeric_limits<std::uint32_t>::max());
 progress.source_images(BenchmarkDatasetSource::kCoconut, selected[0].size() + selected[1].size(), selected[0].size() + selected[1].size());
 progress.source_complete(BenchmarkDatasetSource::kCoconut, recipe->annotation_cache_hit);
 for (const auto source : {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kObjects365V1}) {
  std::uint64_t count = 0;
  bool cached = true, present = false;
  for (const auto& archive : admitted)
   if (archive.origin.artifact.source == source) {
    present = true;
    cached &= archive.download.cache_hit;
   }
  count = eligible_by_source[source];
  if (present) {
   progress.source_images(source, count, count);
   progress.source_complete(source, cached);
  }
 }
 progress.acquisition_complete();
 if (selected[0].empty() || selected[1].empty()) throw std::runtime_error("required COCONut split contains no healthy samples");
 progress.pixels(selected[0].size() + selected[1].size(), selected[0].size() + selected[1].size());
 std::uint64_t rejected_objects = 0;
 for (const auto& component : recipe->components) {
  const auto& rejected = component.index().rejected;
  for (const auto count : {rejected.malformed_records, rejected.degenerate_boxes, rejected.unmapped_categories})
   rejected_objects = common_math::checked_add(rejected_objects, count, "COCONut rejection count overflow");
 }
 progress.rejected(rejected_objects, omitted.size());
 auto sealed_splits = seal_benchmark_splits(pipeline, [&](std::size_t i, std::span<std::byte> scratch) {
  auto seal_request = request(i ? validation : train);
  seal_request.compaction_scratch = scratch;
  return (i ? validation_writer : train_writer).seal_slots(seal_request, selected[i]);
 });
 progress.phase(DatasetCompilePhase::Syncing, 0, 3);
 for (auto& sealed : sealed_splits) sealed.artifact.publish(sealed.info.path, cancellation, detail::StagedArtifact::Publication::DurableReplace, false);
 progress.phase(DatasetCompilePhase::Syncing, 1, 3);
 nlohmann::json manifest{{"recipe", recipe->manifest}, {"sources", recipe->manifest["components"]}, {"image_cache", nlohmann::json::array()}, {"quarantined_images", std::move(omitted)}};
 std::uint64_t cached_image_bytes = 0;
 for (const auto& archive : admitted) {
  const auto facts = membership.summary(archive);
  cached_image_bytes = common_math::checked_add(cached_image_bytes, facts.image_bytes, "COCONut cached image size overflow");
  manifest["image_cache"].push_back(
   {{"source", facts.source}, {"shard", facts.shard}, {"images", facts.image_count}, {"bytes", facts.image_bytes}, {"cache_hit", facts.cache_hit}, {"identity", facts.identity}});
 }
 manifest["cached_image_bytes"] = cached_image_bytes;
 for (std::size_t i = 0; i < 2; ++i) {
  const auto name = i ? "val" : "train";
  manifest[name] = {{"images", selected[i].size()}, {"boxes", boxes[i]}, {"mask_rle_pairs", runs[i]}, {"bytes", std::filesystem::file_size(staging.path() / (std::string(name) + ".bin"))}};
 }
 manifest["compiled_bytes"] = manifest["train"]["bytes"].get<std::uint64_t>() + manifest["val"]["bytes"].get<std::uint64_t>();
 manifest["projected_output_bytes"] = manifest["compiled_bytes"];
 publish_benchmark_manifest(config, staging.path(), cache.root, std::move(manifest), cancellation, &pipeline.storage());
 progress.phase(DatasetCompilePhase::Syncing, 2, 3);
 trace_benchmark_event(trace, "benchmark.directory_sync.started", [&] { return nlohmann::json{{"path", staging.path().string()}}; });
 sync_directory(staging.path());
 trace_benchmark_event(trace, "benchmark.directory_sync.completed", [&] { return nlohmann::json{{"path", staging.path().string()}}; });
 progress.phase(DatasetCompilePhase::Syncing, 3, 3);
 progress.phase(DatasetCompilePhase::Publishing, 0, 1);
 progress.flush();
 failures.flush();
 publish_dataset_directory(staging.path(), config.output_dir, config.overwrite, cancellation, trace);
 staging.published();
 trace_benchmark_event(trace, "benchmark.publication.complete", [&] { return nlohmann::json{{"output", config.output_dir.string()}}; });
 progress.phase(DatasetCompilePhase::Publishing, 1, 1);
 progress.flush();
}
}  // namespace
}  // namespace benchmark_internal
void benchmark_internal::compile_benchmark_recipe(BenchmarkCompilerConfig config, const CoconutRecipeCatalog* explicit_catalog, const CustomRecipeCatalog* explicit_custom_catalog,
 const std::function<void(BenchmarkDatasetSource, std::string_view)>& source_labels_started, const BenchmarkImageReadObserver& image_opened, CompileExecutionLimits limits,
 CocoAnnotationInputObserver stock_input, const std::function<void()>& selection_started, const std::function<void(BenchmarkDatasetSource, std::string_view)>& source_images_acquired) {
 if (!valid_benchmark_selection(config.selection)) throw std::invalid_argument("invalid benchmark dataset selection");
 using namespace benchmark_internal;
 if (config.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Stretch && config.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Letterbox)
  throw std::invalid_argument("invalid benchmark resize mode");
 if (config.resolution == 0U || config.resolution > MAX_IMAGE_EXTENT) { throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format"); }
 if (config.output_dir.empty()) { throw std::runtime_error("benchmark output directory must not be empty"); }
 BenchmarkCompileCancellation cancellation_state(config.cancel_requested);
 const auto cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(cancellation_state);
 try {
  struct StopFailedAttempt {
   BenchmarkCompileCancellation& cancellation;
   int exceptions = std::uncaught_exceptions();
   ~StopFailedAttempt() {
    if (std::uncaught_exceptions() > exceptions) cancellation.cancel();
   }
  };
  const BenchmarkTraceSink trace = make_trace_sink(config.trace);
  const bool coconut = config.selection.dataset == BenchmarkDatasetVariant::Coconut;
  std::vector<BenchmarkDatasetSource> participating;
  if (coconut) {
   participating = {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kCoconut};
   if (config.selection.validation == CoconutValidation::Coconut) participating.push_back(BenchmarkDatasetSource::kObjects365V1);
  }
  ProgressReporter progress(std::move(config.progress), trace, participating, [&](std::exception_ptr error) { cancellation_state.fail(std::move(error)); });
  progress.phase(DatasetCompilePhase::Planning);
  progress.activity("Resolving benchmark output path");
  config.output_dir = std::filesystem::absolute(config.output_dir).lexically_normal();
  if (config.output_dir == config.output_dir.root_path()) { throw std::runtime_error("benchmark output directory must not be the filesystem root"); }
  const std::filesystem::path output_parent = config.output_dir.parent_path().empty() ? std::filesystem::path{"."} : config.output_dir.parent_path();
  if (config.cache_dir.empty()) {
   if (const char* root = std::getenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT"); root != nullptr && root[0] != '\0') { config.cache_dir = root; }
  }
  if (config.cache_dir.empty()) { config.cache_dir = "./.cache/benchmark-dataset/v1"; }
  const std::filesystem::path normalized_cache_root = std::filesystem::weakly_canonical(std::filesystem::absolute(config.cache_dir));
  const std::filesystem::path normalized_output = std::filesystem::weakly_canonical(config.output_dir);
  const std::filesystem::path normalized_publication =
   config.publication_dir.empty() ? normalized_output : std::filesystem::weakly_canonical(std::filesystem::absolute(config.publication_dir).lexically_normal());
  trace_benchmark_event(trace, "benchmark.compile.paths", [&] {
   const TracePath cache_path = project_trace_path(normalized_cache_root.native());
   const TracePath output_path = project_trace_path(normalized_output.native());
   return nlohmann::json{
    {"cache_root", cache_path.text}, {"output_root", output_path.text}, {"cache_root_truncated", cache_path.truncated}, {"output_root_truncated", output_path.truncated},
    {"cache_root_utf8_replaced", cache_path.utf8_replaced}, {"output_root_utf8_replaced", output_path.utf8_replaced}
   };
  });
  if (path_contains(normalized_cache_root, normalized_output) || path_contains(normalized_output, normalized_cache_root) || path_contains(normalized_cache_root, normalized_publication) ||
      path_contains(normalized_publication, normalized_cache_root)) {
   throw std::runtime_error("benchmark output and cache directories must not overlap");
  }
  progress.activity("Preparing benchmark output directory");
  std::filesystem::create_directories(output_parent);
  progress.activity("Preparing benchmark cache");
  const BenchmarkCacheLayout cache = BenchmarkCacheLayout::create(normalized_cache_root);
  CoconutFailureReport coconut_failures(cache.root, progress);
  progress.activity("Waiting for benchmark output lock");
  ArtifactLease output_lease =
   ArtifactLease::acquire(normalized_output.parent_path() / ".cache" / "benchmark-dataset" / "v1" / "locks" / ("output-" + output_lock_identity(normalized_output) + ".lock"), cancel_requested);
  progress.activity("Validating benchmark output destination");
  if (std::filesystem::exists(config.output_dir)) {
   const std::filesystem::file_status status = std::filesystem::symlink_status(config.output_dir);
   if (!std::filesystem::is_directory(status)) { throw std::runtime_error("benchmark output path exists and is not a directory"); }
   if (!config.overwrite && !std::filesystem::is_empty(config.output_dir)) { throw std::runtime_error("benchmark output directory is not empty; use --overwrite"); }
  }
  config.num_workers = config.num_workers > 0 ? config.num_workers : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const std::size_t effective_num_workers =
   std::min<std::size_t>(common_math::checked_cast<std::size_t>(config.num_workers, "benchmark effective worker count overflow"), common_system::allowed_cpu_set().size());
  config.num_workers = common_math::checked_cast<int>(effective_num_workers, "benchmark effective worker count overflow");
  auto compile_cpus = common_system::allowed_cpu_set();
  limits.workspace_plan = std::make_shared<detail::CompileWorkspacePlan>(detail::compile_workspace_plan(
   effective_num_workers, config.resolution, config.perceptual_downscale, coconut ? kParquetPoolBytes : 0, limits.transient_bytes, BenchmarkEncodedImage::normal_window_bytes));
  BenchmarkCompilePipeline pipeline(effective_num_workers, compile_cpus, limits, cancel_requested, trace);
  const auto cancellation_binding = cancellation_state.bind_execution(pipeline.execution());
  compile_cpus.assign(pipeline.execution().cpus().begin(), pipeline.execution().cpus().end());
  std::function<void(const std::filesystem::path&, std::uint64_t, const BenchmarkLabelWorkspace&)> observe_label_projection;
  std::function<void(const std::filesystem::path&, std::uint64_t)> observe_label_write;
  if (trace) {
   observe_label_projection = [&](const auto& root, auto id, const BenchmarkLabelWorkspace& workspace) {
    trace_benchmark_event(
     trace, "benchmark.labels.projection_started", [&] { return nlohmann::json{{"root", root.string()}, {"image_id", id}, {"retained_workspace_bytes", workspace.retained_bytes()}}; });
   };
   observe_label_write = [&](const auto& root, auto id) { trace_benchmark_event(trace, "benchmark.labels.slot_written", [&] { return nlohmann::json{{"root", root.string()}, {"image_id", id}}; }); };
  }
  pipeline.label_configuration(config.resolution, config.resize_mode, std::move(observe_label_projection), std::move(observe_label_write));
  // Inventory, annotation prerequisites, prefetched archives and later repair
  // may retain source leases before their first transfer. Protect their shared
  // fixed transport once, before any of those dependent producers are admitted.
  auto source_transport = pipeline.curl().channel(BenchmarkCurl::Class::Artifact, cancel_requested, archive_image_resources());
  const auto fail_compile = [&](std::exception_ptr error) { cancellation_state.fail(std::move(error)); };
  const auto custom_catalog = explicit_custom_catalog ? *explicit_custom_catalog : coconut ? CustomRecipeCatalog{} : custom_recipe_catalog();
  const auto coconut_catalog = coconut ? (explicit_catalog ? *explicit_catalog : coconut_recipe_catalog(config.selection.validation)) : CoconutRecipeCatalog{};
  if (coconut) {
   compile_coconut_sources(config, cache, coconut_catalog, explicit_catalog != nullptr, pipeline, progress, coconut_failures, cancel_requested, trace, image_opened, std::move(stock_input),
    fail_compile, source_labels_started, selection_started);
   return;
  }
  auto& physical_progress = progress.transfers();
  CompileAllowance preparation_allowance;
  constexpr std::size_t input_budget = 2;
  std::unique_ptr<common_io::StagingDirectory> staging_owner;
  std::unique_ptr<BenchmarkSplitWriter> retained_train_pixels, retained_validation_pixels;
  CustomRecipePreparation custom{};
  struct PrefetchedArchive {
   std::optional<DownloadResult> download{};
   std::shared_ptr<ArtifactLease> lease{};
   std::optional<CachedImageDirectory> images{};
  };
  std::mutex archive_readiness_mutex;
  std::array<std::future<PrefetchedArchive>, 2> custom_transfers;
  StopFailedAttempt stop_failed_attempt{cancellation_state};
  if (!staging_owner) staging_owner = std::make_unique<common_io::StagingDirectory>(config.output_dir, ".", ".benchmark.tmp.XXXXXX", "cannot create benchmark dataset staging directory");
  const std::filesystem::path& staging_dir = staging_owner->path();
  const auto pixel_lanes = pipeline.execution().workers();
  if (progress.pixel_observer_enabled()) progress.pixels(0, 0, false);
  const BenchmarkWriteProgressEvent pixel_progress = progress.pixel_observer_enabled()
                                                      ? BenchmarkWriteProgressEvent{
                                                         .context = &progress,
                                                         .image_completed = [](void* context) { static_cast<ProgressReporter*>(context)->pixel_completed(); },
                                                         .images_invalidated = [](void* context, std::uint64_t count) { static_cast<ProgressReporter*>(context)->invalidate_pixels(count); }
                                                        }
                                                      : BenchmarkWriteProgressEvent{};
  const auto empty_train = make_split("train"), empty_validation = make_split("val");
  const BenchmarkWriteRequest train_pixel_request{
   empty_train, staging_dir / "train.bin", config.resolution, static_cast<int>(pixel_lanes), {}, false, cancel_requested, pixel_progress, config.perceptual_downscale, config.resize_mode, image_opened,
   {}, &pipeline
  };
  const BenchmarkWriteRequest validation_pixel_request{
   empty_validation, staging_dir / "val.bin", config.resolution, static_cast<int>(pixel_lanes), {}, false, cancel_requested, pixel_progress, config.perceptual_downscale, config.resize_mode,
   image_opened, {}, &pipeline
  };
  retained_train_pixels = std::make_unique<BenchmarkSplitWriter>(train_pixel_request, true);
  retained_validation_pixels = std::make_unique<BenchmarkSplitWriter>(validation_pixel_request, true);
  auto& train_writer = *retained_train_pixels;
  auto& validation_writer = *retained_validation_pixels;
  BenchmarkCompilePipeline::Attempt retire_pixels(pipeline);
  StopFailedAttempt stop_pixel_attempt{cancellation_state};
  pipeline.membership_ready();
  pipeline.bind_source(cache.source_images("coco") / "train2017", train_writer, AnnotationSource::Coco);
  pipeline.bind_source(cache.source_images("coco") / "val2017", validation_writer, AnnotationSource::Coco);
  pipeline.bind_source(cache.source_images("open-images") / "train", train_writer, AnnotationSource::OpenImages);
  for (std::size_t shard = 0; shard < custom_catalog.objects_images.size(); ++shard)
   pipeline.bind_source(cache.source_images("objects365") / ("patch-" + std::to_string(shard)), train_writer, AnnotationSource::Objects365);
  constexpr std::size_t kLabelPlanCount = 4;
  progress.label_plans(kLabelPlanCount);
  const auto publish_selected_labels = [&](const NormalizedAnnotationReadView& index) {
   const std::size_t slot = index.source == BenchmarkDatasetSource::kCoco2017 ? (index.split == "val2017" ? 3U : 0U) : index.source == BenchmarkDatasetSource::kObjects365V2 ? 1U : 2U;
   progress.label_plan_started(slot);
   if (source_labels_started) source_labels_started(index.source, index.split);
   BenchmarkCompletionWindow labels(std::clamp<std::size_t>(pipeline.execution().workers() * 2, 2, 64));
   for (std::size_t position = 0; position < index.image_count(); ++position) {
    const auto& image = index.image(position);
    if (!image.width || !image.height) continue;
    throw_if_benchmark_cancelled(cancel_requested);
    labels.make_room();
    const auto root = index.source == BenchmarkDatasetSource::kCoco2017       ? cache.source_images("coco") / index.split
                      : index.source == BenchmarkDatasetSource::kObjects365V2 ? cache.source_images("objects365") / ("patch-" + std::to_string(image.source_shard))
                                                                              : cache.source_images("open-images") / "train";
    const auto id = image.source_image_id;
    const auto generation = pipeline.image_generation(root, id);
    // Acquisition can retire a damaged image while these early labels run.
    // Required assembly replays missing labels after that source settles.
    if (!pipeline.labels_ready(pipeline.source_publication(root, {}, id), id, BenchmarkLabelInput(index, position), index.annotation_sha256)) continue;
    labels.push(pipeline.label_completion(root, id, index.annotation_sha256), [&, root, id, generation](std::exception_ptr error) {
     throw_if_benchmark_cancelled(cancel_requested);
     if (error && pipeline.image_generation(root, id) == generation) std::rethrow_exception(error);
    });
   }
   labels.drain();
  };
  const auto custom_transfer_budget = input_budget;
  if (custom_transfer_budget) {
   // Prefetched leases may wait for metadata to select images. Reserve that
   // metadata's existing envelope before either prefetch can retain capacity.
   preparation_allowance = pipeline.execution().reserve(CompileResources::handles(0, true, custom_annotation_resources().descriptors + custom_annotation_resources().continuation_descriptors));
   // These two source controllers wait on I/O and lifecycle custody. CPU
   // parsing remains in the pipeline; a CPU-clamped pool would serialize
   // independent sources when the compile owns a single CPU lane.
   for (std::size_t source = 0; source < 2; ++source)
    custom_transfers[source] = std::async(std::launch::async, [&, source] {
     try {
      const std::string shard = source ? "val2017" : "train2017";
      const auto root = cache.source_images("coco") / shard;
      // A proof allows the selection-aware owner to admit a warm group without
      // requiring its original archive to remain present.
      if (std::filesystem::is_regular_file(root / ".complete.json")) return PrefetchedArchive{};
      auto lease = ArtifactLease::acquire_charged(cache.locks / ("coco-" + shard + ".images.lock"), cancel_requested, &pipeline.execution(), archive_image_resources());
      const auto request = make_download_request(cache, "coco", source ? custom_catalog.coco_val_images : custom_catalog.coco_train_images);
      auto download = download_artifacts({request}, pipeline.curl().limit(BenchmarkCurl::Class::Artifact), cancel_requested,
       progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const DownloadProgress& update) { physical_progress.update(update, progress); }} : DownloadProgressSink{}, trace, {}, &pipeline,
       lease->allowance())
                       .front();
      trace_benchmark_event(trace, "benchmark.archive.prefetched", [&] { return nlohmann::json{{"path", download.path.string()}, {"shard", shard}}; });
      throw_if_benchmark_cancelled(cancel_requested);
      return PrefetchedArchive{std::move(download), std::move(lease)};
     } catch (...) {
      fail_compile(std::current_exception());
      throw;
     }
    });
  }
  std::array<std::vector<std::uint64_t>, 2> early_coco_ids;
  std::array<bool, 2> early_coco_ready{};
  std::array<std::future<CachedImageDirectory>, 2> early_coco_images;
  StopFailedAttempt stop_early_inputs{cancellation_state};
  const auto start_coco_images = [&](const NormalizedAnnotationReadView& index, std::size_t source) {
   if (early_coco_images[source].valid()) return;
   early_coco_ids[source] = image_ids(index);
   early_coco_images[source] = std::async(std::launch::async, [&, source] {
    struct Ready {
     std::mutex& mutex;
     bool& ready;
     BenchmarkCompilePipeline& pipeline;
     ~Ready() {
      {
       const std::lock_guard lock(mutex);
       ready = true;
      }
      pipeline.execution().notify_admission_change();
     }
    } ready{archive_readiness_mutex, early_coco_ready[source], pipeline};
    try {
     auto prefetched = custom_transfers[source].valid() ? custom_transfers[source].get() : PrefetchedArchive{};
     detail::StorageReservationPool storage(cache.root, storage_observer(trace), &pipeline.storage());
     return acquire_archive_images(cache, BenchmarkDatasetSource::kCoco2017, source ? "val2017" : "train2017", early_coco_ids[source],
      source ? custom_catalog.coco_val_images : custom_catalog.coco_train_images, cancel_requested, &progress, &progress.transfers(), &storage,
      custom_catalog.coco_train_images_count + custom_catalog.coco_validation_images_count, 1, pipeline.curl().limit(BenchmarkCurl::Class::Artifact), trace, {},
      prefetched.download ? &*prefetched.download : nullptr, std::move(prefetched.lease), &pipeline);
    } catch (...) {
     fail_compile(std::current_exception());
     throw;
    }
   });
  };
  {
   const auto preparation_budget = effective_num_workers;
   custom = prepare_custom_recipe(config, cache, custom_catalog, progress, preparation_budget, cancel_requested, trace, std::span(compile_cpus).last(preparation_budget), &pipeline,
    std::move(preparation_allowance), [&](CocoAnnotationSplit split) {
    const auto root = cache.source_images("coco") / (split.training ? "train2017" : "val2017");
    pipeline.original_generation(root, split.generation, !split.index);
    if (split.index) {
     const NormalizedAnnotationReadView index(std::move(*split.index));
     start_coco_images(index, split.training ? 0 : 1);
     publish_selected_labels(index);
    }
   }, publish_selected_labels);
  }
  auto& coco_train_index_path = custom.coco_train_index_path;
  auto& coco_val_index_path = custom.coco_val_index_path;
  auto& objects_index_path = custom.objects_index_path;
  auto& open_images_index_path = custom.open_images_index_path;
  auto& coco_train = custom.coco_train;
  auto& coco_val = custom.coco_val;
  auto& objects = custom.objects;
  auto& open_images = custom.open_images;
  auto& coco_indexes_cache_hit = custom.coco_indexes_cache_hit;
  auto& objects_index_cache_hit = custom.objects_index_cache_hit;
  auto& open_images_index_cache_hit = custom.open_images_index_cache_hit;
  auto& combined_sampling = custom.combined_sampling;
  auto& objects_sampling = custom.objects_sampling;
  auto& open_images_sampling = custom.open_images_sampling;
  auto& sampling_object_artifacts = custom.sampling_object_artifacts;
  std::uint64_t selected_train_images = 0, selected_val_images = 0, selected_train_labels = 0, selected_val_labels = 0;
  std::uint64_t train_masks = 0, val_masks = 0;
  const auto count_index = [&](const NormalizedAnnotationReadView& index, bool validation) {
   auto& images = validation ? selected_val_images : selected_train_images;
   auto& labels = validation ? selected_val_labels : selected_train_labels;
   auto& masks = validation ? val_masks : train_masks;
   images = common_math::checked_add(images, index.image_count(), "benchmark selected image count overflow");
   labels = common_math::checked_add(labels, index.box_count(), "benchmark selected label count overflow");
   masks = common_math::checked_add(masks, index.run_count(), "benchmark selected mask count overflow");
  };
  std::vector<std::uint64_t> archive_sizes;
  {
   count_index(*coco_train, false);
   count_index(*objects, false);
   count_index(*open_images, false);
   count_index(*coco_val, true);
   archive_sizes = {custom_catalog.coco_train_images.expected_size, custom_catalog.coco_val_images.expected_size};
   for (const auto shard : combined_sampling.objects365_shards) archive_sizes.push_back(sampling_object_artifacts.at(shard).expected_size);
  }
  const auto selected_total_images = common_math::checked_add(selected_train_images, selected_val_images, "benchmark image count overflow");
  if (progress.pixel_observer_enabled()) progress.pixels(train_writer.completed() + validation_writer.completed(), selected_total_images);
  const auto projected_output_bytes = common_math::checked_add(estimate_output_bytes(selected_train_images, selected_train_labels, config.resolution, train_masks),
   estimate_output_bytes(selected_val_images, selected_val_labels, config.resolution, val_masks), "benchmark projected output overflow");
  std::uint64_t retained_archive_bytes = 0U;
  for (const std::uint64_t archive_size : archive_sizes) { retained_archive_bytes = common_math::checked_add(retained_archive_bytes, archive_size, "benchmark archive cache estimate overflow"); }
  const std::uint64_t projected_cache = common_math::checked_add(common_math::checked_multiply(selected_total_images, kEstimatedJpegBytes, "benchmark cache estimate overflow"),
   common_math::checked_add(retained_archive_bytes, 0U, "benchmark annotation cache estimate overflow"), "benchmark cache estimate overflow");
  trace_benchmark_event(trace, "benchmark.storage.projection",
   [&] { return nlohmann::json{{"cache_bytes_upper_bound", projected_cache}, {"compiled_output_bytes", projected_output_bytes}, {"selected_images", selected_total_images}}; });
  progress.projected(projected_output_bytes);
  progress.activity("Checking projected benchmark storage");
  require_storage(cache.root, projected_cache, "benchmark image cache and active archives", trace);
  const auto retained_allocation = (retained_train_pixels ? retained_train_pixels->allocated_bytes() : 0) + (retained_validation_pixels ? retained_validation_pixels->allocated_bytes() : 0);
  require_storage(output_parent, projected_output_bytes > retained_allocation ? projected_output_bytes - retained_allocation : 0, "benchmark pixels and currently admitted annotations", trace);
  progress.phase(DatasetCompilePhase::Extracting);
  if (selection_started) selection_started();
  auto& image_transfer_progress = progress.transfers();
  detail::StorageReservationPool cache_storage_reservations(cache.root, storage_observer(trace), &pipeline.storage());
  std::vector<QuarantinedImage> quarantined;
  const std::vector<std::uint64_t> coco_train_ids = image_ids(*coco_train);
  const std::vector<std::uint64_t> coco_val_ids = image_ids(*coco_val);
  const std::uint64_t coco_selected_images = common_math::checked_add(coco_train_ids.size(), coco_val_ids.size(), "COCO selected image count overflow");
  const std::vector<CatalogArtifact> object_artifacts = custom_catalog.objects_images;
  struct ArchiveTask {
   BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
   std::string shard;
   std::vector<std::uint64_t> image_ids;
   CatalogArtifact artifact;
   std::uint16_t numeric_shard = 0;
   std::filesystem::path root{};
  };
  std::vector<ArchiveTask> archive_tasks;
  {
   archive_tasks.reserve(object_artifacts.size() + 2U);
   archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "train2017", coco_train_ids, custom_catalog.coco_train_images});
   archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "val2017", coco_val_ids, custom_catalog.coco_val_images});
   std::vector<std::vector<std::uint64_t>> shard_ids(object_artifacts.size());
   for (const auto& image : objects->images())
    if (image.source_shard < shard_ids.size()) shard_ids[image.source_shard].push_back(image.source_image_id);
   for (std::size_t shard = 0U; shard < object_artifacts.size(); ++shard) {
    const std::uint16_t numeric_shard = common_math::checked_cast<std::uint16_t>(shard, "Objects365 shard index overflow");
    auto ids = std::move(shard_ids[shard]);
    if (!ids.empty()) { archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kObjects365V2, "patch-" + std::to_string(shard), std::move(ids), object_artifacts[shard], numeric_shard}); }
   }
  }
  if (archive_tasks.empty()) throw std::runtime_error("benchmark recipe has no physical image membership");
  std::map<BenchmarkDatasetSource, std::uint64_t> source_totals;
  std::map<std::pair<BenchmarkDatasetSource, std::string>, std::vector<std::size_t>> source_tasks;
  std::map<std::filesystem::path, std::size_t> root_tasks;
  for (std::size_t i = 0; i < archive_tasks.size(); ++i) {
   auto& task = archive_tasks[i];
   task.root = cache.source_images(benchmark_source_name(task.source)) / task.shard;
   root_tasks.emplace(task.root, i);
   source_totals[task.source] = common_math::checked_add(source_totals[task.source], task.image_ids.size(), "benchmark source total overflow");
   source_tasks[{task.source, task.source == BenchmarkDatasetSource::kCoco2017 ? task.shard : std::string{}}].push_back(i);
  }
  // Assign stable physical slots for the current normalized membership.
  // Label projection and acquisition publish independently into these slots.
  PreparedBenchmarkSplit pixel_train = make_split("train"), pixel_validation = make_split("val");
  const auto append_pixel_members = [&](const NormalizedAnnotationReadView& index, bool validation_split) {
   auto& split = validation_split ? pixel_validation : pixel_train;
   std::unordered_map<std::uint16_t, std::uint16_t> source_slots;
   const auto& tasks = source_tasks[{index.source, index.source == BenchmarkDatasetSource::kCoco2017 ? index.split : std::string{}}];
   for (const auto task_index : tasks) {
    const auto& task = archive_tasks[task_index];
    source_slots.emplace(task.numeric_shard, common_math::checked_cast<std::uint16_t>(split.sources.size(), "benchmark pixel source count overflow"));
    split.sources.push_back({task.root});
   }
   if (index.source == BenchmarkDatasetSource::kOpenImagesV7) {
    source_slots.emplace(0, common_math::checked_cast<std::uint16_t>(split.sources.size(), "benchmark pixel source count overflow"));
    split.sources.push_back({cache.source_images("open-images") / "train"});
   }
   for (std::size_t i = 0; i < index.image_count(); ++i) {
    const auto& image = index.image(i);
    if (!validation_split && !image.box_count) continue;
    const auto shard = image.source_shard;
    split.images.push_back({image.source_image_id, image.width, image.height, 0, source_slots.at(shard)});
   }
  };
  {
   append_pixel_members(*coco_train, false);
   append_pixel_members(*objects, false);
   append_pixel_members(*open_images, false);
   append_pixel_members(*coco_val, true);
  }
  std::vector<std::optional<CachedImageDirectory>> archive_results(archive_tasks.size());
  auto train = make_split("train");
  auto validation = make_split("val");
  const auto append_metadata = [&](PreparedBenchmarkSplit& target, PreparedBenchmarkSplit source) {
   const auto base = target.sources.size();
   for (auto image : source.images) {
    image.source_index = common_math::checked_cast<std::uint16_t>(base + image.source_index, "benchmark source count overflow");
    target.images.push_back(image);
   }
   target.sources.insert(target.sources.end(), std::make_move_iterator(source.sources.begin()), std::make_move_iterator(source.sources.end()));
   target.labels = common_math::checked_add(target.labels, source.labels, "benchmark label count overflow");
   target.runs = common_math::checked_add(target.runs, source.runs, "benchmark run count overflow");
  };
  const std::size_t configured_workers = pipeline.execution().workers();
  const int acquisition_num_workers = static_cast<int>(configured_workers);
  const std::size_t open_cache_workers = 0;
  std::optional<AcquiredOpenImages> open_image_result;
  std::mutex repair_mutex;
  std::set<std::pair<std::filesystem::path, std::uint64_t>> repaired_images;
  const std::size_t decompression_workers = 1;
  const std::size_t archive_download_connections = pipeline.curl().limit(BenchmarkCurl::Class::Artifact);
  const BenchmarkImageRepair repair_image_input = [&](const std::filesystem::path& source_root, const BenchmarkImageReadError& error) {
   {
    const std::lock_guard lock(repair_mutex);
    if (!repaired_images.emplace(source_root, error.source_image_id()).second) throw error;
   }
   const EncodedImageRecord* failed_record = nullptr;
   for (const auto* split : {&pixel_train, &pixel_validation}) {
    const auto found = std::ranges::find_if(
     split->images, [&](const EncodedImageRecord& image) { return image.source_image_id == error.source_image_id() && split->sources.at(image.source_index).root == source_root; });
    if (found != split->images.end()) {
     failed_record = &*found;
     break;
    }
   }
   if (!failed_record) throw std::runtime_error("benchmark image repair record is absent from the pixel membership");
   const auto geometry = pipeline.geometry(source_root, error.source_image_id());
   const ImageDecodeProbe decode_probe{error.source_image_id(), geometry ? geometry->width : failed_record->source_width, geometry ? geometry->height : failed_record->source_height};
   trace_benchmark_event(
    trace, "benchmark.pixel_compile.cache_repair", [&] { return nlohmann::json{{"source", source_root.string()}, {"image_id", error.source_image_id()}, {"attempt", 1}, {"reason", error.what()}}; });
   // Retain the failed generation before withdrawing its pipeline product.
   {
    const auto failed_input = trace ? pipeline.image_input(source_root, error.source_image_id()) : nullptr;
    pipeline.retire_image(source_root, error.source_image_id());
    if (trace) try {
      const auto image_path = cached_image_path(source_root, error.source_image_id());
      if ((failed_input && !failed_input->encoded().empty()) || std::filesystem::is_regular_file(image_path)) {
       throw_if_benchmark_cancelled(cancel_requested);
       const auto digest = common_io::sha256_hex(
        failed_input && !failed_input->encoded().empty() ? common_io::sha256_bytes(failed_input->encoded()) : common_io::sha256_file(image_path, [&] { return cancel_requested.requested(); }));
       trace_benchmark_event(
        trace, "benchmark.images.failure_sha256", [&] { return nlohmann::json{{"path", image_path.string()}, {"image_id", error.source_image_id()}, {"sha256", digest}, {"reason", error.what()}}; });
      }
     } catch (...) { throw_if_benchmark_cancelled(cancel_requested); }
   }  // Release the failed encoded backing before replacement admission.
   if (source_root == cache.source_images("open-images") / "train") {
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Repairing failed Open Images JPEG " + std::to_string(error.source_image_id()));
    std::vector<QuarantinedImage> repaired_quarantined;
    open_image_result = acquire_open_images(cache, *open_images, &repaired_quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace, decode_probe, &pipeline);
    quarantined = std::move(repaired_quarantined);
    return std::ranges::binary_search(open_image_result->available_image_ids, error.source_image_id());
   }
   const auto route = root_tasks.find(source_root);
   if (route == root_tasks.end()) throw std::runtime_error("benchmark image repair cannot identify its source archive");
   const auto& task = archive_tasks[route->second];
   progress.source_activity(task.source, "Repairing failed cached JPEG " + std::to_string(error.source_image_id()) + " from " + task.shard);
   auto repaired = acquire_archive_images(cache, task.source, task.shard, task.image_ids, task.artifact, cancel_requested, &progress, &progress.transfers(), &cache_storage_reservations,
    source_totals.at(task.source), decompression_workers, archive_download_connections, trace, decode_probe, nullptr, {}, &pipeline);
   if (repaired.image_count + repaired.quarantined.size() != task.image_ids.size()) throw std::runtime_error("archive repair did not resolve every selected image");
   const bool available = std::ranges::none_of(repaired.quarantined, [&](const auto& rejected) { return rejected.image_id == error.source_image_id(); });
   archive_results[route->second] = std::move(repaired);
   return available;
  };
  std::mutex custom_labels_mutex;
  std::array<PreparedBenchmarkSplit, 4> custom_label_plans;
  std::array<SourceCompileCount, 4> custom_label_counts{};
  std::array<bool, 4> custom_labels_ready{};
  std::atomic<std::size_t> pending_object_archives{archive_tasks.size() - 2};
  const auto prepare_source_labels = [&](std::size_t slot, const NormalizedAnnotationReadView& index, std::vector<CachedImageDirectory> directories) {
   if (source_images_acquired) source_images_acquired(index.source, index.split);
   std::vector<std::uint64_t> unavailable;
   for (const auto& directory : directories)
    for (const auto& rejected : directory.quarantined) unavailable.push_back(rejected.image_id);
   if (slot == 2)
    for (const auto& image : quarantined)
     if (image.source == BenchmarkDatasetSource::kOpenImagesV7) unavailable.push_back(image.image_id);
   std::ranges::sort(unavailable);
   unavailable.erase(std::unique(unavailable.begin(), unavailable.end()), unavailable.end());
   progress.label_plan_started(slot);
   auto plan = make_split(slot == 3 ? "val" : "train");
   custom_label_counts[slot] = append_source_plan(
    pipeline, repair_image_input, index, directories, unavailable.empty() ? nullptr : &unavailable, slot == 3, &plan, slot == 3 ? validation_writer : train_writer, cancel_requested, 0);
   {
    const std::lock_guard lock(custom_labels_mutex);
    custom_label_plans[slot] = std::move(plan);
    custom_labels_ready[slot] = true;
    if (slot == 3)
     validation = std::move(custom_label_plans[slot]);
    else if (custom_labels_ready[0] && custom_labels_ready[1] && custom_labels_ready[2]) {
     for (std::size_t source = 0; source < 3; ++source) append_metadata(train, std::move(custom_label_plans[source]));
    }
   }
   progress.label_plan_completed(slot);
  };
  const auto archive_labels_ready = [&](std::size_t task) {
   if (task < 2)
    prepare_source_labels(task == 0 ? 0 : 3, task == 0 ? *coco_train : *coco_val, {*archive_results[task]});
   else if (pending_object_archives.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::vector<CachedImageDirectory> directories;
    for (std::size_t i = 2; i < archive_results.size(); ++i) directories.push_back(*archive_results[i]);
    prepare_source_labels(1, *objects, std::move(directories));
   }
  };
  std::exception_ptr pipeline_error;
  std::mutex pipeline_error_mutex;
  const auto record_pipeline_error = [&] {
   const std::lock_guard lock(pipeline_error_mutex);
   if (!pipeline_error) {
    pipeline_error = std::current_exception();
    fail_compile(pipeline_error);
   }
  };
  // Validation depends only on its own completed releases/original split and
  // physical geometry. It does not depend on training release completion.
  SourceCompileCount validation_count;
  std::uint64_t validation_target_dropped_boxes = 0;
  StopFailedAttempt stop_validation_attempt{cancellation_state};
  // These are I/O controllers. Their CPU chunks use the compile owner, with
  // no reserved pixel/input/archive partition or nested cache CPU pool.
  const bool overlap_open_images = true;
  const int archive_workers = static_cast<int>(std::min<std::size_t>(archive_tasks.size(), kArchivePipelineConcurrency));
  std::thread open_images_thread;
  if (overlap_open_images) {
   open_images_thread = std::thread([&] {
    try {
     const auto cpus = pipeline.execution().cpus();
     common_system::set_thread_affinity(std::vector<int>(cpus.begin(), cpus.end()));
     open_image_result = acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace, {}, &pipeline);
     prepare_source_labels(2, *open_images, {open_image_result->directory});
    } catch (...) { record_pipeline_error(); }
   });
  }
  try {
   struct WaitingArchive {
    std::size_t index;
    Clock::time_point retry{};
   };
   std::list<WaitingArchive> waiting_archives;
   for (std::size_t index = 0; index < archive_tasks.size(); ++index) waiting_archives.push_back({index});
   // A completed prefetch hands its value into selection exactly once. Warm
   // proofs can return without a lease; those sources use the same charged
   // nonblocking lock admission as every other unclaimed archive.
   std::array<PrefetchedArchive, 2> ready_prefetch;
   const auto take_archive = [&] {
    for (;;) {
     trace_benchmark_event(trace, "benchmark.archive.selection", [] { return nlohmann::json::object(); });
     const auto observed = pipeline.execution().admission_generation();
     std::unique_lock lock(archive_readiness_mutex);
     std::pair<std::size_t, PrefetchedArchive> selected{archive_tasks.size(), {}};
     auto deadline = Clock::time_point::max();
     throw_if_benchmark_cancelled(cancel_requested);
     for (auto it = waiting_archives.begin(); it != waiting_archives.end(); ++it) {
      const auto index = it->index;
      const bool coco = index < custom_transfers.size();
      if (coco && early_coco_images[index].valid()) {
       if (!early_coco_ready[index]) continue;
       auto images = early_coco_images[index].get();
       if (early_coco_ids[index] == archive_tasks[index].image_ids) ready_prefetch[index].images = std::move(images);
      }
      if (coco && ready_prefetch[index].images) {
       selected = {index, std::move(ready_prefetch[index])};
       waiting_archives.erase(it);
       break;
      }
      if (it->retry > Clock::now()) {
       deadline = std::min(deadline, it->retry);
       continue;
      }
      std::shared_ptr<ArtifactLease> lease;
      if (coco) lease = ready_prefetch[index].lease;
      if (!lease) {
       // A sole controller never claims independent work while either its
       // physical lock or complete source continuation is unavailable.
       auto grant = pipeline.execution().try_reserve(archive_image_resources());
       if (!grant) continue;
       const auto& task = archive_tasks[index];
       const auto path = cache.locks / (std::string(benchmark_source_name(task.source)) + "-" + task.shard + ".images.lock");
       lease = ArtifactLease::try_acquire_charged(path, cancel_requested, std::move(*grant));
       if (!lease) {
        // flock alone has no readiness fd. Failed attempts release all
        // credits and cannot consume a controller or block a ready source.
        it->retry = Clock::now() + std::chrono::milliseconds{100};
        deadline = std::min(deadline, it->retry);
        continue;
       }
      }
      selected.first = index;
      if (coco) selected.second = std::move(ready_prefetch[index]);
      selected.second.lease = std::move(lease);
      waiting_archives.erase(it);
      break;
     }
     if (selected.first != archive_tasks.size() || waiting_archives.empty()) {
      lock.unlock();
      pipeline.execution().notify_admission_change();
      return selected;
     }
     lock.unlock();
     trace_benchmark_event(trace, "benchmark.archive.admission_wait", [&] {
      const auto used = pipeline.execution().resource_usage();
      return nlohmann::json{
       {"reason", "source_admission"}, {"bytes", used.bytes}, {"descriptors", used.descriptors}, {"committed_descriptors", used.continuation_descriptors},
       {"descriptor_capacity", pipeline.execution().descriptor_limit()}, {"producer_ceiling", pipeline.execution().descriptor_ceiling(archive_image_resources())}, {"active_cpus", used.cpu_workers}
      };
     });
     try {
      throw_if_benchmark_cancelled(cancel_requested);
      pipeline.execution().wait_for_admission_change(observed, deadline);
     } catch (...) {
      trace_benchmark_event(trace, "benchmark.archive.admission_stopped", [] { return nlohmann::json::object(); });
      throw;
     }
    }
   };
   common_concurrency::parallel_for_range_indexed<int>(0, archive_workers, archive_workers, pipeline.execution().cpus(), [&](const int, const int begin, const int end) {
    for (int worker = begin; worker < end; ++worker) {
     const auto cpus = pipeline.execution().cpus();
     common_system::set_thread_affinity(std::vector<int>(cpus.begin(), cpus.end()));
     while (true) {
      auto [task_index, prefetch] = take_archive();
      if (task_index >= archive_tasks.size()) { break; }
      const ArchiveTask& task = archive_tasks[task_index];
      const std::uint64_t source_total = source_totals.at(task.source);
      try {
       if (prefetch.images)
        archive_results[task_index] = std::move(prefetch.images);
       else
        archive_results[task_index] =
         acquire_archive_images(cache, task.source, task.shard, task.image_ids, task.artifact, cancel_requested, &progress, &image_transfer_progress, &cache_storage_reservations, source_total,
          decompression_workers, archive_download_connections, trace, {}, prefetch.download ? &*prefetch.download : nullptr, std::move(prefetch.lease), &pipeline);
       archive_labels_ready(task_index);
      } catch (...) {
       record_pipeline_error();
       return;
      }
     }
    }
   });
  } catch (...) { record_pipeline_error(); }
  if (!overlap_open_images && !pipeline_error) {
   try {
    common_concurrency::parallel_for_range_indexed<int>(0, 1, 1, pipeline.execution().cpus(), [&](int, int, int) {
     open_image_result = acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace, {}, &pipeline);
     prepare_source_labels(2, *open_images, {open_image_result->directory});
    });
   } catch (...) { record_pipeline_error(); }
  }
  if (open_images_thread.joinable()) { open_images_thread.join(); }
  if (pipeline_error) {
   try {
    pipeline.drain();
   } catch (...) { /* Every reader has retired; propagate the initiating failure. */
   }
   cancellation_state.rethrow_failure();
   std::rethrow_exception(pipeline_error);
  }
  if (!open_image_result) { throw std::runtime_error("Open Images acquisition did not complete"); }
  std::vector<CachedImageDirectory> coco_train_images;
  std::vector<CachedImageDirectory> coco_val_images;
  std::vector<CachedImageDirectory> object_images;
  std::vector<CachedImageDirectory> open_image_directories;
  {
   std::optional<CachedImageDirectory>& coco_train_result = archive_results[0];
   std::optional<CachedImageDirectory>& coco_val_result = archive_results[1];
   if (!coco_train_result.has_value() || !coco_val_result.has_value()) { throw std::runtime_error("COCO image extraction did not complete"); }
   coco_train_images.push_back(std::move(*coco_train_result));
   coco_val_images.push_back(std::move(*coco_val_result));
   object_images.reserve(archive_results.size() - 2U);
   for (std::size_t task_index = 2U; task_index < archive_results.size(); ++task_index) {
    std::optional<CachedImageDirectory>& archive_result = archive_results[task_index];
    if (!archive_result.has_value()) { throw std::runtime_error("Objects365 shard extraction did not complete"); }
    object_images.push_back(std::move(*archive_result));
   }
   open_image_directories.push_back(open_image_result->directory);
   if (coco_train_images.front().image_count + coco_train_images.front().quarantined.size() != coco_train_ids.size() || !coco_val_images.front().quarantined.empty() ||
       coco_val_images.front().image_count != coco_val_ids.size()) {
    throw std::runtime_error("COCO acquisition did not restore every selected image");
   }
   for (std::size_t index = 0U; index < object_images.size(); ++index) {
    if (object_images[index].image_count + object_images[index].quarantined.size() != archive_tasks[index + 2U].image_ids.size()) {
     throw std::runtime_error(
      "Objects365 acquisition did not resolve every selected "
      "training image");
    }
   }
   if (open_image_result->available_image_ids.size() + quarantined.size() != open_images->image_count() || open_image_result->directory.image_count != open_image_result->available_image_ids.size()) {
    throw std::runtime_error("Open Images acquisition resolution count is inconsistent");
   }
  }
  std::vector<std::uint64_t> coco_train_unavailable_ids;
  std::vector<std::uint64_t> objects_unavailable_ids;
  std::vector<std::uint64_t> open_images_unavailable_ids;
  const auto refresh_archive_availability = [&] {
   quarantined.erase(std::remove_if(quarantined.begin(), quarantined.end(), [](const QuarantinedImage& image) { return image.source != BenchmarkDatasetSource::kOpenImagesV7; }), quarantined.end());
   coco_train_unavailable_ids.clear();
   objects_unavailable_ids.clear();
   open_images_unavailable_ids.clear();
   coco_train_unavailable_ids.reserve(coco_train_images.front().quarantined.size());
   for (const CachedImageRejection& rejection : coco_train_images.front().quarantined) {
    coco_train_unavailable_ids.push_back(rejection.image_id);
    quarantined.push_back(QuarantinedImage{BenchmarkDatasetSource::kCoco2017, rejection.image_id, rejection.reason});
   }
   std::size_t unavailable_object_count = 0U;
   for (const CachedImageDirectory& directory : object_images) { unavailable_object_count += directory.quarantined.size(); }
   objects_unavailable_ids.reserve(unavailable_object_count);
   for (const CachedImageDirectory& directory : object_images) {
    for (const CachedImageRejection& rejection : directory.quarantined) {
     objects_unavailable_ids.push_back(rejection.image_id);
     quarantined.push_back(QuarantinedImage{BenchmarkDatasetSource::kObjects365V2, rejection.image_id, rejection.reason});
    }
   }
   for (const QuarantinedImage& image : quarantined) {
    if (image.source == BenchmarkDatasetSource::kOpenImagesV7) { open_images_unavailable_ids.push_back(image.image_id); }
   }
   const auto sort_unique = [](std::vector<std::uint64_t>* ids) {
    std::ranges::sort(*ids);
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
   };
   sort_unique(&coco_train_unavailable_ids);
   sort_unique(&objects_unavailable_ids);
   sort_unique(&open_images_unavailable_ids);
  };
  {
   refresh_archive_availability();
   progress.source_images(BenchmarkDatasetSource::kCoco2017, coco_selected_images, coco_selected_images);
   progress.source_complete(BenchmarkDatasetSource::kCoco2017, coco_indexes_cache_hit && coco_train_images.front().cache_hit && coco_val_images.front().cache_hit);
   progress.source_complete(
    BenchmarkDatasetSource::kObjects365V2, objects_index_cache_hit && std::ranges::all_of(object_images, [](const CachedImageDirectory& directory) { return directory.cache_hit; }));
   progress.source_complete(BenchmarkDatasetSource::kOpenImagesV7, open_images_index_cache_hit && open_image_result->directory.cache_hit);
  }
  progress.acquisition_complete();
  {}
  progress.phase(DatasetCompilePhase::Labels, 0, kLabelPlanCount);
  std::uint64_t training_target_dropped_boxes = 0U;
  std::vector<SourceCompileCount> source_counts;
  const auto prepare_training_plan = [&] {
   auto prepared = make_split("train");
   std::vector<SourceCompileCount> prepared_counts;
   prepared_counts.reserve(3U);
   std::uint64_t prepared_dropped_boxes = 0U;
   progress.label_plan_started(0);
   prepared_counts.push_back(append_source_plan(
    pipeline, repair_image_input, *coco_train, coco_train_images, coco_train_unavailable_ids.empty() ? nullptr : &coco_train_unavailable_ids, false, &prepared, train_writer, cancel_requested, 0));
   progress.label_plan_completed(0);
   progress.label_plan_started(1);
   prepared_counts.push_back(append_source_plan(
    pipeline, repair_image_input, *objects, object_images, objects_unavailable_ids.empty() ? nullptr : &objects_unavailable_ids, false, &prepared, train_writer, cancel_requested, 0));
   progress.label_plan_completed(1);
   progress.label_plan_started(2);
   prepared_counts.push_back(append_source_plan(pipeline, repair_image_input, *open_images, open_image_directories, open_images_unavailable_ids.empty() ? nullptr : &open_images_unavailable_ids, false,
    &prepared, train_writer, cancel_requested, 0));
   progress.label_plan_completed(2);
   train = std::move(prepared);
   source_counts = std::move(prepared_counts);
   training_target_dropped_boxes = prepared_dropped_boxes;
  };
  if (!std::ranges::all_of(custom_labels_ready, [](bool ready) { return ready; })) throw std::logic_error("benchmark source label preparation did not settle");
  for (std::size_t source = 0; source < 3; ++source) source_counts.push_back(custom_label_counts[source]);
  validation_count = custom_label_counts[3];
  progress.phase(DatasetCompilePhase::Labels, kLabelPlanCount, kLabelPlanCount);
  const auto expected_validation_images = custom_catalog.coco_validation_images_count;
  if (validation.images.size() != expected_validation_images) throw std::runtime_error("compiled validation plan does not match admitted membership");
  const auto calculate_output_estimate = [&] {
   return common_math::checked_add(estimate_output_bytes(train.images.size(), train.labels, config.resolution, train.runs),
    estimate_output_bytes(validation.images.size(), validation.labels, config.resolution, validation.runs), "benchmark total output estimate overflow");
  };
  std::uint64_t total_output_estimate = calculate_output_estimate();
  progress.projected(total_output_estimate);
  progress.activity("Checking exact compiled output storage");
  const auto require_additional_output = [&](std::uint64_t estimate) {
   const auto allocated = common_math::checked_add(train_writer.allocated_bytes(), validation_writer.allocated_bytes(), "benchmark allocated output overflow");
   require_storage(output_parent, estimate > allocated ? estimate - allocated : 0, "additional benchmark compiled output staging", trace);
  };
  require_additional_output(total_output_estimate);
  pipeline.drain();
  std::uint64_t total_compile_images = common_math::checked_add(train.images.size(), validation.images.size(), "benchmark compile image count overflow");
  const auto refresh_output_after_rebuild = [&] {
   total_compile_images = common_math::checked_add(train.images.size(), validation.images.size(), "benchmark compile image count overflow");
   if (progress.pixel_observer_enabled()) progress.pixels(train_writer.completed() + validation_writer.completed(), total_compile_images);
   const std::uint64_t repaired_output_estimate = calculate_output_estimate();
   if (repaired_output_estimate > total_output_estimate) {
    progress.activity(
     "Checking expanded compiled output storage after image "
     "repair");
    require_additional_output(repaired_output_estimate);
   }
   total_output_estimate = repaired_output_estimate;
   progress.projected(total_output_estimate);
  };
  const auto rebuild_training_after_quarantine = [&] {
   progress.activity("Rebuilding training labels after image quarantine");
   prepare_training_plan();
   refresh_output_after_rebuild();
  };
  const auto replace_cached_directory = [](std::vector<CachedImageDirectory>* directories, CachedImageDirectory replacement) {
   const auto found = std::ranges::find(*directories, replacement.path, &CachedImageDirectory::path);
   if (found == directories->end()) { return false; }
   *found = std::move(replacement);
   return true;
  };
  const auto repair_cached_image = [&](const PreparedBenchmarkSplit& split, const BenchmarkImageReadError& error) {
   if (error.source_index() >= split.sources.size()) throw std::runtime_error("benchmark image repair source index is invalid");
   const std::filesystem::path source_root = split.sources[error.source_index()].root;
   (void)repair_image_input(source_root, error);
   if (source_root == cache.source_images("open-images") / "train") {
    open_image_directories.front() = open_image_result->directory;
    refresh_archive_availability();
    rebuild_training_after_quarantine();
    progress.source_complete(BenchmarkDatasetSource::kOpenImagesV7, open_images_index_cache_hit && open_image_result->directory.cache_hit);
    progress.acquisition_complete();
    return;
   }
   const auto route = root_tasks.find(source_root);
   const auto task = archive_tasks.begin() + route->second;
   auto repaired = *archive_results[route->second];
   {}
   bool replaced = replace_cached_directory(&coco_train_images, repaired);
   replaced = replace_cached_directory(&coco_val_images, repaired) || replaced;
   replaced = replace_cached_directory(&object_images, std::move(repaired)) || replaced;
   if (!replaced) {
    throw std::runtime_error(
     "benchmark image repair cannot update its cache "
     "directory");
   }
   if (archive_selection_allows_quarantine(task->source, task->shard)) {
    refresh_archive_availability();
    rebuild_training_after_quarantine();
   } else {
    auto prepared = make_split("val");
    validation_count = append_source_plan(pipeline, repair_image_input, *coco_val, coco_val_images, nullptr, true, &prepared, validation_writer, cancel_requested, 0);
    validation = std::move(prepared);
    refresh_output_after_rebuild();
   }
   const bool cache_hit = task->source == BenchmarkDatasetSource::kCoco2017
                           ? coco_indexes_cache_hit && coco_train_images.front().cache_hit && coco_val_images.front().cache_hit
                           : objects_index_cache_hit && std::ranges::all_of(object_images, [](const CachedImageDirectory& directory) { return directory.cache_hit; });
   progress.source_complete(task->source, cache_hit);
   progress.acquisition_complete();
  };
  const auto compile_split = [&](BenchmarkSplitWriter& writer, const BenchmarkWriteRequest& request) {
   for (;;) {
    try {
     write_split_with_progress(writer, request, &progress, trace);
     return;
    } catch (const BenchmarkImageReadError& error) {
     repair_cached_image(request.split, error);
     progress.activity("Retrying " + request.split.name +
                       " unfinished pixels after bounded cache "
                       "repair");
    }
   }
  };
  if (progress.pixel_observer_enabled()) progress.pixels(train_writer.completed() + validation_writer.completed(), total_compile_images);
  compile_split(train_writer,
   BenchmarkWriteRequest{train, staging_dir / "train.bin", config.resolution, config.num_workers, compile_cpus, false, cancel_requested, {}, config.perceptual_downscale, config.resize_mode});
  compile_split(validation_writer,
   BenchmarkWriteRequest{validation, staging_dir / "val.bin", config.resolution, config.num_workers, compile_cpus, false, cancel_requested, {}, config.perceptual_downscale, config.resize_mode});
  auto sealed_splits = seal_benchmark_splits(pipeline, [&](std::size_t i, std::span<std::byte> scratch) {
   auto& product = i == 0 ? train : validation;
   auto& writer = i == 0 ? train_writer : validation_writer;
   BenchmarkWriteRequest seal_request{
    product, staging_dir / (i == 0 ? "train.bin" : "val.bin"), config.resolution, config.num_workers, compile_cpus, false, cancel_requested, {}, config.perceptual_downscale, config.resize_mode
   };
   if (progress.pixel_observer_enabled()) seal_request.compaction = [&, i](std::size_t done, std::size_t total) { progress.compacting(i, done, total); };
   seal_request.trace = trace;
   seal_request.compaction_scratch = scratch;
   return writer.seal(seal_request);
  });
  for (auto& sealed : sealed_splits) sealed.artifact.publish(sealed.info.path, cancel_requested, detail::StagedArtifact::Publication::DurableReplace, false);
  progress.activity("Finalizing rejected and quarantined records");
  std::ranges::sort(quarantined);
  quarantined.erase(
   std::unique(quarantined.begin(), quarantined.end(), [](const QuarantinedImage& left, const QuarantinedImage& right) { return left.source == right.source && left.image_id == right.image_id; }),
   quarantined.end());
  for (const QuarantinedImage& image : quarantined) {
   trace_benchmark_event(trace, "benchmark.quarantine.image", [&] { return nlohmann::json{{"source", benchmark_source_name(image.source)}, {"image_id", image.image_id}, {"reason", image.reason}}; });
  }
  trace_benchmark_event(trace, "benchmark.quarantine.final", [&] { return nlohmann::json{{"images", quarantined.size()}}; });
  std::uint64_t annotation_drops = 0U;
  const auto add_annotation_drops = [&](const NormalizedAnnotationMetadata& index) {
   annotation_drops = common_math::checked_add(annotation_drops, index.rejected.degenerate_boxes, "benchmark rejected annotation count overflow");
   annotation_drops = common_math::checked_add(annotation_drops, index.rejected.duplicate_boxes, "benchmark rejected annotation count overflow");
  };
  {
   add_annotation_drops(*coco_train);
   add_annotation_drops(*coco_val);
   add_annotation_drops(*objects);
   add_annotation_drops(*open_images);
  }
  const std::uint64_t target_dropped_boxes = common_math::checked_add(training_target_dropped_boxes, validation_target_dropped_boxes, "benchmark target annotation count overflow");
  progress.rejected(common_math::checked_add(annotation_drops, target_dropped_boxes, "benchmark rejected annotation count overflow"), quarantined.size());
  constexpr std::uint64_t kSyncStepCount = 4U;
  progress.phase(DatasetCompilePhase::Syncing, 0U, kSyncStepCount);
  progress.activity("Inspecting staged training dataset");
  const auto& train_info = sealed_splits[0].info;
  progress.phase(DatasetCompilePhase::Syncing, 1U, kSyncStepCount);
  progress.activity("Inspecting staged validation dataset");
  const auto& val_info = sealed_splits[1].info;
  progress.phase(DatasetCompilePhase::Syncing, 2U, kSyncStepCount);
  if (train_info.image_count != train.images.size() || val_info.image_count != expected_validation_images || !std::ranges::equal(train_info.class_names(), train.class_names) ||
      !std::ranges::equal(val_info.class_names(), validation.class_names)) {
   throw std::runtime_error("staged benchmark compiled files do not match their plans");
  }
  progress.activity("Building benchmark manifest");
  nlohmann::json manifest{
   {"supplemental_sampling", {{"revision", kSupplementalSamplingRevision}, {"target_images", combined_sampling.target_images}, {"open_images_floor", combined_sampling.open_images_floor},
                              {"open_images_ceiling", combined_sampling.open_images_ceiling}, {"objects365_shards", combined_sampling.objects365_shards},
                              {"objects365_archive_bytes", combined_sampling.objects365_archive_bytes}}},
   {"projected_output_bytes", total_output_estimate},
   {"train", {{"images", train.images.size()}, {"boxes", train.labels}, {"mask_rle_pairs", train.runs}, {"bytes", std::filesystem::file_size(staging_dir / "train.bin")}}},
   {"val",
    {{"source", "coco"}, {"images", validation.images.size()}, {"boxes", validation.labels}, {"mask_rle_pairs", validation.runs}, {"bytes", std::filesystem::file_size(staging_dir / "val.bin")}}},
   {"sources", nlohmann::json::array()},
   {"image_cache", nlohmann::json::array()},
   {"quarantined_images", nlohmann::json::array()},
  };
  const auto append_source_manifest = [&](const NormalizedAnnotationReadView& source_index, const SourceCompileCount& count, const std::string_view annotation_url,
                                       const std::filesystem::path& normalized_index_path, const SupplementalSamplingStats* sampling) {
   nlohmann::json source_manifest{
    {"name", benchmark_source_name(source_index.source)},
    {"version", benchmark_source_version(source_index.source)},
    {"annotation_url", annotation_url},
    {"annotation_sha256", source_index.annotation_sha256},
    {"normalized_index", normalized_index_path.lexically_relative(cache.root).string()},
    {"normalized_index_identity", source_index.completion->identity},
    {"indexed_images", sampling != nullptr ? sampling->full_images : source_index.image_count()},
    {"indexed_boxes", sampling != nullptr ? sampling->full_boxes : source_index.box_count()},
    {"indexed_mask_rle_pairs", source_index.run_count()},
    {"selected_images", count.selected_images},
    {"selected_boxes", source_index.box_count()},
    {"compiled_images", count.compiled_images},
    {"compiled_boxes", count.compiled_boxes},
    {"rejected_records", reject_json(source_index.rejected)},
   };
   if (sampling != nullptr) {
    source_manifest["sampling"] = {
     {"revision", kSupplementalSamplingRevision},
     {"available_class_images", sampling->available_class_images},
     {"selected_class_images", sampling->selected_class_images},
    };
   }
   manifest["sources"].push_back(std::move(source_manifest));
  };
  {
   manifest["source_catalog"] = source_catalog_manifest(custom_catalog);
   append_source_manifest(*coco_train, source_counts[0], custom_catalog.coco_annotations.url, coco_train_index_path, nullptr);
   append_source_manifest(*objects, source_counts[1], custom_catalog.objects_annotations.url, objects_index_path, &objects_sampling.stats);
   append_source_manifest(*open_images, source_counts[2], custom_catalog.open_images_boxes.url, open_images_index_path, &open_images_sampling.stats);
   manifest["validation_source"] = {
    {"name", "coco"},
    {"version", benchmark_source_version(BenchmarkDatasetSource::kCoco2017)},
    {"annotation_sha256", coco_val->annotation_sha256},
    {"normalized_index", coco_val_index_path.lexically_relative(cache.root).string()},
    {"normalized_index_identity", coco_val->completion->identity},
    {"selected_images", validation_count.selected_images},
    {"compiled_images", validation_count.compiled_images},
    {"compiled_boxes", validation_count.compiled_boxes},
    {"compiled_mask_rle_pairs", validation.runs},
    {"rejected_records", reject_json(coco_val->rejected)},
   };
  }
  std::uint64_t cached_image_bytes = 0U;
  const auto append_image_cache = [&](const std::vector<CachedImageDirectory>& directories) {
   for (const CachedImageDirectory& directory : directories) {
    manifest["image_cache"].push_back(
     {{"source", directory.source}, {"shard", directory.shard}, {"path", directory.path.lexically_relative(cache.root).string()}, {"identity", directory.identity},
      {"selection_sha256", directory.selection_sha256}, {"images", directory.image_count}, {"bytes", directory.image_bytes}, {"cache_hit", directory.cache_hit}});
    cached_image_bytes = common_math::checked_add(cached_image_bytes, directory.image_bytes, "benchmark cached image byte total overflow");
   }
  };
  {}
  append_image_cache(coco_train_images);
  append_image_cache(coco_val_images);
  append_image_cache(object_images);
  append_image_cache(open_image_directories);
  manifest["cached_image_bytes"] = cached_image_bytes;
  manifest["compiled_bytes"] = common_math::checked_add(manifest["train"]["bytes"].get<std::uint64_t>(), manifest["val"]["bytes"].get<std::uint64_t>(), "benchmark compiled byte total overflow");
  for (const QuarantinedImage& image : quarantined) {
   manifest["quarantined_images"].push_back({{"source", benchmark_source_name(image.source)}, {"image_id", image.image_id}, {"reason", image.reason}});
  }
  progress.activity("Writing benchmark manifest");
  publish_benchmark_manifest(config, staging_dir, cache.root, std::move(manifest), cancel_requested, &pipeline.storage());
  progress.phase(DatasetCompilePhase::Syncing, 3U, kSyncStepCount);
  progress.activity("Syncing staged benchmark dataset");
  trace_benchmark_event(trace, "benchmark.directory_sync.started", [&] { return nlohmann::json{{"path", staging_dir.string()}}; });
  sync_directory(staging_dir);
  trace_benchmark_event(trace, "benchmark.directory_sync.completed", [&] { return nlohmann::json{{"path", staging_dir.string()}}; });
  progress.phase(DatasetCompilePhase::Syncing, kSyncStepCount, kSyncStepCount);
  progress.phase(DatasetCompilePhase::Publishing, 0U, 1U);
  progress.activity("Atomically publishing benchmark dataset");
  throw_if_benchmark_cancelled(cancel_requested);
  progress.flush();
  coconut_failures.flush();
  throw_if_benchmark_cancelled(cancel_requested);
  publish_dataset_directory(staging_dir, config.output_dir, config.overwrite, cancel_requested, trace);
  staging_owner->published();
  progress.phase(DatasetCompilePhase::Publishing, 1U, 1U);
  progress.flush();
  trace_benchmark_event(trace, "benchmark.publication.complete", [&] {
   return nlohmann::json{
    {"output", config.output_dir.string()}, {"train_images", train.images.size()}, {"val_images", validation.images.size()}, {"train_mask_rle_pairs", train.runs},
    {"val_mask_rle_pairs", validation.runs}, {"bytes", total_output_estimate}
   };
  });
  return;
 } catch (...) {
  cancellation_state.rethrow_failure();
  throw;
 }
}
void compile_benchmark_dataset(BenchmarkCompilerConfig config) { benchmark_internal::compile_benchmark_recipe(std::move(config), nullptr); }
std::string format_benchmark_source_status(const BenchmarkSourceProgress& progress, const std::string_view default_status) {
 std::string status;
 if (progress.complete && progress.cache_hit) {
  status = "Cache hit";
 } else if (progress.complete) {
  status = "complete";
 } else if (!progress.activity.empty()) {
  status = progress.activity;
 } else {
  status = default_status;
 }
 if (!progress.complete && progress.transfer) {
  const auto& transfer = *progress.transfer;
  status += " · " + std::to_string(transfer.completed_bytes);
  if (transfer.total_bytes != 0U) status += " / " + std::to_string(transfer.total_bytes);
  status += transfer.cache_hit ? " bytes reused" : " bytes";
  if (!transfer.cache_hit && transfer.total_bytes == 0U) status += " (total unknown)";
  if (!transfer.cache_hit && transfer.attempt != 0U) status += " · attempt " + std::to_string(transfer.attempt);
  if (transfer.resumed) status += " · retained " + std::to_string(transfer.retained_bytes) + " bytes";
 }
 if (progress.resumed) { status += " · resumed"; }
 if (progress.retry_count != 0U) { status += " · " + std::to_string(progress.retry_count) + " retries"; }
 return status;
}
}  // namespace mmltk::backend::data
