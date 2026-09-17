#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
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
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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
// CLEANUP-IGNORE: This benchmark compiler imports and includes the concrete owners directly used by its implementation.
#include "benchmark_annotations.h"
#include "benchmark_cache.h"
#include "benchmark_catalog.h"
#include "benchmark_download.h"
#include "benchmark_images.h"
#include "benchmark_jpeg.h"
#include "benchmark_sampling.h"
#include "benchmark_writer.h"
#include "detail/benchmark_compiler.h"
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
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kArchivePipelineConcurrency = 4U;
constexpr std::size_t kArchiveReadBufferBytes = std::size_t{1024U} * 1024U;
constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
[[nodiscard]] BenchmarkTraceSink make_trace_sink(const BenchmarkTraceCallback& callback) {
    if (!callback) { return {}; }
    const auto mutex = std::make_shared<std::mutex>();
    return [callback, mutex](const std::string_view event, const nlohmann::json& fields) {
        const std::string serialized = fields.dump();
        const std::lock_guard lock(*mutex);
        callback(event, serialized);
    };
}
[[nodiscard]] std::string combined_artifact_digest(const std::string& left, const std::string& right) {
    const mmltk::common::io::Sha256Digest left_digest = mmltk::common::io::parse_sha256_hex(left);
    const mmltk::common::io::Sha256Digest right_digest = mmltk::common::io::parse_sha256_hex(right);
    std::array<std::uint8_t, 64> identity{};
    std::ranges::copy(left_digest, identity.begin());
    std::ranges::copy(right_digest, identity.begin() + left_digest.size());
    return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(identity));
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
[[nodiscard]] DownloadRequest make_download_request(const BenchmarkCacheLayout& cache, const std::string_view source, const CatalogArtifact& artifact) {
    DownloadRequest request;
    request.artifact_id = artifact.artifact_id;
    request.url = artifact.url;
    request.destination = cache.source_downloads(source) / artifact.filename;
    request.lock_path = cache.locks / (artifact.artifact_id + ".lock");
    request.expected_size = artifact.expected_size;
    if (!artifact.expected_sha256.empty()) { request.expected_sha256 = artifact.expected_sha256; }
    request.maximum_attempts = kMaximumAttempts;
    return request;
}
[[nodiscard]] BenchmarkDatasetSource artifact_source(const std::string_view artifact_id) {
    if (artifact_id.starts_with("coco-")) { return BenchmarkDatasetSource::kCoco2017; }
    if (artifact_id.starts_with("objects365-")) { return BenchmarkDatasetSource::kObjects365V2; }
    if (artifact_id.starts_with("open-images-")) { return BenchmarkDatasetSource::kOpenImagesV7; }
    throw std::runtime_error("benchmark download progress has an unknown artifact");
}
struct ArtifactProgressTotals {
    std::unordered_map<std::string, std::uint64_t> completed;
    std::unordered_map<std::string, std::uint64_t> total;
    std::unordered_map<std::string, std::uint32_t> retries;
    std::unordered_map<std::string, bool> cache_hits;
    std::unordered_map<std::string, bool> resumed;
    std::unordered_map<std::string, DownloadProgressPhase> phases;
    void update(const DownloadProgress& update, ProgressReporter* reporter) {
        const std::lock_guard lock(mutex);
        const auto prior_phase = phases.find(update.artifact_id);
        const bool phase_changed = prior_phase == phases.end() || prior_phase->second != update.phase;
        phases[update.artifact_id] = update.phase;
        completed[update.artifact_id] = update.completed_bytes;
        total[update.artifact_id] = update.total_bytes;
        retries[update.artifact_id] = std::max(retries[update.artifact_id], update.attempt > 0U ? update.attempt - 1U : 0U);
        cache_hits[update.artifact_id] = update.cache_hit;
        resumed[update.artifact_id] = resumed[update.artifact_id] || update.resumed;
        const BenchmarkDatasetSource source = artifact_source(update.artifact_id);
        if (phase_changed && update.phase == DownloadProgressPhase::kDownloading) {
            reporter->source_activity(source, "Downloading " + update.artifact_id);
        } else if (update.phase == DownloadProgressPhase::kVerifyingCachedArtifact) {
            reporter->source_activity(source, "Verifying cached " + update.artifact_id);
        } else if (update.phase == DownloadProgressPhase::kVerifyingDownloadedArtifact) {
            reporter->source_activity(source, "Verifying downloaded " + update.artifact_id);
        }
        std::uint64_t source_completed = 0U;
        std::uint64_t source_total = 0U;
        std::uint64_t source_retries = 0U;
        bool source_cache_hit = true;
        bool source_resumed = false;
        for (const auto& [artifact, bytes] : completed) {
            if (artifact_source(artifact) == source) {
                source_completed = common_math::checked_add(source_completed, bytes, "source progress overflow");
                source_total = common_math::checked_add(source_total, total[artifact], "source progress overflow");
                source_retries = common_math::checked_add(source_retries, retries[artifact], "source retry count overflow");
                source_cache_hit = source_cache_hit && cache_hits[artifact];
                source_resumed = source_resumed || resumed[artifact];
            }
        }
        reporter->source_bytes(source, source_completed, source_total, common_math::checked_cast<std::uint32_t>(source_retries, "source retry count overflow"),
                               source_cache_hit, source_resumed);
    }
    std::mutex mutex;
};
struct ArchiveDestroy {
    void operator()(archive* reader) const noexcept {
        if (reader != nullptr) { (void)archive_read_free(reader); }
    }
};
using ArchiveReader = std::unique_ptr<archive, ArchiveDestroy>;
class StagingFileCleanup {
   public:
    void set(std::filesystem::path path) { path_ = std::move(path); }
    ~StagingFileCleanup() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

