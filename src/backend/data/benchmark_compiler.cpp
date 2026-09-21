#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <memory>
#include <map>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/staging_directory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/types/utf8.h"
// CLEANUP-IGNORE: This benchmark compiler imports and includes the concrete owners directly used by its implementation.
#include "benchmark_annotations.h"
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include "benchmark_download.h"
#include "benchmark_images.h"
#include "benchmark_image_decoder.h"
#include "benchmark_sampling.h"
#include "benchmark_writer.h"
#include "detail/benchmark_compiler.h"
#include "detail/benchmark_recipe.h"
#include "detail/benchmark_progress.h"
#include "detail/benchmark_storage.h"
#include "detail/open_images_acquisition.h"
#include "mask_rle_utils.h"
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
    return mmltk::common::io::sha256_hex(
        mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
}
[[nodiscard]] bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto parent_iterator = parent.begin();
    auto child_iterator = child.begin();
    for (; parent_iterator != parent.end() && child_iterator != child.end(); ++parent_iterator, ++child_iterator) {
        if (*parent_iterator != *child_iterator) { return false; }
    }
    return parent_iterator == parent.end();
}
[[nodiscard]] std::optional<std::uint64_t> parse_archive_image_id(const std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    std::string_view filename = path.substr(slash == std::string_view::npos ? 0U : slash + 1U);
    const std::size_t dot = filename.find_last_of('.');
    if (dot != std::string_view::npos) { filename = filename.substr(0U, dot); }
    const std::size_t underscore = filename.find_last_of('_');
    const std::string_view digits = filename.substr(underscore == std::string_view::npos ? 0U : underscore + 1U);
    if (digits.empty()) { return std::nullopt; }
    std::uint64_t image_id = 0U;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), image_id);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) { return std::nullopt; }
    return image_id;
}
void acquire_physical_inventory(AdmittedRecipeArchive& admitted, std::vector<CoconutPhysicalImage>& inventory, const BenchmarkCacheLayout& cache,
                                ProgressReporter& progress, ArtifactProgressTotals& totals, StorageReservationPool& storage, std::size_t workers,
                                common_concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
                                std::string_view recovery_reason = {}) {
    const auto& archive = admitted.origin;
    const auto owner = benchmark_source_name(archive.artifact.source);
    auto request = make_download_request(cache, owner, archive.artifact);
    const auto inventory_path = cache.source_indexes(owner) / (archive.artifact.artifact_id + ".inventory.bin");
    auto lease = ArtifactLease::acquire(cache.locks / (archive.artifact.artifact_id + ".inventory.lock"), cancellation);
    const auto diagnose = [&](std::string_view reason) {
        if (std::filesystem::is_regular_file(request.destination)) {
            const auto digest = common_io::sha256_hex(common_io::sha256_file(request.destination, [&] { return cancellation.requested(); }));
            trace_benchmark_event(trace, "benchmark.download.failure_sha256",
                                  [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", digest}, {"reason", reason}}; });
        }
    };
    const auto invalidate = [&](std::string_view reason) {
        diagnose(reason);
        invalidate_download_artifact(request, cancellation, trace);
        remove_cache_path(inventory_path);
        request.redownload = true;
    };
    if (!recovery_reason.empty()) {
        if (admitted.structural_attempts >= 3) {
            diagnose(recovery_reason);
            throw std::runtime_error("physical archive remains unavailable after three admissions: " + request.artifact_id + ": " +
                                     std::string(recovery_reason));
        }
        invalidate(recovery_reason);
    }
    while (admitted.structural_attempts < 3) {
        ++admitted.structural_attempts;
        throw_if_benchmark_cancelled(cancellation);
        progress.phase(DatasetCompilePhase::Downloading);
        {
            const auto reservation =
                storage.reserve(additional_download_bytes(request.destination, archive.artifact.expected_size), "additional physical archive bytes");
            admitted.download =
                download_artifacts({request}, workers, cancellation,
                                   progress.transfer_observer_enabled() ? DownloadProgressSink{[&](const auto& update) { totals.update(update, progress); }}
                                                                        : DownloadProgressSink{},
                                   trace)
                    .front();
        }
        try {
            auto rows = coconut_image_archive_inventory(admitted.download.path, inventory_path, archive.source, archive.shard, admitted.download.identity,
                                                        cancellation);
            std::erase_if(inventory, [&](const auto& row) { return row.source == archive.source && row.shard == archive.shard; });
            inventory.insert(inventory.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
            return;
        } catch (const InsufficientBenchmarkStorage&) { throw; } catch (const std::exception& error) {
            throw_if_benchmark_cancelled(cancellation);
            if (admitted.structural_attempts == 3) {
                diagnose(error.what());
                throw;
            }
            invalidate(error.what());
        }
    }
}
// Carries the ordinary mutable physical owner out of joined extraction/writer work.
// Recovery runs only at the compiler's preparation boundary, never in workers.
class PhysicalArchiveRecovery final : public std::runtime_error {
   public:
    PhysicalArchiveRecovery(AdmittedRecipeArchive& archive, std::string reason) : std::runtime_error(std::move(reason)), archive_(&archive) {}
    [[nodiscard]] AdmittedRecipeArchive& archive() const noexcept { return *archive_; }

   private:
    AdmittedRecipeArchive* archive_;
};
class RequiredImageDecodeError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
[[nodiscard]] CachedImageDirectory acquire_archive_images(const BenchmarkCacheLayout& cache, const BenchmarkDatasetSource source, std::string shard,
                                                          const std::vector<std::uint64_t>& expected_ids, const CatalogArtifact& artifact,
                                                          mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress,
                                                          ArtifactProgressTotals* transfer_progress, StorageReservationPool* storage_reservations,
                                                          const std::uint64_t source_total_images, const std::size_t decompression_workers,
                                                          const std::size_t cache_write_workers, const std::size_t download_connections,
                                                          const BenchmarkTraceSink& trace, const std::optional<ImageDecodeProbe> decode_probe = std::nullopt,
                                                          const bool require_every_image = false, const ArchiveImageIdParser& member_parser = {},
                                                          std::string_view completion_slot = {}, AdmittedRecipeArchive* admitted_archive = nullptr) {
    if (expected_ids.empty()) { throw std::runtime_error("benchmark archive extraction cannot have an empty image selection"); }
    if (require_every_image && !admitted_archive) throw std::logic_error("strict image extraction requires an admitted physical owner");
    const std::string source_name(benchmark_source_name(source));
    const std::string archive_name = source_name + " " + shard;
    const std::filesystem::path image_root = cache.source_images(source_name) / shard;
    progress->source_activity(source, "Waiting for " + archive_name + " extraction lock");
    ArtifactLease extraction_lease = ArtifactLease::acquire(cache.locks / (source_name + "-" + shard + ".images.lock"), cancel_requested);
    const auto completion = require_every_image
                                ? image_root / ".recipe-proofs" / (std::string(completion_slot) + "-" + cached_image_selection_digest(expected_ids) + ".json")
                                : image_root / ".complete.json";
    if (decode_probe) {
        if (!std::ranges::binary_search(expected_ids, decode_probe->image_id)) {
            throw std::runtime_error("benchmark archive decode probe ID is not selected");
        }
        progress->source_activity(source, "Invalidating failed " + archive_name + " JPEG under the extraction lock");
        invalidate_cached_image_proofs(image_root);
        remove_cache_path(cached_image_path(image_root, decode_probe->image_id));
    }
    const std::string source_identity = source_name + ":" + shard + ":" + artifact.artifact_id + ":" + std::string(kBenchmarkCatalogRevision);
    progress->source_activity(source, "Checking " + archive_name + " cache completion status");
    const bool quarantine_unavailable = !require_every_image && archive_selection_allows_quarantine(source, shard);
    std::uint64_t cached_image_bytes = 0U;
    std::vector<CachedImageRejection> cached_quarantine;
    if (validate_cached_image_group(image_root, completion, source_identity, expected_ids, &cached_image_bytes, cancel_requested, trace,
                                    quarantine_unavailable ? &cached_quarantine : nullptr)) {
        std::vector<std::uint64_t> available_ids;
        available_ids.reserve(expected_ids.size() - cached_quarantine.size());
        for (const std::uint64_t image_id : expected_ids) {
            if (!std::ranges::binary_search(cached_quarantine, image_id, {}, &CachedImageRejection::image_id)) { available_ids.push_back(image_id); }
        }
        progress->add_source_images(source, expected_ids.size(), source_total_images, "Reusing cached " + archive_name + " images");
        return CachedImageDirectory{source_name,
                                    std::move(shard),
                                    image_root,
                                    source_identity,
                                    cached_image_selection_digest(available_ids),
                                    available_ids.size(),
                                    cached_image_bytes,
                                    true,
                                    std::move(cached_quarantine)};
    }
    DownloadRequest request = make_download_request(cache, source_name, artifact);
    std::uint64_t pending_images = expected_ids.size();
    if (require_every_image) {
        pending_images = 0;
        for (const auto id : expected_ids) {
            throw_if_benchmark_cancelled(cancel_requested);
            const auto path = cached_image_path(image_root, id);
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) == 0) ++pending_images;
        }
    }
    const auto image_storage = common_math::checked_multiply(pending_images, kEstimatedJpegBytes, "benchmark image cache estimate overflow");
    const auto archive_storage = admitted_archive ? 0U : artifact.expected_size != 0U ? artifact.expected_size : kArchiveScratchBytes;
    StorageReservationPool::Reservation storage_reservation =
        storage_reservations->reserve(common_math::checked_add(image_storage, archive_storage, "archive acquisition estimate overflow"),
                                      source_name + " pending archive and extracted images");
    std::optional<DownloadResult> retained;
    std::exception_ptr last_error;
    const std::uint32_t extraction_attempts = admitted_archive ? 1U : 3U;
    for (std::uint32_t attempt = 1U; attempt <= extraction_attempts; ++attempt) {
        throw_if_benchmark_cancelled(cancel_requested);
        std::uint64_t attempt_reported = 0U;
        try {
            if (!admitted_archive && !retained) {
                progress->source_activity(
                    source, "Downloading " + archive_name + " image archive with " + std::to_string(download_connections) + " download connections");
                retained = download_artifacts({request}, download_connections, cancel_requested,
                                              progress->transfer_observer_enabled()
                                                  ? DownloadProgressSink{[&](const DownloadProgress& update) { transfer_progress->update(update, *progress); }}
                                                  : DownloadProgressSink{},
                                              trace)
                               .front();
            }
            progress->source_activity(source, "Opening " + archive_name + " image archive");
            BenchmarkImageValidator image_validator;
            CachedImageDirectory extracted = extract_selected_archive_images(ArchiveExtractionRequest{
                .archive_path = admitted_archive ? admitted_archive->download.path : retained->path,
                .source_identity = source_identity,
                .output_root = image_root,
                .source = source_name,
                .shard = shard,
                .selected_image_ids = expected_ids,
                .image_id_parser = member_parser ? member_parser : ArchiveImageIdParser{parse_archive_image_id},
                .cancel_requested = cancel_requested,
                .progress =
                    [&](const std::uint64_t completed, const std::uint64_t) {
                        if (completed < attempt_reported) { throw std::logic_error("archive attempt image progress regressed"); }
                        const std::uint64_t delta = completed - attempt_reported;
                        attempt_reported = completed;
                        if (progress->transfer_observer_enabled()) {
                            progress->add_source_images(source, delta, source_total_images, "Resolving " + archive_name + " selected images");
                        }
                        trace_benchmark_event(trace, "benchmark.images.progress", [&] {
                            return nlohmann::json{{"source", source_name},
                                                  {"shard", shard},
                                                  {"attempt", attempt},
                                                  {"resolved_images", completed},
                                                  {"total_images", expected_ids.size()}};
                        });
                    },
                .validator =
                    [&](const std::uint64_t image_id, const std::span<const std::uint8_t> encoded) {
                        if (!has_complete_image_markers(encoded)) {
                            if (decode_probe && decode_probe->image_id == image_id && !quarantine_unavailable) {
                                throw RequiredImageDecodeError("required archive image " + std::to_string(image_id) +
                                                               " remains incomplete after bounded repair");
                            }
                            throw std::runtime_error("selected archive image " + std::to_string(image_id) + " is not a complete JPEG or PNG");
                        }
                        try {
                            if (decode_probe && decode_probe->image_id == image_id) {
                                image_validator.validate_decodable(encoded, decode_probe->expected_width, decode_probe->expected_height);
                            } else {
                                (void)image_validator.read_header(encoded);
                            }
                        } catch (const InvalidImageError& error) {
                            if (decode_probe && decode_probe->image_id == image_id && !quarantine_unavailable) {
                                throw RequiredImageDecodeError("required archive image " + std::to_string(image_id) +
                                                               " remains undecodable after bounded repair: " + error.what());
                            }
                            throw InvalidImageError("selected archive image " + std::to_string(image_id) + ": " + error.what());
                        }
                    },
                .trace = trace,
                .quarantine_unavailable = quarantine_unavailable,
                .decompression_workers = decompression_workers,
                .cache_write_workers = cache_write_workers,
                .activity = [&](const std::string_view activity) { progress->source_activity(source, archive_name + ": " + std::string(activity)); },
                .completion_path = completion,
            });
            progress->source_activity(source, "Retaining completed " + archive_name + " archive in cache");
            extracted.cache_hit = false;
            return extracted;
        } catch (const RequiredImageDecodeError& error) {
            if (!admitted_archive) throw;
            throw_if_benchmark_cancelled(cancel_requested);
            if (attempt_reported != 0U)
                progress->rollback_source_images(source, attempt_reported, source_total_images, "Withdrawing failed " + archive_name + " attempt");
            throw PhysicalArchiveRecovery(*admitted_archive, error.what());
        } catch (const InsufficientBenchmarkStorage&) { throw; } catch (...) {
            last_error = std::current_exception();
            std::string failure_reason = "non-standard exception";
            try {
                std::rethrow_exception(last_error);
            } catch (const std::exception& error) { failure_reason = error.what(); } catch (...) {
                failure_reason = "non-standard exception";
            }
            if (attempt_reported != 0U) {
                progress->rollback_source_images(source, attempt_reported, source_total_images, "Withdrawing failed " + archive_name + " attempt");
            }
            throw_if_benchmark_cancelled(cancel_requested);
            if (admitted_archive) throw PhysicalArchiveRecovery(*admitted_archive, failure_reason);
            if (std::filesystem::is_regular_file(request.destination)) {
                progress->source_activity(source, "Failure-only SHA-256 diagnosis for " + archive_name);
                const std::string failure_sha256 =
                    mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(request.destination, [&] { return cancel_requested.requested(); }));
                trace_benchmark_event(trace, "benchmark.download.failure_sha256", [&] {
                    return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", failure_sha256}, {"reason", failure_reason}};
                });
            }
            if (attempt == 3U) { break; }
            progress->source_activity(source, "Repairing " + archive_name + " extraction for retry " + std::to_string(attempt + 1U));
            invalidate_download_artifact(request, cancel_requested, trace);
            retained.reset();
            request.redownload = true;
            // The root is shared by selections. Archive corruption invalidates
            // proof metadata, not otherwise valid JPEGs retained in this root.
            invalidate_cached_image_proofs(image_root);
            trace_benchmark_event(trace, "benchmark.images.archive_retry",
                                  [&] { return nlohmann::json{{"source", source_name}, {"shard", shard}, {"attempt", attempt}, {"reason", failure_reason}}; });
        }
    }
    std::rethrow_exception(last_error);
}
struct SourceCompileCount {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::uint64_t selected_images = 0U;
    std::uint64_t compiled_images = 0U;
    std::uint64_t compiled_boxes = 0U;
    std::uint64_t dropped_boxes = 0U;
};
struct PlannedBenchmarkInstance {
    PackedInstance label{};
    std::vector<RLEPair> mask_rle;
};
SourceCompileCount append_source_plan(const NormalizedAnnotationIndex& index, const std::vector<CachedImageDirectory>& directories,
                                      const std::vector<std::uint64_t>* unavailable_image_ids, const std::uint32_t resolution,
                                      const mmltk::backend::imaging::resample::ImageResizeMode resize_mode, const bool require_every_image,
                                      PreparedBenchmarkSplit* split, mmltk::common::concurrency::CancellationObservation cancel_requested,
                                      std::optional<AnnotationSource> provenance = {}, std::span<const std::uint16_t> image_sources = {},
                                      std::span<const std::pair<std::uint32_t, std::uint32_t>> source_dimensions = {}) {
    if (directories.empty()) { throw std::runtime_error("benchmark source has no cached image directories"); }
    if (!image_sources.empty() && image_sources.size() != index.images.size()) throw std::runtime_error("benchmark component source membership is misaligned");
    if (!source_dimensions.empty() && source_dimensions.size() != index.images.size())
        throw std::runtime_error("benchmark component image geometry is misaligned");
    if (unavailable_image_ids != nullptr &&
        (!std::ranges::is_sorted(*unavailable_image_ids) || std::ranges::adjacent_find(*unavailable_image_ids) != unavailable_image_ids->end())) {
        throw std::runtime_error("benchmark unavailable image IDs must be sorted and unique");
    }
    const std::size_t source_base = split->sources.size();
    for (const CachedImageDirectory& directory : directories) {
        if (split->sources.size() >= std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("benchmark split exceeds its cached source representation");
        }
        split->sources.push_back(CachedImageSource{directory.path});
    }
    std::array<std::size_t, 51> objects_shards{};
    objects_shards.fill(std::numeric_limits<std::size_t>::max());
    if (image_sources.empty() && index.source == BenchmarkDatasetSource::kObjects365V2) {
        for (std::size_t directory_index = 0U; directory_index < directories.size(); ++directory_index) {
            constexpr std::string_view prefix = "patch-";
            if (!std::string_view(directories[directory_index].shard).starts_with(prefix)) {
                throw std::runtime_error("Objects365 cache has an invalid shard identity");
            }
            std::uint32_t shard = 0U;
            const std::string_view number = std::string_view(directories[directory_index].shard).substr(prefix.size());
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), shard);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || shard >= objects_shards.size() ||
                objects_shards[shard] != std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("Objects365 cache shard map is invalid");
            }
            objects_shards[shard] = directory_index;
        }
    }
    SourceCompileCount counts{index.source, index.images.size(), 0U, 0U};
    std::vector<PlannedBenchmarkInstance> image_labels;
    dataset::MaskResizeScratch mask_resize_scratch;
    std::size_t unavailable_index = 0U;
    for (std::size_t image_index = 0U; image_index < index.images.size(); ++image_index) {
        if ((image_index & 4095U) == 0U) { throw_if_benchmark_cancelled(cancel_requested); }
        const NormalizedImage& image = index.images[image_index];
        while (unavailable_image_ids != nullptr && unavailable_index < unavailable_image_ids->size() &&
               (*unavailable_image_ids)[unavailable_index] < image.source_image_id) {
            ++unavailable_index;
        }
        if (unavailable_image_ids != nullptr && unavailable_index < unavailable_image_ids->size() &&
            (*unavailable_image_ids)[unavailable_index] == image.source_image_id) {
            if (require_every_image) { throw std::runtime_error("required benchmark image is unavailable"); }
            continue;
        }
        std::size_t local_source = image_sources.empty() ? 0U : image_sources[image_index];
        if (image_sources.empty() && index.source == BenchmarkDatasetSource::kObjects365V2) {
            local_source = objects_shards[image.source_shard];
            if (local_source == std::numeric_limits<std::size_t>::max()) { throw std::runtime_error("Objects365 image references an unacquired shard"); }
        }
        if (local_source >= directories.size()) throw std::runtime_error("benchmark component references an unacquired directory");
        image_labels.clear();
        image_labels.reserve(image.box_count);
        const auto [source_width, source_height] = source_dimensions.empty() ? std::pair{image.width, image.height} : source_dimensions[image_index];
        const bool matching_geometry = source_width == image.width && source_height == image.height;
        const auto box_count = matching_geometry ? image.box_count : 0U;
        counts.dropped_boxes += image.box_count - box_count;
        const mmltk::backend::imaging::resample::ImageResizeGeometry letterbox =
            mmltk::backend::imaging::resample::compute_image_resize_geometry(source_width, source_height, resolution, resolution, resize_mode);
        for (std::uint64_t box_index = image.first_box; box_index < image.first_box + box_count; ++box_index) {
            const NormalizedBox& box = index.boxes[common_math::checked_cast<std::size_t>(box_index, "box index overflow")];
            PackedInstance label = benchmark_canvas_box(box.class_id, box.x1, box.y1, box.x2, box.y2, letterbox);
            label.flags = box.flags;
            label.annotation_id = box.annotation_id;
            label.source_category_id = box.source_category_id;
            label.source_ordinal = box.source_ordinal;
            label.original_area = box.original_area;
            if (index.source == BenchmarkDatasetSource::kOpenImagesV7) label.original_area *= static_cast<double>(image.width) * image.height;
            if (label.bbox_x2 <= label.bbox_x1 || label.bbox_y2 <= label.bbox_y1) {
                throw std::runtime_error("benchmark continuous box is not representable on the compiled canvas");
            }
            std::vector<RLEPair> mask_rle;
            if (box.mask_rle_pairs != 0U) {
                if (box.mask_rle_offset > index.mask_rle_pairs.size() || box.mask_rle_pairs > index.mask_rle_pairs.size() - box.mask_rle_offset) {
                    throw std::runtime_error("benchmark source mask range is invalid");
                }
                const auto source_mask = std::span(index.mask_rle_pairs).subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs);
                mask_rle = dataset::resize_row_major_mask(source_mask, dataset::MaskDimensions{image.width, image.height},
                                                          dataset::MaskDimensions{resolution, resolution}, letterbox, &mask_resize_scratch)
                               .pairs;
            }
            image_labels.push_back(PlannedBenchmarkInstance{label, std::move(mask_rle)});
        }
        if (image_labels.empty() && !require_every_image) { continue; }
        const std::uint32_t first_label = common_math::checked_cast<std::uint32_t>(split->labels.size(), "benchmark label index overflow");
        split->images.push_back(EncodedImageRecord{
            image.source_image_id,
            source_width,
            source_height,
            first_label,
            common_math::checked_cast<std::uint16_t>(image_labels.size(), "per-image label count overflow"),
            common_math::checked_cast<std::uint16_t>(source_base + local_source, "benchmark cached source index overflow"),
            provenance.value_or(index.source == BenchmarkDatasetSource::kCoco2017       ? AnnotationSource::Coco
                                : index.source == BenchmarkDatasetSource::kObjects365V2 ? AnnotationSource::Objects365
                                                                                        : AnnotationSource::OpenImages),
        });
        for (PlannedBenchmarkInstance& instance : image_labels) {
            instance.label.mask_rle_offset = common_math::checked_multiply<decltype(PackedInstance::mask_rle_offset)>(
                split->rle_pairs.size(), sizeof(RLEPair), "benchmark mask offset overflow");
            instance.label.mask_rle_pairs = common_math::checked_cast<std::uint16_t>(instance.mask_rle.size(), "benchmark instance mask run count overflow");
            split->labels.push_back(instance.label);
            split->rle_pairs.insert(split->rle_pairs.end(), instance.mask_rle.begin(), instance.mask_rle.end());
        }
        counts.compiled_images = common_math::checked_add(counts.compiled_images, 1U, "benchmark compiled image count overflow");
        counts.compiled_boxes = common_math::checked_add(counts.compiled_boxes, image_labels.size(), "benchmark compiled box count overflow");
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
[[nodiscard]] std::uint64_t estimate_output_bytes(const std::uint64_t images, const std::uint64_t boxes, const std::uint32_t resolution,
                                                  const std::uint64_t mask_rle_pairs = 0U) {
    const std::uint64_t pixels = common_math::checked_multiply(
        images,
        common_math::checked_multiply(common_math::checked_multiply<std::uint64_t>(resolution, resolution, "benchmark output resolution overflow"),
                                      3U * sizeof(float), "benchmark image stride overflow"),
        "benchmark output pixel bytes overflow");
    const std::uint64_t labels = common_math::checked_multiply(boxes, sizeof(PackedInstance), "benchmark output label bytes overflow");
    const std::uint64_t masks = common_math::checked_multiply(mask_rle_pairs, sizeof(RLEPair), "benchmark output mask bytes overflow");
    const std::uint64_t index = common_math::checked_multiply(images, sizeof(ImageEntry), "benchmark output index bytes overflow");
    return common_math::checked_add(
        common_math::checked_add(common_math::checked_add(pixels, labels, "benchmark output size overflow"), masks, "benchmark output size overflow"),
        common_math::checked_add(index, 2U * HUGE_PAGE_SIZE, "benchmark output metadata overflow"), "benchmark output size overflow");
}
void sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) { throw common_io::errno_error("cannot open benchmark directory for sync", path.string()); }
    common_io::ScopedFd directory(descriptor);
    if (::fsync(directory.get()) != 0) { throw common_io::errno_error("cannot sync benchmark directory", path.string()); }
}
void publish_dataset_directory(const std::filesystem::path& staging, const std::filesystem::path& output, const bool overwrite,
                               const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace) {
    throw_if_benchmark_cancelled(cancellation);
    const bool output_exists = std::filesystem::exists(output);
    if (!output_exists) {
        throw_if_benchmark_cancelled(cancellation);
        std::filesystem::rename(staging, output);
        common_io::sync_parent_directory(output);
        return;
    }
    if (!overwrite) {
        if (!std::filesystem::is_empty(output)) { throw std::runtime_error("benchmark output directory is no longer empty"); }
        std::filesystem::remove(output);
        throw_if_benchmark_cancelled(cancellation);
        std::filesystem::rename(staging, output);
        common_io::sync_parent_directory(output);
        return;
    }
    throw_if_benchmark_cancelled(cancellation);
    const long result = ::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, output.c_str(), RENAME_EXCHANGE);
    if (result != 0) { throw common_io::errno_error("cannot atomically exchange benchmark dataset directory", output.string()); }
    common_io::sync_parent_directory(output);
    std::error_code cleanup_error;
    const bool removed = common_io::remove_tree_no_follow(staging, cleanup_error);
    trace_benchmark_event(trace, "benchmark.publication.old_generation_cleanup", [&] {
        return nlohmann::json{{"path", staging.string()}, {"removed", removed}, {"error", cleanup_error ? cleanup_error.message() : std::string{}}};
    });
}
void write_split_with_progress(const BenchmarkWriteRequest& request, ProgressReporter* progress, const std::uint64_t completed_before,
                               const std::uint64_t total_images, const BenchmarkTraceSink& trace) {
    progress->pixel_attempt(completed_before, total_images, request.split.name, request.split.images.size());
    progress->activity("Decoding and resizing " + request.split.name + " JPEG pixels");
    const Clock::time_point started = trace ? Clock::now() : Clock::time_point{};
    BenchmarkWriteRequest observed = request;
    if (progress->pixel_observer_enabled()) {
        observed.progress = {.context = progress, .image_completed = [](void* context) { static_cast<ProgressReporter*>(context)->pixel_completed(); }};
    }
    write_benchmark_split(observed);
    trace_benchmark_event(trace, "benchmark.pixel_compile.complete", [&] {
        const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
        return nlohmann::json{{"split", request.split.name},
                              {"completed_images", request.split.images.size()},
                              {"elapsed_seconds", elapsed_seconds},
                              {"images_per_second", elapsed_seconds > 0.0 ? static_cast<double>(request.split.images.size()) / elapsed_seconds : 0.0},
                              {"eta_seconds", 0.0}};
    });
}
[[nodiscard]] nlohmann::json mapping_manifest() {
    nlohmann::json mappings;
    mappings["coco"] = nlohmann::json::array();
    for (const NumericCategoryMapping& mapping : coco_category_mappings()) {
        mappings["coco"].push_back({{"source_id", mapping.source_id},
                                    {"source_name", mapping.expected_name},
                                    {"target_id", mapping.target_id},
                                    {"target_name", coco80_class_names()[mapping.target_id]}});
    }
    mappings["objects365"] = nlohmann::json::array();
    for (const NumericCategoryMapping& mapping : objects365_category_mappings()) {
        mappings["objects365"].push_back({{"source_id", mapping.source_id},
                                          {"source_name", mapping.expected_name},
                                          {"target_id", mapping.target_id},
                                          {"target_name", coco80_class_names()[mapping.target_id]}});
    }
    mappings["open_images"] = nlohmann::json::array();
    for (const StringCategoryMapping& mapping : open_images_category_mappings()) {
        mappings["open_images"].push_back({{"source_id", mapping.source_id},
                                           {"source_name", mapping.expected_name},
                                           {"target_id", mapping.target_id},
                                           {"target_name", coco80_class_names()[mapping.target_id]}});
    }
    return mappings;
}
[[nodiscard]] nlohmann::json source_catalog_manifest(const CustomRecipeCatalog& catalog) {
    nlohmann::json artifacts = nlohmann::json::array();
    const auto append = [&](const CatalogArtifact& artifact) {
        nlohmann::json record{
            {"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename}, {"expected_size", artifact.expected_size}};
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
bool archive_selection_allows_quarantine(const BenchmarkDatasetSource source, const std::string_view split) noexcept {
    return source != BenchmarkDatasetSource::kCoco2017 || split == "train2017";
}
void publish_benchmark_manifest(const BenchmarkCompilerConfig& config, const std::filesystem::path& staging_dir, const std::filesystem::path& cache_root,
                                nlohmann::json facts, const common_concurrency::CancellationObservation cancelled) {
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
    write_json_atomically(staging_dir / "benchmark_manifest.json", facts, cancelled);
}
}  // namespace benchmark_internal
void benchmark_internal::compile_benchmark_recipe(BenchmarkCompilerConfig config, const CoconutRecipeCatalog* explicit_catalog,
                                                  const CustomRecipeCatalog* explicit_custom_catalog) {
    if (!valid_benchmark_selection(config.selection)) throw std::invalid_argument("invalid benchmark dataset selection");
    using namespace benchmark_internal;
    if (config.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Stretch &&
        config.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Letterbox)
        throw std::invalid_argument("invalid benchmark resize mode");
    if (config.resolution == 0U || config.resolution > MAX_IMAGE_EXTENT) {
        throw std::runtime_error("benchmark resolution exceeds the compiled coordinate format");
    }
    if (config.output_dir.empty()) { throw std::runtime_error("benchmark output directory must not be empty"); }
    struct CancellationState final {
        mmltk::common::concurrency::CancellationObservation external;
        std::atomic<bool> internal{false};
        [[nodiscard]] bool cancelled() const noexcept { return external.requested() || internal.load(std::memory_order_relaxed); }
    } cancellation_state{.external = config.cancel_requested};
    const auto cancel_requested = mmltk::common::concurrency::CancellationObservation::Borrow(cancellation_state);
    std::atomic<bool>* const cancel_signal = &cancellation_state.internal;
    const BenchmarkTraceSink trace = make_trace_sink(config.trace);
    const bool coconut = config.selection.dataset == BenchmarkDatasetVariant::Coconut;
    const std::string completion_slot = "coconut-" + std::to_string(static_cast<unsigned>(config.selection.validation));
    std::vector<BenchmarkDatasetSource> participating;
    if (coconut) {
        participating = {BenchmarkDatasetSource::kCoco2017, BenchmarkDatasetSource::kObjects365V2, BenchmarkDatasetSource::kCoconut};
        if (config.selection.validation == CoconutValidation::Coconut) participating.push_back(BenchmarkDatasetSource::kObjects365V1);
    }
    ProgressReporter progress(std::move(config.progress), trace, participating);
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
        config.publication_dir.empty() ? normalized_output
                                       : std::filesystem::weakly_canonical(std::filesystem::absolute(config.publication_dir).lexically_normal());
    trace_benchmark_event(trace, "benchmark.compile.paths", [&] {
        const TracePath cache_path = project_trace_path(normalized_cache_root.native());
        const TracePath output_path = project_trace_path(normalized_output.native());
        return nlohmann::json{{"cache_root", cache_path.text},
                              {"output_root", output_path.text},
                              {"cache_root_truncated", cache_path.truncated},
                              {"output_root_truncated", output_path.truncated},
                              {"cache_root_utf8_replaced", cache_path.utf8_replaced},
                              {"output_root_utf8_replaced", output_path.utf8_replaced}};
    });
    if (path_contains(normalized_cache_root, normalized_output) || path_contains(normalized_output, normalized_cache_root) ||
        path_contains(normalized_cache_root, normalized_publication) || path_contains(normalized_publication, normalized_cache_root)) {
        throw std::runtime_error("benchmark output and cache directories must not overlap");
    }
    progress.activity("Preparing benchmark output directory");
    std::filesystem::create_directories(output_parent);
    progress.activity("Preparing benchmark cache");
    const BenchmarkCacheLayout cache = BenchmarkCacheLayout::create(normalized_cache_root);
    CoconutFailureReport coconut_failures(cache.root, progress);
    progress.activity("Waiting for benchmark output lock");
    ArtifactLease output_lease = ArtifactLease::acquire(
        normalized_output.parent_path() / ".cache" / "benchmark-dataset" / "v1" / "locks" / ("output-" + output_lock_identity(normalized_output) + ".lock"),
        cancel_requested);
    progress.activity("Validating benchmark output destination");
    if (std::filesystem::exists(config.output_dir)) {
        const std::filesystem::file_status status = std::filesystem::symlink_status(config.output_dir);
        if (!std::filesystem::is_directory(status)) { throw std::runtime_error("benchmark output path exists and is not a directory"); }
        if (!config.overwrite && !std::filesystem::is_empty(config.output_dir)) {
            throw std::runtime_error("benchmark output directory is not empty; use --overwrite");
        }
    }
    config.num_workers = config.num_workers > 0 ? config.num_workers : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    const std::size_t effective_num_workers = std::min<std::size_t>(
        common_math::checked_cast<std::size_t>(config.num_workers, "benchmark effective worker count overflow"), common_system::allowed_cpu_set().size());
    const int effective_num_workers_int = common_math::checked_cast<int>(effective_num_workers, "benchmark effective worker count overflow");
    const auto custom_catalog = explicit_custom_catalog ? *explicit_custom_catalog : coconut ? CustomRecipeCatalog{} : custom_recipe_catalog();
    const auto coconut_catalog =
        coconut ? (explicit_catalog ? *explicit_catalog : coconut_recipe_catalog(config.selection.validation)) : CoconutRecipeCatalog{};
    std::vector<AdmittedRecipeArchive> admitted;
    std::vector<CoconutPhysicalImage> inventory;
    ArtifactProgressTotals physical_progress;
    StorageReservationPool physical_storage(cache.root, trace);
    if (coconut) {
        admitted.reserve(coconut_catalog.images.size());
        for (const auto& archive : coconut_catalog.images) {
            admitted.push_back({archive, {}});
            acquire_physical_inventory(admitted.back(), inventory, cache, progress, physical_progress, physical_storage, effective_num_workers,
                                       cancel_requested, trace);
        }
    }
    std::optional<CoconutPhysicalMembership> membership;
    if (coconut) membership.emplace(inventory, cancel_requested);
    std::vector<CoconutImageNamespace> refreshed_sources;
    // Keep the output lease, resource budgets and physical admissions alive across
    // preparation attempts. Failed staged files and dependent plans unwind before
    // the single physical owner advances; successful unrelated caches remain valid.
    for (;;) {
        try {
            CustomRecipePreparation custom{};
            std::optional<CoconutRecipePreparation> enhanced;
            if (coconut) {
                enhanced = prepare_coconut_recipe(config, cache, coconut_catalog, admitted, *membership, progress, coconut_failures, effective_num_workers,
                                                  cancel_requested, trace, refreshed_sources);
                refreshed_sources.clear();
            } else
                custom = prepare_custom_recipe(config, cache, custom_catalog, progress, effective_num_workers, cancel_requested, trace);
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
            const auto count_index = [&](const NormalizedAnnotationIndex& index, bool validation) {
                auto& images = validation ? selected_val_images : selected_train_images;
                auto& labels = validation ? selected_val_labels : selected_train_labels;
                auto& masks = validation ? val_masks : train_masks;
                images = common_math::checked_add(images, index.images.size(), "benchmark selected image count overflow");
                labels = common_math::checked_add(labels, index.boxes.size(), "benchmark selected label count overflow");
                masks = common_math::checked_add(masks, index.mask_rle_pairs.size(), "benchmark selected mask count overflow");
            };
            std::vector<std::uint64_t> archive_sizes;
            if (enhanced) {
                for (const auto& component : enhanced->components) count_index(component.index, coconut_validation_component(component.edition));
                if (enhanced->stock_validation) count_index(*enhanced->stock_validation, true);
                for (const auto& archive : admitted) archive_sizes.push_back(archive.origin.artifact.expected_size);
            } else {
                count_index(*coco_train, false);
                count_index(*objects, false);
                count_index(*open_images, false);
                count_index(*coco_val, true);
                archive_sizes = {custom_catalog.coco_train_images.expected_size, custom_catalog.coco_val_images.expected_size};
                for (const auto shard : combined_sampling.objects365_shards) archive_sizes.push_back(sampling_object_artifacts.at(shard).expected_size);
            }
            const auto selected_total_images = common_math::checked_add(selected_train_images, selected_val_images, "benchmark image count overflow");
            const auto projected_output_upper_bound = common_math::checked_add(
                estimate_output_bytes(selected_train_images, selected_train_labels, config.resolution, train_masks),
                estimate_output_bytes(selected_val_images, selected_val_labels, config.resolution, val_masks), "benchmark projected output overflow");
            std::uint64_t retained_archive_bytes = 0U;
            for (const std::uint64_t archive_size : archive_sizes) {
                retained_archive_bytes = common_math::checked_add(retained_archive_bytes, archive_size, "benchmark archive cache estimate overflow");
            }
            const std::uint64_t projected_cache =
                common_math::checked_add(common_math::checked_multiply(selected_total_images, kEstimatedJpegBytes, "benchmark cache estimate overflow"),
                                         common_math::checked_add(retained_archive_bytes, enhanced ? enhanced->annotation_storage_bytes : 0U,
                                                                  "benchmark annotation cache estimate overflow"),
                                         "benchmark cache estimate overflow");
            trace_benchmark_event(trace, "benchmark.storage.projection", [&] {
                return nlohmann::json{{"cache_bytes_upper_bound", projected_cache},
                                      {"compiled_output_bytes_upper_bound", projected_output_upper_bound},
                                      {"selected_images", selected_total_images}};
            });
            progress.projected(projected_output_upper_bound);
            progress.activity("Checking projected benchmark storage");
            if (!enhanced) require_storage(cache.root, projected_cache, "benchmark image cache and active archives", trace);
            require_storage(output_parent, projected_output_upper_bound, "benchmark compiled output staging upper bound", trace);
            progress.phase(DatasetCompilePhase::Extracting);
            ArtifactProgressTotals image_transfer_progress;
            StorageReservationPool cache_storage_reservations(cache.root, trace);
            std::vector<QuarantinedImage> quarantined;
            const std::vector<std::uint64_t> coco_train_ids = coconut ? std::vector<std::uint64_t>{} : image_ids(*coco_train);
            const std::vector<std::uint64_t> coco_val_ids = coconut ? std::vector<std::uint64_t>{} : image_ids(*coco_val);
            const std::uint64_t coco_selected_images =
                common_math::checked_add(coco_train_ids.size(), coco_val_ids.size(), "COCO selected image count overflow");
            const std::vector<CatalogArtifact> object_artifacts = custom_catalog.objects_images;
            struct ArchiveTask {
                BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
                std::string shard;
                std::vector<std::uint64_t> image_ids;
                CatalogArtifact artifact;
                std::unordered_map<std::string, std::uint64_t> members{};
                CoconutImageNamespace image_namespace = CoconutImageNamespace::CocoTrain;
                std::uint16_t numeric_shard = 0;
                AdmittedRecipeArchive* admitted = nullptr;
            };
            const auto archive_member_parser = [](const ArchiveTask& task) -> ArchiveImageIdParser {
                if (task.members.empty()) return {};
                return [&task](std::string_view raw) -> std::optional<std::uint64_t> {
                    // Extraction visits directory headers too; inventory admits these roots.
                    if (raw == "." || raw == "./") return std::nullopt;
                    const auto member = canonical_coconut_archive_member(raw);
                    const auto found = task.members.find(member);
                    return found == task.members.end() ? std::nullopt : std::optional(found->second);
                };
            };
            std::vector<ArchiveTask> archive_tasks;
            if (enhanced) {
                std::map<std::pair<CoconutImageNamespace, std::uint16_t>, std::size_t> task_slots;
                for (auto& acquired : admitted) {
                    const auto& archive = acquired.origin;
                    if (!task_slots.emplace(std::pair{archive.source, archive.shard}, archive_tasks.size()).second)
                        throw std::runtime_error("COCONut physical archive catalog contains a duplicate source/shard");
                    archive_tasks.push_back({archive.artifact.source, archive.cache_shard, {}, archive.artifact, {}, archive.source, archive.shard, &acquired});
                }
                for (const auto& component : enhanced->components)
                    for (const auto& row : component.inventory) {
                        throw_if_benchmark_cancelled(cancel_requested);
                        auto& task = archive_tasks.at(task_slots.at({row.physical.source, row.physical.shard}));
                        task.image_ids.push_back(row.physical.image_id);
                        if (!task.members.emplace(row.physical.member, row.physical.image_id).second)
                            throw std::runtime_error("COCONut recipe repeats a physical archive member");
                    }
                if (enhanced->stock_validation) {
                    auto& task = archive_tasks.at(task_slots.at({CoconutImageNamespace::CocoValidation, 0}));
                    task.image_ids = image_ids(*enhanced->stock_validation);
                }
                for (auto& task : archive_tasks) {
                    std::ranges::sort(task.image_ids);
                    if (std::ranges::adjacent_find(task.image_ids) != task.image_ids.end())
                        throw std::runtime_error("COCONut recipe repeats a physical image ID");
                }
                std::erase_if(archive_tasks, [](const auto& task) { return task.image_ids.empty(); });
            } else {
                archive_tasks.reserve(object_artifacts.size() + 2U);
                archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "train2017", coco_train_ids, custom_catalog.coco_train_images});
                archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "val2017", coco_val_ids, custom_catalog.coco_val_images});
                for (std::size_t shard = 0U; shard < object_artifacts.size(); ++shard) {
                    const std::uint16_t numeric_shard = common_math::checked_cast<std::uint16_t>(shard, "Objects365 shard index overflow");
                    std::vector<std::uint64_t> ids = image_ids(*objects, numeric_shard);
                    if (!ids.empty()) {
                        archive_tasks.push_back(
                            ArchiveTask{BenchmarkDatasetSource::kObjects365V2, "patch-" + std::to_string(shard), std::move(ids), object_artifacts[shard]});
                    }
                }
            }
            if (archive_tasks.empty()) throw std::runtime_error("benchmark recipe has no physical image membership");
            std::map<BenchmarkDatasetSource, std::uint64_t> source_totals;
            for (const auto& task : archive_tasks)
                source_totals[task.source] = common_math::checked_add(source_totals[task.source], task.image_ids.size(), "benchmark source total overflow");
            std::vector<std::optional<CachedImageDirectory>> archive_results(archive_tasks.size());
            std::optional<AcquiredOpenImages> open_image_result;
            std::exception_ptr pipeline_error;
            std::mutex pipeline_error_mutex;
            const auto record_pipeline_error = [&] {
                const std::lock_guard lock(pipeline_error_mutex);
                if (!pipeline_error) {
                    pipeline_error = std::current_exception();
                    cancel_signal->store(true, std::memory_order_relaxed);
                }
            };
            const std::size_t configured_workers = effective_num_workers;
            const int acquisition_num_workers = effective_num_workers_int;
            const bool overlap_open_images = !coconut && configured_workers >= 5U;
            const std::size_t open_cache_workers =
                configured_workers == 1U ? 0U : std::min<std::size_t>(4U, std::max<std::size_t>(1U, configured_workers / 8U));
            const std::size_t open_worker_budget = overlap_open_images ? 1U + open_cache_workers : 0U;
            const std::size_t archive_worker_budget = std::max<std::size_t>(1U, configured_workers - open_worker_budget);
            const int archive_workers = common_math::checked_cast<int>(
                std::min<std::size_t>({archive_tasks.size(), kArchivePipelineConcurrency, std::max<std::size_t>(1U, archive_worker_budget / 3U)}),
                "archive pipeline concurrency overflow");
            const std::size_t per_archive_worker_budget =
                archive_worker_budget / common_math::checked_cast<std::size_t>(archive_workers, "archive worker budget overflow");
            std::size_t decompression_workers = 0U;
            for (std::size_t candidate = 1U; 1U + candidate + std::max<std::size_t>(1U, candidate / 2U) <= per_archive_worker_budget; ++candidate) {
                decompression_workers = candidate;
            }
            const std::size_t archive_cache_workers = configured_workers == 1U ? 0U : std::max<std::size_t>(1U, decompression_workers / 2U);
            const std::size_t archive_download_connections = std::max<std::size_t>(1U, per_archive_worker_budget - 1U);
            progress.activity("Acquisition worker budget: " + std::to_string(archive_workers) + " archive controllers, " +
                              std::to_string(decompression_workers) + " decompression workers and " +
                              (archive_cache_workers == 0U ? std::string{"inline cache writes per archive, "}
                                                           : std::to_string(archive_cache_workers) + " cache writers per archive, ") +
                              (open_cache_workers == 0U ? std::string{"inline Open Images cache writes"}
                                                        : std::to_string(open_cache_workers) + " Open Images cache writers") +
                              ", " + std::to_string(archive_download_connections) + " download connections per archive");
            std::thread open_images_thread;
            if (overlap_open_images) {
                open_images_thread = std::thread([&] {
                    try {
                        open_image_result = acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers,
                                                                open_cache_workers, trace);
                    } catch (...) { record_pipeline_error(); }
                });
            }
            try {
                std::atomic<std::size_t> next_archive_task{0U};
                common_concurrency::parallel_for_range_indexed<int>(0, archive_workers, archive_workers, [&](const int, const int begin, const int end) {
                    for (int worker = begin; worker < end; ++worker) {
                        (void)worker;
                        while (true) {
                            const std::size_t task_index = next_archive_task.fetch_add(1U, std::memory_order_relaxed);
                            if (task_index >= archive_tasks.size()) { break; }
                            const ArchiveTask& task = archive_tasks[task_index];
                            const std::uint64_t source_total = source_totals.at(task.source);
                            try {
                                archive_results[task_index] = acquire_archive_images(
                                    cache, task.source, task.shard, task.image_ids, task.artifact, cancel_requested, &progress, &image_transfer_progress,
                                    &cache_storage_reservations, source_total, decompression_workers, archive_cache_workers, archive_download_connections,
                                    trace, {}, coconut, archive_member_parser(task), completion_slot, task.admitted);
                            } catch (...) {
                                record_pipeline_error();
                                return;
                            }
                        }
                    }
                });
            } catch (...) { record_pipeline_error(); }
            if (!coconut && !overlap_open_images && !pipeline_error) {
                try {
                    open_image_result =
                        acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace);
                } catch (...) { record_pipeline_error(); }
            }
            if (open_images_thread.joinable()) { open_images_thread.join(); }
            if (pipeline_error) { std::rethrow_exception(pipeline_error); }
            if (!coconut && !open_image_result) { throw std::runtime_error("Open Images acquisition did not complete"); }
            std::vector<CachedImageDirectory> coco_train_images;
            std::vector<CachedImageDirectory> coco_val_images;
            std::vector<CachedImageDirectory> object_images;
            std::vector<CachedImageDirectory> open_image_directories;
            if (!coconut) {
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
                if (coco_train_images.front().image_count + coco_train_images.front().quarantined.size() != coco_train_ids.size() ||
                    !coco_val_images.front().quarantined.empty() || coco_val_images.front().image_count != coco_val_ids.size()) {
                    throw std::runtime_error("COCO acquisition did not restore every selected image");
                }
                for (std::size_t index = 0U; index < object_images.size(); ++index) {
                    if (object_images[index].image_count + object_images[index].quarantined.size() != archive_tasks[index + 2U].image_ids.size()) {
                        throw std::runtime_error(
                            "Objects365 acquisition did not resolve every selected "
                            "training image");
                    }
                }
                if (open_image_result->available_image_ids.size() + quarantined.size() != open_images->images.size() ||
                    open_image_result->directory.image_count != open_image_result->available_image_ids.size()) {
                    throw std::runtime_error("Open Images acquisition resolution count is inconsistent");
                }
            }
            std::vector<std::uint64_t> coco_train_unavailable_ids;
            std::vector<std::uint64_t> objects_unavailable_ids;
            std::vector<std::uint64_t> open_images_unavailable_ids;
            const auto refresh_archive_availability = [&] {
                quarantined.erase(std::remove_if(quarantined.begin(), quarantined.end(),
                                                 [](const QuarantinedImage& image) { return image.source != BenchmarkDatasetSource::kOpenImagesV7; }),
                                  quarantined.end());
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
            if (!coconut) {
                refresh_archive_availability();
                progress.source_images(BenchmarkDatasetSource::kCoco2017, coco_selected_images, coco_selected_images);
                progress.source_complete(BenchmarkDatasetSource::kCoco2017,
                                         coco_indexes_cache_hit && coco_train_images.front().cache_hit && coco_val_images.front().cache_hit);
                progress.source_complete(
                    BenchmarkDatasetSource::kObjects365V2,
                    objects_index_cache_hit && std::ranges::all_of(object_images, [](const CachedImageDirectory& directory) { return directory.cache_hit; }));
                progress.source_complete(BenchmarkDatasetSource::kOpenImagesV7, open_images_index_cache_hit && open_image_result->directory.cache_hit);
            } else {
                for (const auto& [source, total] : source_totals) {
                    progress.source_images(source, total, total);
                    bool cache_hit = enhanced->annotation_cache_hit;
                    for (std::size_t i = 0; i < archive_tasks.size(); ++i)
                        if (archive_tasks[i].source == source) cache_hit = cache_hit && archive_results[i] && archive_results[i]->cache_hit;
                    progress.source_complete(source, cache_hit);
                }
                progress.source_complete(BenchmarkDatasetSource::kCoconut, enhanced->annotation_cache_hit);
            }
            const std::uint64_t kLabelPlanCount =
                enhanced ? enhanced->components.size() + static_cast<std::size_t>(enhanced->stock_validation.has_value()) : 4U;
            std::uint64_t completed_label_plans = 0U;
            progress.phase(DatasetCompilePhase::Labels, completed_label_plans, kLabelPlanCount);
            PreparedBenchmarkSplit train = make_split("train");
            PreparedBenchmarkSplit validation = make_split("val");
            std::uint64_t training_target_dropped_boxes = 0U;
            std::uint64_t validation_target_dropped_boxes = 0U;
            std::vector<SourceCompileCount> source_counts;
            const auto prepare_training_plan = [&](const bool report_progress) {
                PreparedBenchmarkSplit prepared = make_split("train");
                std::vector<SourceCompileCount> prepared_counts;
                prepared_counts.reserve(3U);
                std::uint64_t prepared_dropped_boxes = 0U;
                if (report_progress) { progress.activity("Preparing COCO training labels"); }
                prepared_counts.push_back(append_source_plan(*coco_train, coco_train_images,
                                                             coco_train_unavailable_ids.empty() ? nullptr : &coco_train_unavailable_ids, config.resolution,
                                                             config.resize_mode, false, &prepared, cancel_requested));
                if (report_progress) {
                    progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
                    progress.activity("Preparing Objects365 training labels");
                }
                prepared_counts.push_back(append_source_plan(*objects, object_images, objects_unavailable_ids.empty() ? nullptr : &objects_unavailable_ids,
                                                             config.resolution, config.resize_mode, false, &prepared, cancel_requested));
                if (report_progress) {
                    progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
                    progress.activity("Preparing Open Images training labels");
                }
                prepared_counts.push_back(append_source_plan(*open_images, open_image_directories,
                                                             open_images_unavailable_ids.empty() ? nullptr : &open_images_unavailable_ids, config.resolution,
                                                             config.resize_mode, false, &prepared, cancel_requested));
                if (report_progress) { progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount); }
                train = std::move(prepared);
                source_counts = std::move(prepared_counts);
                training_target_dropped_boxes = prepared_dropped_boxes;
            };
            SourceCompileCount validation_count;
            const auto prepare_enhanced_plans = [&] {
                train = make_split("train");
                validation = make_split("val");
                source_counts.clear();
                training_target_dropped_boxes = 0U;
                validation_target_dropped_boxes = 0U;
                for (const auto& component : enhanced->components) {
                    std::vector<CachedImageDirectory> directories;
                    std::map<std::pair<CoconutImageNamespace, std::uint16_t>, std::uint16_t> slots;
                    for (std::size_t task_index = 0; task_index < archive_tasks.size(); ++task_index) {
                        const auto& task = archive_tasks[task_index];
                        if (task.image_namespace != component.source) continue;
                        if (!archive_results[task_index]) throw std::runtime_error("COCONut archive did not settle");
                        slots.emplace(std::pair{task.image_namespace, task.numeric_shard},
                                      common_math::checked_cast<std::uint16_t>(directories.size(), "COCONut directory count overflow"));
                        directories.push_back(*archive_results[task_index]);
                    }
                    std::vector<std::uint16_t> image_sources;
                    image_sources.reserve(component.inventory.size());
                    for (const auto& row : component.inventory) image_sources.push_back(slots.at({row.physical.source, row.physical.shard}));
                    const auto release = std::ranges::find(coconut_catalog.releases, component.edition, &CoconutReleaseComponent::edition);
                    if (release == coconut_catalog.releases.end()) throw std::runtime_error("COCONut component release is absent");
                    const auto dimensions = coconut_image_dimensions(component, release->name, directories, image_sources, coconut_failures, progress,
                                                                      cancel_requested);
                    const bool is_validation = coconut_validation_component(component.edition);
                    auto& split = is_validation ? validation : train;
                    auto count = append_source_plan(component.index, directories, nullptr, config.resolution, config.resize_mode, true, &split,
                                                    cancel_requested, coconut_annotation_source(component.source), image_sources, dimensions);
                    auto& dropped_boxes = is_validation ? validation_target_dropped_boxes : training_target_dropped_boxes;
                    dropped_boxes = common_math::checked_add(dropped_boxes, count.dropped_boxes, "COCONut rejected annotation count overflow");
                    source_counts.push_back(count);
                    progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
                }
                if (enhanced->stock_validation) {
                    const auto task = std::ranges::find(archive_tasks, CoconutImageNamespace::CocoValidation, &ArchiveTask::image_namespace);
                    if (task == archive_tasks.end()) throw std::runtime_error("stock COCO validation archive is absent");
                    validation_count = append_source_plan(*enhanced->stock_validation, {*archive_results[task - archive_tasks.begin()]}, nullptr,
                                                          config.resolution, config.resize_mode, true, &validation, cancel_requested);
                }
            };
            if (enhanced)
                prepare_enhanced_plans();
            else {
                prepare_training_plan(true);
                progress.activity("Preparing COCO validation labels");
                validation_count =
                    append_source_plan(*coco_val, coco_val_images, nullptr, config.resolution, config.resize_mode, true, &validation, cancel_requested);
            }
            progress.phase(DatasetCompilePhase::Labels, kLabelPlanCount, kLabelPlanCount);
            const auto expected_validation_images = enhanced ? enhanced->validation_images : custom_catalog.coco_validation_images_count;
            if (validation.images.size() != expected_validation_images) throw std::runtime_error("compiled validation plan does not match admitted membership");
            const auto calculate_output_estimate = [&] {
                return common_math::checked_add(
                    estimate_output_bytes(train.images.size(), train.labels.size(), config.resolution, train.rle_pairs.size()),
                    estimate_output_bytes(validation.images.size(), validation.labels.size(), config.resolution, validation.rle_pairs.size()),
                    "benchmark total output estimate overflow");
            };
            std::uint64_t total_output_estimate = calculate_output_estimate();
            progress.projected(total_output_estimate);
            progress.activity("Checking exact compiled output storage");
            require_storage(output_parent, total_output_estimate, "benchmark compiled output staging", trace);
            progress.activity("Creating atomic benchmark staging directory");
            common_io::StagingDirectory staging(config.output_dir, ".", ".benchmark.tmp.XXXXXX", "cannot create benchmark dataset staging directory");
            const std::filesystem::path& staging_dir = staging.path();
            std::uint64_t total_compile_images =
                common_math::checked_add(train.images.size(), validation.images.size(), "benchmark compile image count overflow");
            ArtifactProgressTotals repair_transfer_progress;
            const auto rebuild_training_after_quarantine = [&] {
                progress.activity("Rebuilding training labels after image quarantine");
                prepare_training_plan(false);
                total_compile_images = common_math::checked_add(train.images.size(), validation.images.size(), "benchmark compile image count overflow");
                const std::uint64_t repaired_output_estimate = calculate_output_estimate();
                if (repaired_output_estimate > total_output_estimate) {
                    progress.activity(
                        "Checking expanded compiled output storage after image "
                        "repair");
                    require_storage(output_parent, repaired_output_estimate, "benchmark compiled output staging", trace);
                }
                total_output_estimate = repaired_output_estimate;
                progress.projected(total_output_estimate);
            };
            const auto replace_cached_directory = [](std::vector<CachedImageDirectory>* directories, CachedImageDirectory replacement) {
                const auto found = std::ranges::find(*directories, replacement.path, &CachedImageDirectory::path);
                if (found == directories->end()) { return false; }
                *found = std::move(replacement);
                return true;
            };
            const auto repair_cached_image = [&](const PreparedBenchmarkSplit& split, const BenchmarkImageReadError& error) {
                if (error.source_index() >= split.sources.size()) { throw std::runtime_error("benchmark image repair source index is invalid"); }
                const auto failed_record = std::ranges::find_if(split.images, [&](const EncodedImageRecord& image) {
                    return image.source_index == error.source_index() && image.source_image_id == error.source_image_id();
                });
                if (failed_record == split.images.end()) {
                    throw std::runtime_error(
                        "benchmark image repair record is absent from the "
                        "compile plan");
                }
                const ImageDecodeProbe decode_probe{error.source_image_id(), failed_record->source_width, failed_record->source_height};
                const std::filesystem::path source_root = split.sources[error.source_index()].root;
                const std::filesystem::path image_path = cached_image_path(source_root, error.source_image_id());
                if (std::filesystem::is_regular_file(image_path)) {
                    progress.activity("Failure-only SHA-256 diagnosis for cached image " + std::to_string(error.source_image_id()));
                    const std::string failure_sha256 =
                        mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(image_path, [&] { return cancel_requested.requested(); }));
                    trace_benchmark_event(trace, "benchmark.images.failure_sha256", [&] {
                        return nlohmann::json{
                            {"path", image_path.string()}, {"image_id", error.source_image_id()}, {"sha256", failure_sha256}, {"reason", error.what()}};
                    });
                }
                if (open_image_result && source_root == open_image_result->directory.path) {
                    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                                             "Repairing failed Open Images JPEG " + std::to_string(error.source_image_id()));
                    std::vector<QuarantinedImage> repaired_quarantined;
                    AcquiredOpenImages repaired = acquire_open_images(cache, *open_images, &repaired_quarantined, cancel_requested, &progress,
                                                                      acquisition_num_workers, open_cache_workers, trace, decode_probe);
                    quarantined = std::move(repaired_quarantined);
                    open_image_result = std::move(repaired);
                    open_image_directories.front() = open_image_result->directory;
                    refresh_archive_availability();
                    rebuild_training_after_quarantine();
                    return;
                }
                const auto task = std::ranges::find_if(archive_tasks, [&](const ArchiveTask& candidate) {
                    return cache.source_images(benchmark_source_name(candidate.source)) / candidate.shard == source_root;
                });
                if (task == archive_tasks.end()) {
                    throw std::runtime_error(
                        "benchmark image repair cannot identify its "
                        "source archive");
                }
                progress.source_activity(task->source, "Repairing failed cached JPEG " + std::to_string(error.source_image_id()) + " from " + task->shard);
                progress.source_images(task->source, 0U, task->image_ids.size());
                CachedImageDirectory repaired = acquire_archive_images(
                    cache, task->source, task->shard, task->image_ids, task->artifact, cancel_requested, &progress, &repair_transfer_progress,
                    &cache_storage_reservations, task->image_ids.size(), decompression_workers, archive_cache_workers, archive_download_connections, trace,
                    decode_probe, coconut, archive_member_parser(*task), completion_slot, task->admitted);
                if (repaired.image_count + repaired.quarantined.size() != task->image_ids.size()) {
                    throw std::runtime_error(
                        "archive repair did not resolve every selected "
                        "image");
                }
                if (enhanced) {
                    archive_results[task - archive_tasks.begin()] = std::move(repaired);
                    return;
                }
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
                }
            };
            const auto compile_split = [&](const BenchmarkWriteRequest& request, const std::uint64_t completed_before) {
                std::vector<std::pair<std::uint16_t, std::uint64_t>> repaired_images;
                repaired_images.reserve(4U);
                for (std::uint32_t attempt = 1U;; ++attempt) {
                    try {
                        write_split_with_progress(request, &progress, completed_before, total_compile_images, trace);
                        return;
                    } catch (const BenchmarkImageReadError& error) {
                        const auto repair_key = std::pair{error.source_index(), error.source_image_id()};
                        if (std::ranges::find(repaired_images, repair_key) != repaired_images.end()) { throw; }
                        repaired_images.push_back(repair_key);
                        trace_benchmark_event(trace, "benchmark.pixel_compile.cache_repair", [&] {
                            return nlohmann::json{
                                {"split", request.split.name}, {"image_id", error.source_image_id()}, {"attempt", attempt}, {"reason", error.what()}};
                        });
                        repair_cached_image(request.split, error);
                        progress.activity("Retrying " + request.split.name +
                                          " pixel compilation from zero after bounded cache "
                                          "repair");
                    }
                }
            };
            compile_split(BenchmarkWriteRequest{train,
                                                staging_dir / "train.bin",
                                                config.resolution,
                                                config.num_workers,
                                                {},
                                                false,
                                                cancel_requested,
                                                {},
                                                config.perceptual_downscale,
                                                config.resize_mode},
                          0U);
            compile_split(BenchmarkWriteRequest{validation,
                                                staging_dir / "val.bin",
                                                config.resolution,
                                                config.num_workers,
                                                {},
                                                false,
                                                cancel_requested,
                                                {},
                                                config.perceptual_downscale,
                                                config.resize_mode},
                          train.images.size());
            progress.activity("Finalizing rejected and quarantined records");
            std::ranges::sort(quarantined);
            quarantined.erase(std::unique(quarantined.begin(), quarantined.end(),
                                          [](const QuarantinedImage& left, const QuarantinedImage& right) {
                                              return left.source == right.source && left.image_id == right.image_id;
                                          }),
                              quarantined.end());
            for (const QuarantinedImage& image : quarantined) {
                trace_benchmark_event(trace, "benchmark.quarantine.image", [&] {
                    return nlohmann::json{{"source", benchmark_source_name(image.source)}, {"image_id", image.image_id}, {"reason", image.reason}};
                });
            }
            trace_benchmark_event(trace, "benchmark.quarantine.final", [&] { return nlohmann::json{{"images", quarantined.size()}}; });
            std::uint64_t annotation_drops = 0U;
            const auto add_annotation_drops = [&](const NormalizedAnnotationIndex& index) {
                annotation_drops = common_math::checked_add(annotation_drops, index.rejected.degenerate_boxes, "benchmark rejected annotation count overflow");
                annotation_drops = common_math::checked_add(annotation_drops, index.rejected.duplicate_boxes, "benchmark rejected annotation count overflow");
            };
            if (enhanced) {
                for (const auto& component : enhanced->components) add_annotation_drops(component.index);
                if (enhanced->stock_validation) add_annotation_drops(*enhanced->stock_validation);
            } else {
                add_annotation_drops(*coco_train);
                add_annotation_drops(*coco_val);
                add_annotation_drops(*objects);
                add_annotation_drops(*open_images);
            }
            const std::uint64_t target_dropped_boxes =
                common_math::checked_add(training_target_dropped_boxes, validation_target_dropped_boxes, "benchmark target annotation count overflow");
            progress.rejected(common_math::checked_add(annotation_drops, target_dropped_boxes, "benchmark rejected annotation count overflow"),
                              quarantined.size());
            constexpr std::uint64_t kSyncStepCount = 4U;
            progress.phase(DatasetCompilePhase::Syncing, 0U, kSyncStepCount);
            progress.activity("Inspecting staged training dataset");
            const CompiledDatasetInfo train_info = inspect_compiled_dataset(staging_dir / "train.bin");
            progress.phase(DatasetCompilePhase::Syncing, 1U, kSyncStepCount);
            progress.activity("Inspecting staged validation dataset");
            const CompiledDatasetInfo val_info = inspect_compiled_dataset(staging_dir / "val.bin");
            progress.phase(DatasetCompilePhase::Syncing, 2U, kSyncStepCount);
            if (train_info.image_count != train.images.size() || val_info.image_count != expected_validation_images ||
                !std::ranges::equal(train_info.class_names(), train.class_names) || !std::ranges::equal(val_info.class_names(), validation.class_names)) {
                throw std::runtime_error("staged benchmark compiled files do not match their plans");
            }
            progress.activity("Building benchmark manifest");
            nlohmann::json manifest{
                {"supplemental_sampling",
                 {{"revision", kSupplementalSamplingRevision},
                  {"target_images", combined_sampling.target_images},
                  {"open_images_floor", combined_sampling.open_images_floor},
                  {"open_images_ceiling", combined_sampling.open_images_ceiling},
                  {"objects365_shards", combined_sampling.objects365_shards},
                  {"objects365_archive_bytes", combined_sampling.objects365_archive_bytes}}},
                {"projected_output_bytes", total_output_estimate},
                {"train",
                 {{"images", train.images.size()},
                  {"boxes", train.labels.size()},
                  {"mask_rle_pairs", train.rle_pairs.size()},
                  {"bytes", std::filesystem::file_size(staging_dir / "train.bin")}}},
                {"val",
                 {{"source", "coco"},
                  {"images", validation.images.size()},
                  {"boxes", validation.labels.size()},
                  {"mask_rle_pairs", validation.rle_pairs.size()},
                  {"bytes", std::filesystem::file_size(staging_dir / "val.bin")}}},
                {"sources", nlohmann::json::array()},
                {"image_cache", nlohmann::json::array()},
                {"quarantined_images", nlohmann::json::array()},
            };
            const auto append_source_manifest = [&](const NormalizedAnnotationIndex& source_index, const SourceCompileCount& count,
                                                    const std::string_view annotation_url, const std::filesystem::path& normalized_index_path,
                                                    const SupplementalSamplingStats* sampling) {
                const nlohmann::json cache_identity = read_json_file(normalized_index_path.string() + ".complete.json");
                nlohmann::json source_manifest{
                    {"name", benchmark_source_name(source_index.source)},
                    {"version", benchmark_source_version(source_index.source)},
                    {"annotation_url", annotation_url},
                    {"annotation_sha256", source_index.annotation_sha256},
                    {"normalized_index", normalized_index_path.lexically_relative(cache.root).string()},
                    {"normalized_index_identity", cache_identity.at("identity").get<std::string>()},
                    {"indexed_images", sampling != nullptr ? sampling->full_images : source_index.images.size()},
                    {"indexed_boxes", sampling != nullptr ? sampling->full_boxes : source_index.boxes.size()},
                    {"indexed_mask_rle_pairs", source_index.mask_rle_pairs.size()},
                    {"selected_images", count.selected_images},
                    {"selected_boxes", source_index.boxes.size()},
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
            if (!enhanced) {
                manifest["source_catalog"] = source_catalog_manifest(custom_catalog);
                append_source_manifest(*coco_train, source_counts[0], custom_catalog.coco_annotations.url, coco_train_index_path, nullptr);
                append_source_manifest(*objects, source_counts[1], custom_catalog.objects_annotations.url, objects_index_path, &objects_sampling.stats);
                append_source_manifest(*open_images, source_counts[2], custom_catalog.open_images_boxes.url, open_images_index_path,
                                       &open_images_sampling.stats);
                const nlohmann::json val_cache_identity = read_json_file(coco_val_index_path.string() + ".complete.json");
                manifest["validation_source"] = {
                    {"name", "coco"},
                    {"version", benchmark_source_version(BenchmarkDatasetSource::kCoco2017)},
                    {"annotation_sha256", coco_val->annotation_sha256},
                    {"normalized_index", coco_val_index_path.lexically_relative(cache.root).string()},
                    {"normalized_index_identity", val_cache_identity.at("identity").get<std::string>()},
                    {"selected_images", validation_count.selected_images},
                    {"compiled_images", validation_count.compiled_images},
                    {"compiled_boxes", validation_count.compiled_boxes},
                    {"compiled_mask_rle_pairs", validation.rle_pairs.size()},
                    {"rejected_records", reject_json(coco_val->rejected)},
                };
            } else {
                manifest.erase("supplemental_sampling");
                manifest["recipe"] = enhanced->manifest;
                manifest["val"]["source"] = "selected-coconut-validation";
                manifest["sources"] = enhanced->manifest["components"];
            }
            std::uint64_t cached_image_bytes = 0U;
            const auto append_image_cache = [&](const std::vector<CachedImageDirectory>& directories) {
                for (const CachedImageDirectory& directory : directories) {
                    manifest["image_cache"].push_back({{"source", directory.source},
                                                       {"shard", directory.shard},
                                                       {"path", directory.path.lexically_relative(cache.root).string()},
                                                       {"identity", directory.identity},
                                                       {"selection_sha256", directory.selection_sha256},
                                                       {"images", directory.image_count},
                                                       {"bytes", directory.image_bytes},
                                                       {"cache_hit", directory.cache_hit}});
                    cached_image_bytes = common_math::checked_add(cached_image_bytes, directory.image_bytes, "benchmark cached image byte total overflow");
                }
            };
            if (enhanced) {
                for (const auto& result : archive_results) {
                    if (!result) throw std::runtime_error("COCONut image acquisition incomplete");
                    append_image_cache({*result});
                }
            }
            append_image_cache(coco_train_images);
            append_image_cache(coco_val_images);
            append_image_cache(object_images);
            append_image_cache(open_image_directories);
            manifest["cached_image_bytes"] = cached_image_bytes;
            manifest["compiled_bytes"] = common_math::checked_add(manifest["train"]["bytes"].get<std::uint64_t>(),
                                                                  manifest["val"]["bytes"].get<std::uint64_t>(), "benchmark compiled byte total overflow");
            for (const QuarantinedImage& image : quarantined) {
                manifest["quarantined_images"].push_back(
                    {{"source", benchmark_source_name(image.source)}, {"image_id", image.image_id}, {"reason", image.reason}});
            }
            progress.activity("Writing benchmark manifest");
            publish_benchmark_manifest(config, staging_dir, cache.root, std::move(manifest), cancel_requested);
            progress.phase(DatasetCompilePhase::Syncing, 3U, kSyncStepCount);
            progress.activity("Syncing staged benchmark dataset");
            sync_directory(staging_dir);
            progress.phase(DatasetCompilePhase::Syncing, kSyncStepCount, kSyncStepCount);
            progress.phase(DatasetCompilePhase::Publishing, 0U, 1U);
            progress.activity("Atomically publishing benchmark dataset");
            throw_if_benchmark_cancelled(cancel_requested);
            publish_dataset_directory(staging_dir, config.output_dir, config.overwrite, cancel_requested, trace);
            staging.published();
            progress.phase(DatasetCompilePhase::Publishing, 1U, 1U);
            trace_benchmark_event(trace, "benchmark.publication.complete", [&] {
                return nlohmann::json{{"output", config.output_dir.string()},
                                      {"train_images", train.images.size()},
                                      {"val_images", validation.images.size()},
                                      {"train_mask_rle_pairs", train.rle_pairs.size()},
                                      {"val_mask_rle_pairs", validation.rle_pairs.size()},
                                      {"bytes", total_output_estimate}};
            });
            return;
        } catch (const PhysicalArchiveRecovery& error) {
            // The extraction scheduler has joined all workers before propagating its
            // first failure. Clear only its internal stop, never external cancellation.
            throw_if_benchmark_cancelled(cancellation_state.external);
            cancel_signal->store(false, std::memory_order_relaxed);
            auto& archive = error.archive();
            membership.reset();
            acquire_physical_inventory(archive, inventory, cache, progress, physical_progress, physical_storage, effective_num_workers, cancel_requested, trace,
                                       error.what());
            refreshed_sources.push_back(archive.origin.source);
            membership.emplace(inventory, cancel_requested);
        } catch (const CoconutPhysicalMembershipError& error) {
            membership.reset();
            bool repaired = false;
            for (auto& archive : admitted) {
                if (archive.origin.source != error.source() &&
                    !(error.source() == CoconutImageNamespace::CocoTrain && archive.origin.source == CoconutImageNamespace::CocoUnlabeled))
                    continue;
                acquire_physical_inventory(archive, inventory, cache, progress, physical_progress, physical_storage, effective_num_workers, cancel_requested,
                                           trace, error.what());
                if (std::ranges::find(refreshed_sources, archive.origin.source) == refreshed_sources.end()) refreshed_sources.push_back(archive.origin.source);
                repaired = true;
                if (std::ranges::any_of(inventory,
                                        [&](const auto& image) { return image.source == archive.origin.source && image.image_id == error.image_id(); }))
                    break;
            }
            if (!repaired) throw;
            membership.emplace(inventory, cancel_requested);
        }
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
    if (!progress.complete && progress.resumed) { status += " · resumed"; }
    if (!progress.complete && progress.retry_count != 0U) { status += " · " + std::to_string(progress.retry_count) + " retries"; }
    return status;
}
}  // namespace mmltk::backend::data