   private:
    std::filesystem::path path_;
};
[[nodiscard]] std::string archive_error(const archive* reader) {
    const char* message = archive_error_string(const_cast<archive*>(reader));
    return message != nullptr ? message : "unknown libarchive error";
}
[[nodiscard]] std::filesystem::path extract_manifest_path(const std::filesystem::path& path) { return path.string() + ".extract.json"; }
[[nodiscard]] std::string extract_archive_member(const std::filesystem::path& archive_path, const std::string_view member_suffix,
                                                 const std::filesystem::path& output_path, const std::string_view archive_identity,
                                                 const std::filesystem::path& lock_path, mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                 const BenchmarkTraceSink& trace) {
    ArtifactLease lease = ArtifactLease::acquire(lock_path, cancel_requested);
    const std::filesystem::path completion = extract_manifest_path(output_path);
    if (std::filesystem::is_regular_file(output_path) && std::filesystem::is_regular_file(completion)) {
        try {
            const nlohmann::json metadata = read_json_file(completion);
            if (metadata.value("schema_version", 0U) == kBenchmarkCacheSchemaVersion && metadata.value("complete", false) &&
                metadata.value("archive_identity", std::string{}) == archive_identity && metadata.value("member", std::string{}) == member_suffix &&
                metadata.value("size", 0ULL) == std::filesystem::file_size(output_path)) {
                const std::string identity = metadata.value("identity", std::string{});
                if (!identity.empty()) {
                    trace_benchmark_event(trace, "benchmark.archive.extract_cache_hit",
                                          [&] { return nlohmann::json{{"member", member_suffix}, {"path", output_path.string()}}; });
                    return identity;
                }
            }
        } catch (const std::exception& error) {
            trace_benchmark_event(trace, "benchmark.archive.extract_cache_invalid",
                                  [&] { return nlohmann::json{{"member", member_suffix}, {"path", output_path.string()}, {"reason", error.what()}}; });
        }
    }
    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);
    if (remove_error) { throw std::filesystem::filesystem_error("cannot remove invalid extracted annotation", output_path, remove_error); }
    remove_error.clear();
    std::filesystem::remove(completion, remove_error);
    if (remove_error) { throw std::filesystem::filesystem_error("cannot remove invalid extraction metadata", completion, remove_error); }
    ArchiveReader reader(archive_read_new());
    if (!reader) { throw std::runtime_error("cannot allocate benchmark annotation archive reader"); }
    if (archive_read_support_filter_all(reader.get()) < ARCHIVE_WARN || archive_read_support_format_tar(reader.get()) < ARCHIVE_WARN ||
        archive_read_support_format_zip(reader.get()) < ARCHIVE_WARN ||
        archive_read_open_filename(reader.get(), archive_path.c_str(), kArchiveReadBufferBytes) < ARCHIVE_WARN) {
        throw std::runtime_error("cannot open benchmark annotation archive: " + archive_error(reader.get()));
    }
    archive_entry* entry = nullptr;
    bool found = false;
    std::string staging_text = output_path.string() + ".tmp.XXXXXX";
    common_io::FileHandle staging;
    std::filesystem::path staging_path;
    StagingFileCleanup staging_cleanup;
    std::uint64_t expected_size = 0U;
    while (true) {
        throw_if_benchmark_cancelled(cancel_requested);
        int status = ARCHIVE_RETRY;
        for (std::uint32_t retry = 0U; status == ARCHIVE_RETRY && retry < 8U; ++retry) { status = archive_read_next_header(reader.get(), &entry); }
        if (status == ARCHIVE_EOF) { break; }
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
            throw std::runtime_error("cannot read benchmark annotation archive: " + archive_error(reader.get()));
        }
        const char* pathname = archive_entry_pathname(entry);
        const std::string_view name = pathname != nullptr ? std::string_view(pathname) : std::string_view{};
        if (!name.ends_with(member_suffix)) {
            int skip_status = ARCHIVE_RETRY;
            for (std::uint32_t retry = 0U; skip_status == ARCHIVE_RETRY && retry < 8U; ++retry) { skip_status = archive_read_data_skip(reader.get()); }
            if (skip_status < ARCHIVE_WARN) { throw std::runtime_error("cannot skip benchmark annotation archive member"); }
            continue;
        }
        if (found || archive_entry_filetype(entry) != AE_IFREG || archive_entry_size(entry) <= 0) {
            throw std::runtime_error("benchmark annotation archive member is missing or ambiguous");
        }
        found = true;
        expected_size = common_math::checked_cast<std::uint64_t>(archive_entry_size(entry), "extracted annotation size overflow");
        staging = common_io::FileHandle::create_unique_output(staging_text,
                                                              common_math::checked_cast<std::size_t>(expected_size, "extracted annotation size overflow"));
        staging_path = staging_text;
        staging_cleanup.set(staging_path);
        std::vector<std::uint8_t> buffer(kArchiveReadBufferBytes);
        std::uint64_t offset = 0U;
        while (offset < expected_size) {
            throw_if_benchmark_cancelled(cancel_requested);
            const std::size_t remaining = common_math::checked_cast<std::size_t>(expected_size - offset, "extraction remaining size overflow");
            la_ssize_t count = ARCHIVE_RETRY;
            for (std::uint32_t retry = 0U; count == ARCHIVE_RETRY && retry < 8U; ++retry) {
                count = archive_read_data(reader.get(), buffer.data(), std::min(buffer.size(), remaining));
            }
            if (count <= 0) { throw std::runtime_error("cannot read complete benchmark annotation archive member"); }
            staging.pwrite_all(buffer.data(), common_math::checked_cast<std::size_t>(count, "archive read overflow"),
                               common_math::checked_cast<std::size_t>(offset, "archive write offset overflow"));
            offset += common_math::checked_cast<std::uint64_t>(count, "archive read overflow");
        }
    }
    if (!found) { throw std::runtime_error("benchmark annotation archive does not contain " + std::string(member_suffix)); }
    staging.sync_data();
    staging = common_io::FileHandle{};
    std::string identity_material;
    identity_material.reserve(archive_identity.size() + member_suffix.size() + 64U);
    identity_material.append(archive_identity);
    identity_material.push_back('\n');
    identity_material.append(member_suffix);
    identity_material.push_back('\n');
    identity_material.append(std::to_string(expected_size));
    const std::string identity = mmltk::common::io::sha256_hex(
        mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(identity_material.data()), identity_material.size())));
    throw_if_benchmark_cancelled(cancel_requested);
    common_io::publish_staged_path_atomically(staging_path, output_path, true);
    write_json_atomically(completion,
                          nlohmann::json{{"schema_version", kBenchmarkCacheSchemaVersion},
                                         {"complete", true},
                                         {"archive_identity", archive_identity},
                                         {"member", member_suffix},
                                         {"size", expected_size},
                                         {"identity", identity},
                                         {"integrity_mode", "archive_structure_size"}},
                          cancel_requested);
    trace_benchmark_event(trace, "benchmark.archive.extracted",
                          [&] { return nlohmann::json{{"member", member_suffix}, {"bytes", expected_size}, {"identity", identity}}; });
    return identity;
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
class RequiredImageDecodeError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
[[nodiscard]] CachedImageDirectory acquire_archive_images(const BenchmarkCacheLayout& cache, const BenchmarkDatasetSource source, std::string shard,
                                                          const std::vector<std::uint64_t>& expected_ids, const CatalogArtifact& artifact,
                                                          mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress,
                                                          ArtifactProgressTotals* transfer_progress, StorageReservationPool* storage_reservations,
                                                          std::atomic<std::uint64_t>* source_extracted_images, const std::uint64_t source_total_images,
                                                          const std::size_t decompression_workers, const std::size_t cache_write_workers,
                                                          const std::size_t download_connections, const BenchmarkTraceSink& trace,
                                                          const std::optional<JpegDecodeProbe> decode_probe = std::nullopt) {
    if (expected_ids.empty()) { throw std::runtime_error("benchmark archive extraction cannot have an empty image selection"); }
    const std::string source_name(benchmark_source_name(source));
    const std::string archive_name = source_name + " " + shard;
    const std::filesystem::path image_root = cache.source_images(source_name) / shard;
    progress->source_activity(source, "Waiting for " + archive_name + " extraction lock");
    ArtifactLease extraction_lease = ArtifactLease::acquire(cache.locks / (source_name + "-" + shard + ".images.lock"), cancel_requested);
    if (decode_probe) {
        if (!std::ranges::binary_search(expected_ids, decode_probe->image_id)) {
            throw std::runtime_error("benchmark archive decode probe ID is not selected");
        }
        progress->source_activity(source, "Invalidating failed " + archive_name + " JPEG under the extraction lock");
        remove_cache_path(image_root / ".complete.json");
        remove_cache_path(cached_image_path(image_root, decode_probe->image_id));
    }
    const std::string source_identity = source_name + ":" + shard + ":" + artifact.artifact_id + ":" + std::string(kBenchmarkCatalogRevision);
    progress->source_activity(source, "Checking " + archive_name + " cache completion status");
    const bool quarantine_unavailable = archive_selection_allows_quarantine(source, shard);
    std::uint64_t cached_image_bytes = 0U;
    std::vector<CachedImageRejection> cached_quarantine;
    if (validate_cached_image_group(image_root, image_root / ".complete.json", source_identity, expected_ids, &cached_image_bytes, cancel_requested, trace,
                                    quarantine_unavailable ? &cached_quarantine : nullptr)) {
        std::vector<std::uint64_t> available_ids;
        available_ids.reserve(expected_ids.size() - cached_quarantine.size());
        for (const std::uint64_t image_id : expected_ids) {
            if (!std::ranges::binary_search(cached_quarantine, image_id, {}, &CachedImageRejection::image_id)) { available_ids.push_back(image_id); }
        }
        const std::uint64_t source_completed = source_extracted_images->fetch_add(expected_ids.size(), std::memory_order_relaxed) + expected_ids.size();
        progress->source_images(source, source_completed, source_total_images);
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
    const std::uint64_t acquisition_storage = common_math::checked_add(
        common_math::checked_multiply(expected_ids.size(), kEstimatedJpegBytes, "benchmark image cache estimate overflow"),
        artifact.expected_size != 0U ? artifact.expected_size : kArchiveScratchBytes, "benchmark archive acquisition estimate overflow");
    StorageReservationPool::Reservation storage_reservation = storage_reservations->reserve(acquisition_storage, source_name + " archive and extracted images");
    const DownloadRequest request = make_download_request(cache, source_name, artifact);
    std::exception_ptr last_error;
    for (std::uint32_t attempt = 1U; attempt <= 3U; ++attempt) {
        throw_if_benchmark_cancelled(cancel_requested);
        std::uint64_t attempt_reported = 0U;
        try {
            progress->source_activity(source,
                                      "Downloading " + archive_name + " image archive with " + std::to_string(download_connections) + " download connections");
            const std::vector<DownloadResult> downloads = download_artifacts(
                {request}, download_connections, cancel_requested, [&](const DownloadProgress& update) { transfer_progress->update(update, progress); }, trace);
            progress->source_activity(source, "Opening " + archive_name + " image archive");
            JpegValidator jpeg_validator;
            CachedImageDirectory extracted = extract_selected_archive_images(ArchiveExtractionRequest{
                .archive_path = downloads.front().path,
                .source_identity = source_identity,
                .output_root = image_root,
                .source = source_name,
                .shard = shard,
                .selected_image_ids = expected_ids,
                .image_id_parser = parse_archive_image_id,
                .cancel_requested = cancel_requested,
                .progress =
                    [&](const std::uint64_t completed, const std::uint64_t) {
                        const std::uint64_t delta = completed - attempt_reported;
                        attempt_reported = completed;
                        const std::uint64_t source_completed = source_extracted_images->fetch_add(delta, std::memory_order_relaxed) + delta;
                        progress->source_images(source, source_completed, source_total_images);
                    },
                .validator =
                    [&](const std::uint64_t image_id, const std::span<const std::uint8_t> encoded) {
                        if (!has_complete_jpeg_markers(encoded)) {
                            if (decode_probe && decode_probe->image_id == image_id && !quarantine_unavailable) {
                                throw RequiredImageDecodeError(
                                    "required validation JPEG remains "
                                    "incomplete after bounded repair");
                            }
                            throw std::runtime_error("selected archive entry is not a complete JPEG");
                        }
                        if (decode_probe && decode_probe->image_id == image_id) {
                            try {
                                jpeg_validator.validate_decodable(encoded, decode_probe->expected_width, decode_probe->expected_height);
                            } catch (const InvalidJpegError& error) {
                                if (!quarantine_unavailable) {
                                    throw RequiredImageDecodeError(
                                        "required validation JPEG remains "
                                        "undecodable after bounded repair: " +
                                        std::string(error.what()));
                                }
                                throw;
                            }
                        } else {
                            (void)jpeg_validator.read_header(encoded);
                        }
                    },
                .trace = trace,
                .quarantine_unavailable = quarantine_unavailable,
                .decompression_workers = decompression_workers,
                .cache_write_workers = cache_write_workers,
                .activity = [&](const std::string_view activity) { progress->source_activity(source, archive_name + ": " + std::string(activity)); },
            });
            progress->source_activity(source, "Retaining completed " + archive_name + " archive in cache");
            extracted.cache_hit = false;
            return extracted;
        } catch (const RequiredImageDecodeError&) { throw; } catch (...) {
            last_error = std::current_exception();
            std::string failure_reason = "non-standard exception";
            try {
                std::rethrow_exception(last_error);
            } catch (const std::exception& error) { failure_reason = error.what(); } catch (...) {
                failure_reason = "non-standard exception";
            }
            if (attempt_reported != 0U) {
                const std::uint64_t source_completed = source_extracted_images->fetch_sub(attempt_reported, std::memory_order_relaxed) - attempt_reported;
                progress->source_images(source, source_completed, source_total_images);
            }
            throw_if_benchmark_cancelled(cancel_requested);
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
            std::error_code cleanup_error;
            if (!common_io::remove_tree_no_follow(image_root, cleanup_error)) {
                throw std::filesystem::filesystem_error("cannot clear failed benchmark extraction", image_root, cleanup_error);
            }
            trace_benchmark_event(trace, "benchmark.images.archive_retry",
                                  [&] { return nlohmann::json{{"source", source_name}, {"shard", shard}, {"attempt", attempt}, {"reason", failure_reason}}; });
        }
    }
    std::rethrow_exception(last_error);
}
[[nodiscard]] std::optional<NormalizedAnnotationIndex> discover_cached_index(const std::filesystem::path& path, const BenchmarkDatasetSource source,
                                                                             const std::string_view split,
                                                                             mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                             const BenchmarkTraceSink& trace) {
    try {
        const nlohmann::json manifest = read_json_file(path.string() + ".complete.json");
        const std::string digest = manifest.at("annotation_sha256").get<std::string>();
        return load_normalized_annotation_index(path, source, split, digest, cancel_requested, trace);
    } catch (const std::exception&) {
        throw_if_benchmark_cancelled(cancel_requested);
        return std::nullopt;
    }
}
[[nodiscard]] NormalizedAnnotationIndex load_or_build_index(const BenchmarkCacheLayout& cache, const std::filesystem::path& path,
                                                            const BenchmarkDatasetSource source, const std::string_view split,
                                                            const std::string_view annotation_sha256,
                                                            mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                            const BenchmarkTraceSink& trace, const std::function<NormalizedAnnotationIndex()>& builder) {
    ArtifactLease lease =
        ArtifactLease::acquire(cache.locks / (std::string(benchmark_source_name(source)) + "-" + std::string(split) + ".index.lock"), cancel_requested);
    if (auto cached = load_normalized_annotation_index(path, source, split, annotation_sha256, cancel_requested, trace)) { return std::move(*cached); }
    NormalizedAnnotationIndex index = builder();
    store_normalized_annotation_index(path, index, cancel_requested, trace);
    return index;
}
// Runs an annotation indexing step with up to three attempts, invoking repair (cache invalidation
// plus artifact re-download) between attempts. Cancellation always rethrows immediately.
void retry_annotation_indexing(mmltk::common::concurrency::CancellationObservation cancel_requested, const std::function<void()>& body,
                               const std::function<void(const std::exception&)>& repair) {
    for (std::uint32_t attempt = 1U; attempt <= 3U; ++attempt) {
        try {
            body();
            break;
        } catch (const std::exception& error) {
            throw_if_benchmark_cancelled(cancel_requested);
            if (attempt == 3U) { throw; }
            repair(error);
        }
    }
}
struct SourceCompileCount {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::uint64_t selected_images = 0U;
    std::uint64_t compiled_images = 0U;
    std::uint64_t compiled_boxes = 0U;
};
struct PlannedBenchmarkInstance {
    PackedInstance label{};
    std::vector<RLEPair> mask_rle;
};
// Per-image duplicate rejection orders and compares planned labels by the same identity, so the
// projection is handed to the range algorithms directly instead of through comparator wrappers.
[[nodiscard]] constexpr auto planned_label_identity(const PlannedBenchmarkInstance& instance) noexcept {
    return std::tuple{instance.label.class_id, instance.label.bbox_x1, instance.label.bbox_y1, instance.label.bbox_x2, instance.label.bbox_y2};
}
SourceCompileCount append_source_plan(const NormalizedAnnotationIndex& index, const std::vector<CachedImageDirectory>& directories,
                                      const std::vector<std::uint64_t>* unavailable_image_ids, const std::uint32_t resolution, const bool require_every_image,
                                      PreparedBenchmarkSplit* split, std::uint64_t* dropped_boxes,
                                      mmltk::common::concurrency::CancellationObservation cancel_requested) {
    if (directories.empty()) { throw std::runtime_error("benchmark source has no cached image directories"); }
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
    if (index.source == BenchmarkDatasetSource::kObjects365V2) {
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
        std::size_t local_source = 0U;
        if (index.source == BenchmarkDatasetSource::kObjects365V2) {
            local_source = objects_shards[image.source_shard];
            if (local_source == std::numeric_limits<std::size_t>::max()) { throw std::runtime_error("Objects365 image references an unacquired shard"); }
        }
        image_labels.clear();
        image_labels.reserve(image.box_count);
        const mmltk::backend::imaging::resample::RgbLetterbox letterbox = mmltk::backend::imaging::resample::compute_rgb_letterbox(image.width, image.height, resolution, resolution);
        for (std::uint64_t box_index = image.first_box; box_index < image.first_box + image.box_count; ++box_index) {
            const NormalizedBox& box = index.boxes[common_math::checked_cast<std::size_t>(box_index, "box index overflow")];
            PackedInstance label = benchmark_letterbox_box(box.class_id, box.x1, box.y1, box.x2, box.y2, letterbox);
            if (label.bbox_x2 <= label.bbox_x1 || label.bbox_y2 <= label.bbox_y1) {
                ++*dropped_boxes;
                continue;
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
        std::ranges::sort(image_labels, std::ranges::less{}, planned_label_identity);
        const auto unique_end = std::ranges::unique(image_labels, std::ranges::equal_to{}, planned_label_identity).begin();
        *dropped_boxes += static_cast<std::uint64_t>(image_labels.end() - unique_end);
        image_labels.erase(unique_end, image_labels.end());
        if (image_labels.empty() && !require_every_image) { continue; }
        const std::uint32_t first_label = common_math::checked_cast<std::uint32_t>(split->labels.size(), "benchmark label index overflow");
        split->images.push_back(EncodedImageRecord{
            image.source_image_id,
            image.width,
            image.height,
            first_label,
            common_math::checked_cast<std::uint16_t>(image_labels.size(), "per-image label count overflow"),
            common_math::checked_cast<std::uint16_t>(source_base + local_source, "benchmark cached source index overflow"),
        });
        for (PlannedBenchmarkInstance& instance : image_labels) {
            instance.label.mask_rle_offset = common_math::checked_cast<std::uint32_t>(
                common_math::checked_multiply(split->rle_pairs.size(), sizeof(RLEPair), "benchmark mask offset overflow"), "benchmark mask offset overflow");
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
    progress->activity("Decoding, letterboxing, and resizing " + request.split.name + " JPEG pixels");
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
[[nodiscard]] nlohmann::json reject_json(const AnnotationRejectCounts& rejected) {
    return nlohmann::json{{"raw_records", rejected.raw_records},           {"unmapped_categories", rejected.unmapped_categories},
                          {"unknown_images", rejected.unknown_images},     {"malformed_records", rejected.malformed_records},
                          {"degenerate_boxes", rejected.degenerate_boxes}, {"duplicate_boxes", rejected.duplicate_boxes}};
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
[[nodiscard]] nlohmann::json source_catalog_manifest() {
    nlohmann::json artifacts = nlohmann::json::array();
    const auto append = [&](const CatalogArtifact& artifact) {
        nlohmann::json record{
            {"artifact_id", artifact.artifact_id}, {"url", artifact.url}, {"filename", artifact.filename}, {"expected_size", artifact.expected_size}};
        if (!artifact.expected_sha256.empty()) { record["expected_sha256"] = artifact.expected_sha256; }
        artifacts.push_back(std::move(record));
    };
    append(coco_annotations_artifact());
    append(coco_train_images_artifact());
    append(coco_val_images_artifact());
    append(objects365_annotations_artifact());
    for (const CatalogArtifact& artifact : objects365_train_image_artifacts()) { append(artifact); }
    append(open_images_boxes_artifact());
    append(open_images_classes_artifact());
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
    facts["resampling"] = {{"perceptual_downscale", config.perceptual_downscale}, {"version", 1U}};
    facts["catalog_revision"] = kBenchmarkCatalogRevision;
    facts["mapping_revision"] = kBenchmarkMappingRevision;
    facts["resolution"] = config.resolution;
    facts["cache_root"] = cache_root.string();
    facts["source_catalog"] = source_catalog_manifest();
    facts["mappings"] = mapping_manifest();
    write_json_atomically(staging_dir / "benchmark_manifest.json", facts, cancelled);
}
}  // namespace benchmark_internal
void compile_benchmark_dataset(BenchmarkCompilerConfig config) {
    using namespace benchmark_internal;
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
    ProgressReporter progress(std::move(config.progress), trace);
    progress.phase(DatasetCompilePhase::Planning);
    progress.activity("Resolving benchmark output path");
    config.output_dir = std::filesystem::absolute(config.output_dir).lexically_normal();
    if (config.output_dir == config.output_dir.root_path()) { throw std::runtime_error("benchmark output directory must not be the filesystem root"); }
    const std::filesystem::path output_parent = config.output_dir.parent_path().empty() ? std::filesystem::path{"."} : config.output_dir.parent_path();
    progress.activity("Preparing benchmark output directory");
    std::filesystem::create_directories(output_parent);
    progress.activity("Preparing benchmark cache");
    const std::filesystem::path cache_root = config.cache_dir.empty() ? std::filesystem::path{"./.cache/benchmark-dataset/v1"} : config.cache_dir;
    const std::filesystem::path normalized_cache_root = std::filesystem::weakly_canonical(std::filesystem::absolute(cache_root));
    const std::filesystem::path normalized_output = std::filesystem::weakly_canonical(config.output_dir);
    if (path_contains(normalized_cache_root, normalized_output) || path_contains(normalized_output, normalized_cache_root)) {
        throw std::runtime_error("benchmark output and cache directories must not overlap");
    }
    const BenchmarkCacheLayout cache = BenchmarkCacheLayout::create(normalized_cache_root);
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
    const std::filesystem::path coco_train_index_path = cache.source_indexes("coco") / "train2017.normalized.bin";
    const std::filesystem::path coco_val_index_path = cache.source_indexes("coco") / "val2017.normalized.bin";
    const std::filesystem::path objects_index_path = cache.source_indexes("objects365") / "train.normalized.bin";
    const std::filesystem::path open_images_index_path = cache.source_indexes("open-images") / "train.normalized.bin";
    progress.activity("Waiting for annotation cache locks");
    ArtifactLease coco_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "coco-annotations.lifecycle.lock", cancel_requested);
    ArtifactLease objects_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "objects365-annotations.lifecycle.lock", cancel_requested);
    ArtifactLease open_images_annotation_lifecycle = ArtifactLease::acquire(cache.locks / "open-images-annotations.lifecycle.lock", cancel_requested);
    constexpr std::uint64_t kIndexCount = 6U;
    progress.phase(DatasetCompilePhase::Indexing, 0U, kIndexCount);
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO train annotation index");
    std::optional<NormalizedAnnotationIndex> coco_train =
        discover_cached_index(coco_train_index_path, BenchmarkDatasetSource::kCoco2017, "train2017", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Validating cached COCO validation annotation index");
    std::optional<NormalizedAnnotationIndex> coco_val =
        discover_cached_index(coco_val_index_path, BenchmarkDatasetSource::kCoco2017, "val2017", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Validating cached Objects365 annotation index");
    std::optional<NormalizedAnnotationIndex> objects =
        discover_cached_index(objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", cancel_requested, trace);
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Validating cached Open Images annotation index");
    std::optional<NormalizedAnnotationIndex> open_images =
        discover_cached_index(open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", cancel_requested, trace);
    const bool coco_indexes_cache_hit = coco_train && coco_val;
    const bool objects_index_cache_hit = objects.has_value();
    const bool open_images_index_cache_hit = open_images.has_value();
    std::uint64_t completed_indexes = static_cast<std::uint64_t>(coco_train.has_value()) + static_cast<std::uint64_t>(coco_val.has_value()) +
                                      static_cast<std::uint64_t>(objects.has_value()) + static_cast<std::uint64_t>(open_images.has_value());
    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    std::vector<DownloadRequest> annotation_requests;
    std::optional<DownloadRequest> coco_annotation_request;
    std::optional<DownloadRequest> objects_annotation_request;
    std::optional<DownloadRequest> open_images_boxes_request;
    std::optional<DownloadRequest> open_images_classes_request;
    const auto annotation_parse_options = [&](const BenchmarkDatasetSource source, std::string split, const std::uint32_t expected_image_count,
                                              const bool keep_images_without_mapped_boxes) {
        return AnnotationParseOptions{
            source, std::move(split), expected_image_count, config.num_workers, keep_images_without_mapped_boxes, cancel_requested, trace,
        };
    };
    if (!coco_train || !coco_val) {
        coco_annotation_request = make_download_request(cache, "coco", coco_annotations_artifact());
        annotation_requests.push_back(*coco_annotation_request);
    }
    if (!objects) {
        objects_annotation_request = make_download_request(cache, "objects365", objects365_annotations_artifact());
        annotation_requests.push_back(*objects_annotation_request);
    }
    if (!open_images) {
        open_images_boxes_request = make_download_request(cache, "open-images", open_images_boxes_artifact());
        open_images_classes_request = make_download_request(cache, "open-images", open_images_classes_artifact());
        annotation_requests.push_back(*open_images_boxes_request);
        annotation_requests.push_back(*open_images_classes_request);
    }
    std::unordered_map<std::string, DownloadResult> annotation_downloads;
    // The initial annotation fetch and the post-validation repair differ only in their concurrency
    // budget and progress sink; downloading and indexing results by artifact id happens here once.
    const auto fetch_annotation_artifacts = [&](const std::vector<DownloadRequest>& requests, const std::size_t workers, ArtifactProgressTotals* totals) {
        const std::vector<DownloadResult> downloads =
            download_artifacts(requests, workers, cancel_requested, [&](const DownloadProgress& update) { totals->update(update, &progress); }, trace);
        for (std::size_t index = 0U; index < downloads.size(); ++index) {
            annotation_downloads.insert_or_assign(requests[index].artifact_id, downloads[index]);
        }
    };
    if (!annotation_requests.empty()) {
        ArtifactProgressTotals transfer_progress;
        std::uint64_t annotation_storage = 0U;
        bool needs_archive_scratch = false;
        for (const DownloadRequest& request : annotation_requests) {
            annotation_storage = common_math::checked_add(annotation_storage, request.expected_size, "benchmark annotation storage estimate overflow");
            needs_archive_scratch = needs_archive_scratch || request.artifact_id == coco_annotations_artifact().artifact_id ||
                                    request.artifact_id == objects365_annotations_artifact().artifact_id;
        }
        if (needs_archive_scratch) {
            annotation_storage = common_math::checked_add(annotation_storage, kArchiveScratchBytes, "benchmark annotation storage estimate overflow");
        }
        require_storage(cache.root, annotation_storage, "benchmark annotation acquisition and indexing", trace);
        progress.phase(DatasetCompilePhase::Downloading);
        if (!coco_train || !coco_val) { progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Downloading COCO annotation metadata"); }
        if (!objects) { progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Downloading Objects365 annotation metadata"); }
        if (!open_images) { progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Downloading Open Images annotation metadata"); }
        fetch_annotation_artifacts(annotation_requests, std::min<std::size_t>(3U, effective_num_workers), &transfer_progress);
        progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
    }
    ArtifactProgressTotals annotation_repair_progress;
    const auto repair_annotation_artifacts = [&](const BenchmarkDatasetSource source, const std::vector<DownloadRequest>& requests,
                                                 const std::string_view reason) {
        for (const DownloadRequest& request : requests) {
            if (std::filesystem::is_regular_file(request.destination)) {
                progress.source_activity(source, "Failure-only SHA-256 diagnosis for " + request.artifact_id);
                const std::string failure_sha256 =
                    mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(request.destination, [&] { return cancel_requested.requested(); }));
                trace_benchmark_event(trace, "benchmark.download.failure_sha256",
                                      [&] { return nlohmann::json{{"artifact", request.artifact_id}, {"sha256", failure_sha256}, {"reason", reason}}; });
            }
            invalidate_download_artifact(request, cancel_requested, trace);
        }
        progress.source_activity(source,
                                 "Redownloading annotation metadata after structural "
                                 "validation failure");
        fetch_annotation_artifacts(
            requests,
            requests.size() == 1U ? std::min<std::size_t>(8U, effective_num_workers) : std::min<std::size_t>({3U, requests.size(), effective_num_workers}),
            &annotation_repair_progress);
    };
    if (!coco_train || !coco_val) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& archive = annotation_downloads.at(coco_annotations_artifact().artifact_id);
                const std::filesystem::path extracted_dir = cache.source_indexes("coco") / "source-json";
                std::filesystem::create_directories(extracted_dir);
                if (!coco_train) {
                    const std::filesystem::path json_path = extracted_dir / "instances_train2017.json";
                    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Extracting COCO train annotations");
                    const std::string digest = extract_archive_member(archive.path, "annotations/instances_train2017.json", json_path, archive.identity,
                                                                      cache.locks / "coco-train-json.extract.lock", cancel_requested, trace);
                    coco_train =
                        load_or_build_index(cache, coco_train_index_path, BenchmarkDatasetSource::kCoco2017, "train2017", digest, cancel_requested, trace, [&] {
                            progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO train annotations");
                            return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
                                                                annotation_parse_options(BenchmarkDatasetSource::kCoco2017, "train2017", 118287U, false));
                        });
                    ++completed_indexes;
                    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
                }
                if (!coco_val) {
                    const std::filesystem::path json_path = extracted_dir / "instances_val2017.json";
                    progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Extracting COCO validation annotations");
                    const std::string digest = extract_archive_member(archive.path, "annotations/instances_val2017.json", json_path, archive.identity,
                                                                      cache.locks / "coco-val-json.extract.lock", cancel_requested, trace);
                    coco_val =
                        load_or_build_index(cache, coco_val_index_path, BenchmarkDatasetSource::kCoco2017, "val2017", digest, cancel_requested, trace, [&] {
                            progress.source_activity(BenchmarkDatasetSource::kCoco2017, "Parsing and indexing COCO validation annotations");
                            return parse_coco_style_annotations(json_path, digest, coco_category_mappings(),
                                                                annotation_parse_options(BenchmarkDatasetSource::kCoco2017, "val2017", 5000U, true));
                        });
                    ++completed_indexes;
                    progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
                }
            },
            [&](const std::exception& error) {
                const std::filesystem::path extracted_dir = cache.source_indexes("coco") / "source-json";
                if (!coco_train) {
                    remove_cache_path(extracted_dir / "instances_train2017.json.extract.json");
                    remove_cache_path(extracted_dir / "instances_train2017.json");
                    remove_cache_path(coco_train_index_path.string() + ".complete.json");
                    remove_cache_path(coco_train_index_path);
                }
                if (!coco_val) {
                    remove_cache_path(extracted_dir / "instances_val2017.json.extract.json");
                    remove_cache_path(extracted_dir / "instances_val2017.json");
                    remove_cache_path(coco_val_index_path.string() + ".complete.json");
                    remove_cache_path(coco_val_index_path);
                }
                repair_annotation_artifacts(BenchmarkDatasetSource::kCoco2017, {*coco_annotation_request}, error.what());
            });
    }
    if (!objects) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& annotation_archive = annotation_downloads.at(objects365_annotations_artifact().artifact_id);
                const std::filesystem::path extracted_dir = cache.source_indexes("objects365") / "source-json";
                std::filesystem::create_directories(extracted_dir);
                const std::filesystem::path json_path = extracted_dir / "zhiyuan_objv2_train.json";
                progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Extracting Objects365 train annotations");
                const std::string annotation_digest =
                    extract_archive_member(annotation_archive.path, "zhiyuan_objv2_train.json", json_path, annotation_archive.identity,
                                           cache.locks / "objects365-train-json.extract.lock", cancel_requested, trace);
                objects = load_or_build_index(
                    cache, objects_index_path, BenchmarkDatasetSource::kObjects365V2, "train", annotation_digest, cancel_requested, trace, [&] {
                        progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Parsing and indexing Objects365 annotations");
                        return parse_coco_style_annotations(json_path, annotation_digest, objects365_category_mappings(),
                                                            annotation_parse_options(BenchmarkDatasetSource::kObjects365V2, "train", 0U, false));
                    });
                ++completed_indexes;
                progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
            },
            [&](const std::exception& error) {
                const std::filesystem::path json_path = cache.source_indexes("objects365") / "source-json" / "zhiyuan_objv2_train.json";
                remove_cache_path(json_path.string() + ".extract.json");
                remove_cache_path(json_path);
                remove_cache_path(objects_index_path.string() + ".complete.json");
                remove_cache_path(objects_index_path);
                repair_annotation_artifacts(BenchmarkDatasetSource::kObjects365V2, {*objects_annotation_request}, error.what());
            });
    }
    if (!open_images) {
        retry_annotation_indexing(
            cancel_requested,
            [&] {
                const DownloadResult& boxes = annotation_downloads.at(open_images_boxes_artifact().artifact_id);
                const DownloadResult& classes = annotation_downloads.at(open_images_classes_artifact().artifact_id);
                const std::string annotation_identity = combined_artifact_digest(boxes.identity, classes.identity);
                open_images = load_or_build_index(
                    cache, open_images_index_path, BenchmarkDatasetSource::kOpenImagesV7, "train", annotation_identity, cancel_requested, trace, [&] {
                        progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Parsing and indexing Open Images annotations");
                        return parse_open_images_annotations(boxes.path, classes.path, annotation_identity, open_images_category_mappings(),
                                                             annotation_parse_options(BenchmarkDatasetSource::kOpenImagesV7, "train", 0U, false));
                    });
                ++completed_indexes;
                progress.phase(DatasetCompilePhase::Indexing, completed_indexes, kIndexCount);
            },
            [&](const std::exception& error) {
                remove_cache_path(open_images_index_path.string() + ".complete.json");
                remove_cache_path(open_images_index_path);
                repair_annotation_artifacts(BenchmarkDatasetSource::kOpenImagesV7, {*open_images_boxes_request, *open_images_classes_request}, error.what());
            });
    }
    if (!coco_train || !coco_val || !objects || !open_images) { throw std::runtime_error("benchmark normalized annotation indexing did not complete"); }
    if (coco_val->images.size() != 5000U) { throw std::runtime_error("COCO val2017 normalized index must contain exactly 5,000 images"); }
    {
        progress.activity("Checking COCO train and validation metadata reuse");
        std::unordered_set<std::uint64_t> validation_ids;
        validation_ids.reserve(coco_val->images.size());
        for (const NormalizedImage& image : coco_val->images) { validation_ids.emplace(image.source_image_id); }
        for (const NormalizedImage& image : coco_train->images) {
            if (validation_ids.contains(image.source_image_id)) { throw std::runtime_error("COCO train and validation metadata reuse an image ID"); }
        }
        trace_benchmark_event(trace, "benchmark.split_reuse.metadata_check", [&] {
            return nlohmann::json{{"coco_train_images", coco_train->images.size()}, {"coco_val_images", coco_val->images.size()}, {"overlap", 0}};
        });
    }
    progress.source_activity(BenchmarkDatasetSource::kObjects365V2, "Selecting byte-efficient Objects365 shards and balanced images");
    const std::vector<CatalogArtifact> sampling_object_artifacts = objects365_train_image_artifacts();
    std::vector<std::uint64_t> object_shard_bytes;
    object_shard_bytes.reserve(sampling_object_artifacts.size());
    for (const CatalogArtifact& artifact : sampling_object_artifacts) { object_shard_bytes.push_back(artifact.expected_size); }
    CombinedSupplementalSamplingResult combined_sampling =
        sample_combined_supplemental_indices(*coco_train, *objects, *open_images, object_shard_bytes, cancel_requested);
    SupplementalSamplingResult objects_sampling = std::move(combined_sampling.objects365);
    SupplementalSamplingResult open_images_sampling = std::move(combined_sampling.open_images);
    *objects = std::move(objects_sampling.index);
    progress.phase(DatasetCompilePhase::Indexing, ++completed_indexes, kIndexCount);
    progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Selecting Open Images class-deficit and diversity sample");
    *open_images = std::move(open_images_sampling.index);
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
        return nlohmann::json{{"target_images", combined_sampling.target_images},
                              {"objects365_images", objects_sampling.stats.selected_images},
                              {"open_images_images", open_images_sampling.stats.selected_images},
                              {"open_images_floor", combined_sampling.open_images_floor},
                              {"open_images_ceiling", combined_sampling.open_images_ceiling},
                              {"objects365_shards", combined_sampling.objects365_shards},
                              {"objects365_archive_bytes", combined_sampling.objects365_archive_bytes}};
    });
    progress.activity("Finalizing normalized annotation cache");
    open_images_annotation_lifecycle = ArtifactLease{};
    objects_annotation_lifecycle = ArtifactLease{};
    coco_annotation_lifecycle = ArtifactLease{};
    const std::uint64_t selected_train_images =
        common_math::checked_add(common_math::checked_add(coco_train->images.size(), objects->images.size(), "benchmark selected image count overflow"),
                                 open_images->images.size(), "benchmark selected image count overflow");
    const std::uint64_t selected_total_images =
        common_math::checked_add(selected_train_images, coco_val->images.size(), "benchmark selected image count overflow");
    const std::uint64_t selected_train_labels =
        common_math::checked_add(common_math::checked_add(coco_train->boxes.size(), objects->boxes.size(), "benchmark selected label count overflow"),
                                 open_images->boxes.size(), "benchmark selected label count overflow");
    const std::uint64_t projected_output_upper_bound =
        common_math::checked_add(estimate_output_bytes(selected_train_images, selected_train_labels, config.resolution, coco_train->mask_rle_pairs.size()),
                                 estimate_output_bytes(coco_val->images.size(), coco_val->boxes.size(), config.resolution, coco_val->mask_rle_pairs.size()),
                                 "benchmark projected output size overflow");
    std::vector<std::uint64_t> archive_sizes{coco_train_images_artifact().expected_size, coco_val_images_artifact().expected_size};
    for (const std::uint16_t shard : combined_sampling.objects365_shards) { archive_sizes.push_back(sampling_object_artifacts.at(shard).expected_size); }
    std::uint64_t retained_archive_bytes = 0U;
    for (const std::uint64_t archive_size : archive_sizes) {
        retained_archive_bytes = common_math::checked_add(retained_archive_bytes, archive_size, "benchmark archive cache estimate overflow");
    }
    const std::uint64_t projected_cache =
        common_math::checked_add(common_math::checked_multiply(selected_total_images, kEstimatedJpegBytes, "benchmark cache estimate overflow"),
                                 retained_archive_bytes, "benchmark cache estimate overflow");
    trace_benchmark_event(trace, "benchmark.storage.projection", [&] {
        return nlohmann::json{{"cache_bytes_upper_bound", projected_cache},
                              {"compiled_output_bytes_upper_bound", projected_output_upper_bound},
                              {"selected_images", selected_total_images}};
    });
    progress.projected(projected_output_upper_bound);
    progress.activity("Checking projected benchmark storage");
    require_storage(cache.root, projected_cache, "benchmark image cache and active archives", trace);
    require_storage(output_parent, projected_output_upper_bound, "benchmark compiled output staging upper bound", trace);
    progress.phase(DatasetCompilePhase::Extracting);
    ArtifactProgressTotals image_transfer_progress;
    StorageReservationPool cache_storage_reservations(cache.root, trace);
    std::vector<QuarantinedImage> quarantined;
    const std::vector<std::uint64_t> coco_train_ids = image_ids(*coco_train);
    const std::vector<std::uint64_t> coco_val_ids = image_ids(*coco_val);
    const std::uint64_t coco_selected_images = common_math::checked_add(coco_train_ids.size(), coco_val_ids.size(), "COCO selected image count overflow");
    const std::vector<CatalogArtifact> object_artifacts = objects365_train_image_artifacts();
    struct ArchiveTask {
        BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
        std::string shard;
        std::vector<std::uint64_t> image_ids;
        CatalogArtifact artifact;
    };
    std::vector<ArchiveTask> archive_tasks;
    archive_tasks.reserve(object_artifacts.size() + 2U);
    archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "train2017", coco_train_ids, coco_train_images_artifact()});
    archive_tasks.push_back(ArchiveTask{BenchmarkDatasetSource::kCoco2017, "val2017", coco_val_ids, coco_val_images_artifact()});
    for (std::size_t shard = 0U; shard < object_artifacts.size(); ++shard) {
        const std::uint16_t numeric_shard = common_math::checked_cast<std::uint16_t>(shard, "Objects365 shard index overflow");
        std::vector<std::uint64_t> ids = image_ids(*objects, numeric_shard);
        if (!ids.empty()) {
            archive_tasks.push_back(
                ArchiveTask{BenchmarkDatasetSource::kObjects365V2, "patch-" + std::to_string(shard), std::move(ids), object_artifacts[shard]});
        }
    }
    std::vector<std::optional<CachedImageDirectory>> archive_results(archive_tasks.size());
    std::atomic<std::uint64_t> coco_extracted{0U};
    std::atomic<std::uint64_t> objects_completed{0U};
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
    const bool overlap_open_images = configured_workers >= 5U;
    const std::size_t open_cache_workers = configured_workers == 1U ? 0U : std::min<std::size_t>(4U, std::max<std::size_t>(1U, configured_workers / 8U));
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
    progress.activity(
        "Acquisition worker budget: " + std::to_string(archive_workers) + " archive controllers, " + std::to_string(decompression_workers) +
        " decompression workers and " +
        (archive_cache_workers == 0U ? std::string{"inline cache writes per archive, "}
                                     : std::to_string(archive_cache_workers) + " cache writers per archive, ") +
        (open_cache_workers == 0U ? std::string{"inline Open Images cache writes"} : std::to_string(open_cache_workers) + " Open Images cache writers") + ", " +
        std::to_string(archive_download_connections) + " download connections per archive");
    std::thread open_images_thread;
    if (overlap_open_images) {
        open_images_thread = std::thread([&] {
            try {
                open_image_result =
                    acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace);
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
                    std::atomic<std::uint64_t>* source_completed = task.source == BenchmarkDatasetSource::kCoco2017 ? &coco_extracted : &objects_completed;
                    const std::uint64_t source_total = task.source == BenchmarkDatasetSource::kCoco2017 ? coco_selected_images : objects->images.size();
                    try {
                        archive_results[task_index] =
                            acquire_archive_images(cache, task.source, task.shard, task.image_ids, task.artifact, cancel_requested, &progress,
                                                   &image_transfer_progress, &cache_storage_reservations, source_completed, source_total, decompression_workers,
                                                   archive_cache_workers, archive_download_connections, trace);
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
            open_image_result =
                acquire_open_images(cache, *open_images, &quarantined, cancel_requested, &progress, acquisition_num_workers, open_cache_workers, trace);
        } catch (...) { record_pipeline_error(); }
    }
    if (open_images_thread.joinable()) { open_images_thread.join(); }
    if (pipeline_error) { std::rethrow_exception(pipeline_error); }
    if (!open_image_result) { throw std::runtime_error("Open Images acquisition did not complete"); }
    std::vector<CachedImageDirectory> coco_train_images;
    std::vector<CachedImageDirectory> coco_val_images;
    std::vector<CachedImageDirectory> object_images;
    std::vector<CachedImageDirectory> open_image_directories;
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
    refresh_archive_availability();
    progress.source_images(BenchmarkDatasetSource::kCoco2017, coco_selected_images, coco_selected_images);
    progress.source_complete(BenchmarkDatasetSource::kCoco2017,
                             coco_indexes_cache_hit && coco_train_images.front().cache_hit && coco_val_images.front().cache_hit);
    progress.source_complete(
        BenchmarkDatasetSource::kObjects365V2,
        objects_index_cache_hit && std::ranges::all_of(object_images, [](const CachedImageDirectory& directory) { return directory.cache_hit; }));
    progress.source_complete(BenchmarkDatasetSource::kOpenImagesV7, open_images_index_cache_hit && open_image_result->directory.cache_hit);
    constexpr std::uint64_t kLabelPlanCount = 4U;
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
        prepared_counts.push_back(append_source_plan(*coco_train, coco_train_images, coco_train_unavailable_ids.empty() ? nullptr : &coco_train_unavailable_ids,
                                                     config.resolution, false, &prepared, &prepared_dropped_boxes, cancel_requested));
        if (report_progress) {
            progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
            progress.activity("Preparing Objects365 training labels");
        }
        prepared_counts.push_back(append_source_plan(*objects, object_images, objects_unavailable_ids.empty() ? nullptr : &objects_unavailable_ids,
                                                     config.resolution, false, &prepared, &prepared_dropped_boxes, cancel_requested));
        if (report_progress) {
            progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
            progress.activity("Preparing Open Images training labels");
        }
        prepared_counts.push_back(append_source_plan(*open_images, open_image_directories,
                                                     open_images_unavailable_ids.empty() ? nullptr : &open_images_unavailable_ids, config.resolution, false,
                                                     &prepared, &prepared_dropped_boxes, cancel_requested));
        if (report_progress) { progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount); }
        train = std::move(prepared);
        source_counts = std::move(prepared_counts);
        training_target_dropped_boxes = prepared_dropped_boxes;
    };
    prepare_training_plan(true);
    progress.activity("Preparing COCO validation labels");
    const SourceCompileCount validation_count =
        append_source_plan(*coco_val, coco_val_images, nullptr, config.resolution, true, &validation, &validation_target_dropped_boxes, cancel_requested);
    progress.phase(DatasetCompilePhase::Labels, ++completed_label_plans, kLabelPlanCount);
    if (validation.images.size() != 5000U) { throw std::runtime_error("compiled COCO validation plan must contain exactly 5,000 images"); }
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
    std::uint64_t total_compile_images = common_math::checked_add(train.images.size(), validation.images.size(), "benchmark compile image count overflow");
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
        const JpegDecodeProbe decode_probe{error.source_image_id(), failed_record->source_width, failed_record->source_height};
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
        if (source_root == open_image_result->directory.path) {
            progress.source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Repairing failed Open Images JPEG " + std::to_string(error.source_image_id()));
            std::vector<QuarantinedImage> repaired_quarantined;
            AcquiredOpenImages repaired = acquire_open_images(cache, *open_images, &repaired_quarantined, cancel_requested, &progress, acquisition_num_workers,
                                                              open_cache_workers, trace, decode_probe);
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
        std::atomic<std::uint64_t> repaired_images{0U};
        CachedImageDirectory repaired = acquire_archive_images(cache, task->source, task->shard, task->image_ids, task->artifact, cancel_requested, &progress,
                                                               &repair_transfer_progress, &cache_storage_reservations, &repaired_images, task->image_ids.size(),
                                                               decompression_workers, archive_cache_workers, archive_download_connections, trace, decode_probe);
        if (repaired.image_count + repaired.quarantined.size() != task->image_ids.size()) {
            throw std::runtime_error(
                "archive repair did not resolve every selected "
                "image");
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
                    return nlohmann::json{{"split", request.split.name}, {"image_id", error.source_image_id()}, {"attempt", attempt}, {"reason", error.what()}};
                });
                repair_cached_image(request.split, error);
                progress.activity("Retrying " + request.split.name +
                                  " pixel compilation from zero after bounded cache "
                                  "repair");
            }
        }
    };
    compile_split(
        BenchmarkWriteRequest{
            train, staging_dir / "train.bin", config.resolution, config.num_workers, {}, false, cancel_requested, {}, config.perceptual_downscale},
        0U);
    compile_split(
        BenchmarkWriteRequest{
            validation, staging_dir / "val.bin", config.resolution, config.num_workers, {}, false, cancel_requested, {}, config.perceptual_downscale},
        train.images.size());
    progress.activity("Finalizing rejected and quarantined records");
    std::ranges::sort(quarantined);
    quarantined.erase(
        std::unique(quarantined.begin(), quarantined.end(),
                    [](const QuarantinedImage& left, const QuarantinedImage& right) { return left.source == right.source && left.image_id == right.image_id; }),
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
    add_annotation_drops(*coco_train);
    add_annotation_drops(*coco_val);
    add_annotation_drops(*objects);
    add_annotation_drops(*open_images);
    const std::uint64_t target_dropped_boxes =
        common_math::checked_add(training_target_dropped_boxes, validation_target_dropped_boxes, "benchmark target annotation count overflow");
    progress.rejected(common_math::checked_add(annotation_drops, target_dropped_boxes, "benchmark rejected annotation count overflow"), quarantined.size());
    constexpr std::uint64_t kSyncStepCount = 4U;
    progress.phase(DatasetCompilePhase::Syncing, 0U, kSyncStepCount);
    progress.activity("Inspecting staged training dataset");
    const CompiledDatasetInfo train_info = inspect_compiled_dataset(staging_dir / "train.bin");
    progress.phase(DatasetCompilePhase::Syncing, 1U, kSyncStepCount);
    progress.activity("Inspecting staged validation dataset");
    const CompiledDatasetInfo val_info = inspect_compiled_dataset(staging_dir / "val.bin");
    progress.phase(DatasetCompilePhase::Syncing, 2U, kSyncStepCount);
    if (train_info.image_count != train.images.size() || val_info.image_count != 5000U || !std::ranges::equal(train_info.class_names(), train.class_names) ||
        !std::ranges::equal(val_info.class_names(), validation.class_names)) {
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
    append_source_manifest(*coco_train, source_counts[0], coco_annotations_artifact().url, coco_train_index_path, nullptr);
    append_source_manifest(*objects, source_counts[1], objects365_annotations_artifact().url, objects_index_path, &objects_sampling.stats);
    append_source_manifest(*open_images, source_counts[2], open_images_boxes_artifact().url, open_images_index_path, &open_images_sampling.stats);
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
    append_image_cache(coco_train_images);
    append_image_cache(coco_val_images);
    append_image_cache(object_images);
    append_image_cache(open_image_directories);
    manifest["cached_image_bytes"] = cached_image_bytes;
    manifest["compiled_bytes"] = common_math::checked_add(manifest["train"]["bytes"].get<std::uint64_t>(), manifest["val"]["bytes"].get<std::uint64_t>(),
                                                          "benchmark compiled byte total overflow");
    for (const QuarantinedImage& image : quarantined) {
        manifest["quarantined_images"].push_back({{"source", benchmark_source_name(image.source)}, {"image_id", image.image_id}, {"reason", image.reason}});
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
}
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
