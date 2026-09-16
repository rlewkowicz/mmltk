#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "src/backend/data/benchmark_dataset_compiler.h"
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/image_resize.h"
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
#include "benchmark_curl.h"
#include "benchmark_download.h"
#include "benchmark_images.h"
#include "benchmark_jpeg.h"
#include "benchmark_sampling.h"
#include "benchmark_writer.h"
#include "detail/benchmark_compiler.h"
#include "mask_rle_utils.h"
#include "worker_queue.h"
namespace mmltk::backend::data {
namespace common_concurrency = mmltk::common::concurrency;
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace common_system = mmltk::common::system;
namespace benchmark_internal {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kArchivePipelineConcurrency = 4U;
constexpr std::size_t kOpenImagesGroupImages = 4096U;
constexpr std::uint32_t kMaximumAttempts = 5U;
constexpr std::uint64_t kEstimatedJpegBytes = std::uint64_t{256U} * 1024U;
constexpr std::size_t kArchiveReadBufferBytes = std::size_t{1024U} * 1024U;
constexpr std::size_t kMaximumOpenImagesJpegBytes = std::size_t{8U} * 1024U * 1024U;
constexpr std::size_t kMaximumRetainedOpenImagesBufferBytes = std::size_t{2U} * 1024U * 1024U;
constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
struct QuarantinedImage {
    BenchmarkDatasetSource source = BenchmarkDatasetSource::kCoco2017;
    std::uint64_t image_id = 0U;
    std::string reason;
    // Manifest order is the declaration order of these fields; the type owns it so no call site
    // spells the comparison out again.
    auto operator<=>(const QuarantinedImage&) const = default;
};
class ProgressReporter {
   public:
    ProgressReporter(BenchmarkProgressCallback callback, const BenchmarkTraceSink& trace) : callback_(std::move(callback)), trace_(&trace) {
        if (!callback_) { return; }
        const auto source_progress = [](const BenchmarkDatasetSource source) {
            BenchmarkSourceProgress progress;
            progress.source = source;
            return progress;
        };
        state_.sources = {
            source_progress(BenchmarkDatasetSource::kCoco2017),
            source_progress(BenchmarkDatasetSource::kObjects365V2),
            source_progress(BenchmarkDatasetSource::kOpenImagesV7),
        };
    }
    void phase(const DatasetCompilePhase phase, const std::uint64_t completed = 0U, const std::uint64_t total = 0U) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        const bool phase_changed = state_.phase != phase;
        if (phase == DatasetCompilePhase::Extracting && state_.phase != DatasetCompilePhase::Extracting) {
            for (BenchmarkSourceProgress& source : state_.sources) {
                source.activity.clear();
                source.completed_bytes = 0U;
                source.total_bytes = 0U;
                source.completed_images = 0U;
                source.total_images = 0U;
                source.retry_count = 0U;
                source.cache_hit = false;
                source.resumed = false;
                source.complete = false;
            }
        }
        state_.phase = phase;
        state_.completed = completed;
        state_.total = total;
        if (phase_changed || state_.activity.empty()) { set_activity_unlocked(default_phase_activity(phase)); }
        emit();
    }
    void activity(std::string activity) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        set_activity_unlocked(std::move(activity));
        emit();
    }
    void pixel_attempt(const std::uint64_t completed, const std::uint64_t total, const std::string_view split, const std::uint64_t split_total) {
        if (!pixel_observer_enabled()) { return; }
        const std::lock_guard lock(mutex_);
        if (callback_ && state_.pixel_attempt == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("benchmark pixel attempt counter overflow");
        }
        if (callback_) {
            state_.phase = DatasetCompilePhase::Pixels;
            state_.completed = completed;
            state_.total = total;
            ++state_.pixel_attempt;
            state_.pixel_attempt_offset = completed;
            set_activity_unlocked(default_phase_activity(DatasetCompilePhase::Pixels));
        } else {
            activity_started_ = Clock::now();
        }
        pixel_completed_before_ = completed;
        pixel_split_ = split;
        pixel_split_total_ = split_total;
        pixel_completed_ = 0U;
        if (callback_) { emit(); }
    }
    void pixel_completed() {
        const std::lock_guard lock(mutex_);
        if (pixel_completed_ >= pixel_split_total_) { throw std::overflow_error("benchmark pixel completion exceeds its split total"); }
        ++pixel_completed_;
        constexpr std::uint64_t kPixelProgressQuantum = 64U;
        if ((pixel_completed_ % kPixelProgressQuantum) != 0U && pixel_completed_ != pixel_split_total_) { return; }
        if (callback_) {
            state_.completed = pixel_completed_before_ + pixel_completed_;
            state_.activity_elapsed_seconds =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - activity_started_).count());
            emit();
        }
        if (trace_ != nullptr && *trace_) {
            const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - activity_started_).count();
            const double images_per_second = elapsed_seconds > 0.0 ? static_cast<double>(pixel_completed_) / elapsed_seconds : 0.0;
            const double eta_seconds = images_per_second > 0.0 ? static_cast<double>(pixel_split_total_ - pixel_completed_) / images_per_second : 0.0;
            trace_benchmark_event(*trace_, "benchmark.pixel_compile.throughput", [&] {
                return nlohmann::json{{"split", pixel_split_},
                                      {"completed_images", pixel_completed_},
                                      {"total_images", pixel_split_total_},
                                      {"elapsed_seconds", elapsed_seconds},
                                      {"images_per_second", images_per_second},
                                      {"eta_seconds", eta_seconds}};
            });
        }
    }
    void source_activity(const BenchmarkDatasetSource source, std::string activity) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        BenchmarkSourceProgress& source_state = source_progress(source);
        source_state.activity = activity;
        set_activity_unlocked(std::move(activity));
        emit();
    }
    [[nodiscard]] bool pixel_observer_enabled() const noexcept { return callback_ || (trace_ != nullptr && static_cast<bool>(*trace_)); }
    void projected(const std::uint64_t bytes) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        state_.projected_output_bytes = bytes;
        emit();
    }
    void rejected(const std::uint64_t dropped, const std::uint64_t quarantined) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        state_.dropped_instances = dropped;
        state_.quarantined_images = quarantined;
        emit();
    }
    void source_bytes(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total, const std::uint32_t retry_count,
                      const bool cache_hit, const bool resumed) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        BenchmarkSourceProgress& progress = source_progress(source);
        progress.completed_bytes = completed;
        progress.total_bytes = total;
        progress.retry_count = retry_count;
        progress.cache_hit = cache_hit;
        progress.resumed = progress.resumed || resumed;
        update_source_phase_progress();
        emit();
    }
    void source_images(const BenchmarkDatasetSource source, const std::uint64_t completed, const std::uint64_t total) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        BenchmarkSourceProgress& progress = source_progress(source);
        progress.completed_images = completed;
        progress.total_images = total;
        update_source_phase_progress();
        emit();
    }
    void source_complete(const BenchmarkDatasetSource source, const bool cache_hit) {
        if (!callback_) { return; }
        const std::lock_guard lock(mutex_);
        BenchmarkSourceProgress& progress = source_progress(source);
        progress.complete = true;
        progress.cache_hit = cache_hit;
        progress.activity = cache_hit ? "Cache hit" : "Complete";
        update_source_phase_progress();
        emit();
    }

   private:
    // CLEANUP-IGNORE: exhaustive phase-to-activity descriptor; its switch shape intentionally follows the enum.
    [[nodiscard]] static std::string default_phase_activity(const DatasetCompilePhase phase) {
        switch (phase) {
            case DatasetCompilePhase::Idle: return "Compiling benchmark dataset";
            case DatasetCompilePhase::Planning: return "Planning benchmark dataset";
            case DatasetCompilePhase::Downloading: return "Acquiring benchmark metadata";
            case DatasetCompilePhase::Indexing: return "Indexing benchmark annotations";
            case DatasetCompilePhase::Extracting: return "Acquiring and extracting source images";
            case DatasetCompilePhase::Labels: return "Preparing compiled labels";
            case DatasetCompilePhase::Pixels: return "Compiling image pixels";
            case DatasetCompilePhase::Syncing: return "Validating and syncing compiled output";
            case DatasetCompilePhase::Publishing: return "Publishing benchmark dataset";
        }
        std::unreachable();
    }
    void set_activity_unlocked(std::string activity) {
        state_.activity = std::move(activity);
        state_.activity_elapsed_seconds = 0U;
        activity_started_ = Clock::now();
    }
    static void add_progress(std::uint64_t& target, const std::uint64_t value, const char* context) {
        if (value > std::numeric_limits<std::uint64_t>::max() - target) { throw std::overflow_error(context); }
        target += value;
    }
    BenchmarkSourceProgress& source_progress(const BenchmarkDatasetSource source) {
        const auto found = std::ranges::find(state_.sources, source, &BenchmarkSourceProgress::source);
        if (found == state_.sources.end()) { throw std::runtime_error("benchmark progress references an unknown source"); }
        return *found;
    }
    void update_source_phase_progress() {
        if (state_.phase == DatasetCompilePhase::Downloading) {
            state_.completed = 0U;
            state_.total = 0U;
            for (const BenchmarkSourceProgress& source : state_.sources) {
                add_progress(state_.completed, source.completed_bytes, "benchmark download progress overflow");
                add_progress(state_.total, source.total_bytes, "benchmark download progress overflow");
            }
            return;
        }
        if (state_.phase != DatasetCompilePhase::Extracting) { return; }
        constexpr std::uint64_t kSourceProgressScale = 1'000'000U;
        state_.completed = 0U;
        state_.total = 0U;
        for (const BenchmarkSourceProgress& source : state_.sources) {
            const bool use_images = source.total_images > 0U;
            const std::uint64_t completed = use_images ? source.completed_images : source.completed_bytes;
            const std::uint64_t total = use_images ? source.total_images : source.total_bytes;
            if (total == 0U && !source.complete) { continue; }
            const std::uint64_t scaled_completed =
                total == 0U ? kSourceProgressScale
                            : static_cast<std::uint64_t>(static_cast<long double>(std::min(completed, total)) * static_cast<long double>(kSourceProgressScale) /
                                                         static_cast<long double>(total));
            add_progress(state_.completed, scaled_completed, "benchmark extraction progress overflow");
            add_progress(state_.total, kSourceProgressScale, "benchmark extraction progress overflow");
        }
    }
    void emit() const {
        if (callback_) { callback_(state_); }
    }
    BenchmarkProgressCallback callback_;
    const BenchmarkTraceSink* trace_ = nullptr;
    BenchmarkCompileProgress state_;
    Clock::time_point activity_started_ = Clock::now();
    std::string_view pixel_split_;
    std::uint64_t pixel_split_total_ = 0U;
    std::uint64_t pixel_completed_ = 0U;
    std::uint64_t pixel_completed_before_ = 0U;
    std::mutex mutex_;
};
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
[[nodiscard]] std::uint64_t available_bytes(const std::filesystem::path& path) {
    std::filesystem::path probe = std::filesystem::absolute(path).lexically_normal();
    while (!std::filesystem::exists(probe)) {
        const std::filesystem::path parent = probe.parent_path();
        if (parent == probe || parent.empty()) { throw std::runtime_error("cannot locate an existing parent for storage preflight"); }
        probe = parent;
    }
    struct statvfs status{};
    if (::statvfs(probe.c_str(), &status) != 0) { throw common_io::errno_error("cannot inspect benchmark storage", probe.string()); }
    return common_math::checked_multiply(status.f_bavail, status.f_frsize, "available storage byte count overflow");
}
[[nodiscard]] bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto parent_iterator = parent.begin();
    auto child_iterator = child.begin();
    for (; parent_iterator != parent.end() && child_iterator != child.end(); ++parent_iterator, ++child_iterator) {
        if (*parent_iterator != *child_iterator) { return false; }
    }
    return parent_iterator == parent.end();
}
void require_storage(const std::filesystem::path& path, const std::uint64_t required, const char* description, const BenchmarkTraceSink& trace) {
    const std::uint64_t available = available_bytes(path);
    trace_benchmark_event(trace, "benchmark.storage.preflight", [&] {
        return nlohmann::json{{"target", description}, {"path", path.string()}, {"required_bytes", required}, {"available_bytes", available}};
    });
    if (available < required) {
        throw std::runtime_error(std::string("insufficient storage for ") + description + ": requires " + std::to_string(required) + " bytes, available " +
                                 std::to_string(available));
    }
}
class StorageReservationPool {
   public:
    class Reservation {
       public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&&) = delete;
        Reservation& operator=(Reservation&&) = delete;
        ~Reservation() { release(); }

       private:
        friend class StorageReservationPool;
        Reservation(StorageReservationPool* owner, const std::uint64_t bytes) : owner_(owner), bytes_(bytes) {}
        void release() noexcept {
            if (owner_ != nullptr) {
                owner_->release(bytes_);
                owner_ = nullptr;
                bytes_ = 0U;
            }
        }
        StorageReservationPool* owner_ = nullptr;
        std::uint64_t bytes_ = 0U;
    };
    StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace) : path_(std::move(path)), trace_(std::move(trace)) {}
    [[nodiscard]] Reservation reserve(const std::uint64_t required, const std::string_view description) {
        const std::lock_guard lock(mutex_);
        const std::uint64_t available = available_bytes(path_);
        if (reserved_ > available || required > available - reserved_) {
            throw std::runtime_error("insufficient storage for " + std::string(description) + ": requires " + std::to_string(required) + " bytes with " +
                                     std::to_string(reserved_) + " bytes already reserved, available " + std::to_string(available));
        }
        reserved_ += required;
        trace_benchmark_event(trace_, "benchmark.storage.reserved", [&] {
            return nlohmann::json{
                {"target", description}, {"path", path_.string()}, {"required_bytes", required}, {"reserved_bytes", reserved_}, {"available_bytes", available}};
        });
        return Reservation(this, required);
    }

   private:
    void release(const std::uint64_t bytes) noexcept {
        const std::lock_guard lock(mutex_);
        reserved_ = bytes <= reserved_ ? reserved_ - bytes : 0U;
    }
    std::filesystem::path path_;
    BenchmarkTraceSink trace_;
    std::mutex mutex_;
    std::uint64_t reserved_ = 0U;
};
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
[[nodiscard]] std::vector<std::uint64_t> image_ids(const NormalizedAnnotationIndex& index, const std::optional<std::uint16_t> shard = std::nullopt) {
    std::vector<std::uint64_t> ids;
    ids.reserve(index.images.size());
    for (const NormalizedImage& image : index.images) {
        if (!shard || image.source_shard == *shard) { ids.push_back(image.source_image_id); }
    }
    return ids;
}
class InvalidJpegError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
class RequiredImageDecodeError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
struct JpegDecodeProbe {
    std::uint64_t image_id = 0U;
    std::uint32_t expected_width = 0U;
    std::uint32_t expected_height = 0U;
};
class JpegValidator {
   private:
    class MappedJpeg {
       public:
        explicit MappedJpeg(const std::filesystem::path& path) : file_(common_io::FileHandle::open_readonly(path.string())), size_(file_.size()) {
            if (size_ == 0U) { throw InvalidJpegError("cached benchmark JPEG is empty"); }
            data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_.get(), 0);
            if (data_ == MAP_FAILED) {
                data_ = nullptr;
                throw common_io::errno_error("cannot map cached benchmark JPEG", path.string());
            }
        }
        ~MappedJpeg() {
            if (data_ != nullptr) { (void)::munmap(data_, size_); }
        }
        MappedJpeg(const MappedJpeg&) = delete;
        MappedJpeg& operator=(const MappedJpeg&) = delete;
        [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {static_cast<const std::uint8_t*>(data_), size_}; }

       private:
        common_io::FileHandle file_;
        std::size_t size_ = 0U;
        void* data_ = nullptr;
    };
    [[nodiscard]] static MappedJpeg map_file(const std::filesystem::path& path) { return MappedJpeg(path); }

   public:
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> read_header(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U,
                                                                      const std::uint32_t expected_height = 0U) {
        try {
            const BenchmarkJpegHeader header = decoder_.read_header(encoded, expected_width, expected_height);
            return {header.width, header.height};
        } catch (const BenchmarkJpegError& error) { throw InvalidJpegError(error.what()); }
    }
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> validate_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U,
                                                                        const std::uint32_t expected_height = 0U) {
        const MappedJpeg mapped = map_file(path);
        return read_header(mapped.bytes(), expected_width, expected_height);
    }
    void validate_decodable(const std::span<const std::uint8_t> encoded, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U) {
        try {
            const BenchmarkJpegHeader header = decoder_.read_header(encoded, expected_width, expected_height);
            decoder_.decode_rgb(encoded, header, &decoded_, &cmyk_);
        } catch (const BenchmarkJpegError& error) { throw InvalidJpegError(std::string("cannot fully decode benchmark JPEG: ") + error.what()); }
    }
    void validate_decodable_file(const std::filesystem::path& path, const std::uint32_t expected_width = 0U, const std::uint32_t expected_height = 0U) {
        const MappedJpeg mapped = map_file(path);
        validate_decodable(mapped.bytes(), expected_width, expected_height);
    }

   private:
    BenchmarkJpegDecoder decoder_;
    std::vector<std::uint8_t> decoded_;
    std::vector<std::uint8_t> cmyk_;
};
[[nodiscard]] NormalizedImage& find_normalized_image(NormalizedAnnotationIndex* index, const std::uint64_t image_id) {
    const auto found = std::ranges::lower_bound(index->images, image_id, {}, &NormalizedImage::source_image_id);
    if (found == index->images.end() || found->source_image_id != image_id) {
        throw std::runtime_error("cached benchmark image is absent from its annotation index");
    }
    return *found;
}
[[nodiscard]] const NormalizedImage& find_normalized_image(const NormalizedAnnotationIndex& index, const std::uint64_t image_id) {
    const auto found = std::ranges::lower_bound(index.images, image_id, {}, &NormalizedImage::source_image_id);
    if (found == index.images.end() || found->source_image_id != image_id) {
        throw std::runtime_error("cached benchmark image is absent from its annotation index");
    }
    return *found;
}
void remove_cache_path(const std::filesystem::path& path) {
    std::error_code error;
    (void)std::filesystem::remove(path, error);
    if (error) { throw std::filesystem::filesystem_error("cannot invalidate cached benchmark image", path, error); }
}
void ensure_curl_global() { ensure_curl_global_initialized("cannot initialize Open Images transfers: "); }
void set_open_images_curl_long_option(CURL* handle, const CURLoption option, const long value, const char* description) {
    set_curl_option_with_prefix(handle, option, value, "cannot configure Open Images ", description);
}
struct OpenImagesTransfer {
    std::uint64_t image_id = 0U;
    std::uint32_t attempt = 1U;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    CurlEasy easy;
    std::vector<std::uint8_t> encoded;
    std::exception_ptr callback_error;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    OpenImagesTransfer(const std::uint64_t id, const std::uint32_t attempt_value, mmltk::common::concurrency::CancellationObservation cancel,
                       std::vector<std::uint8_t> reusable_buffer)
        : image_id(id), attempt(attempt_value), cancel_requested(cancel), easy(curl_easy_init()), encoded(std::move(reusable_buffer)) {
        if (!easy) { throw std::runtime_error("cannot allocate Open Images transfer"); }
        encoded.clear();
        const std::string url = open_images_train_image_url(image_id);
        configure_curl_transfer(easy.get(),
                                CurlTransferSetup{
                                    .url = url.c_str(),
                                    .maximum_redirects = 5L,
                                    .error_buffer = error_buffer.data(),
                                    .owner = this,
                                    .write_callback = &OpenImagesTransfer::write_callback,
                                    .header_callback = nullptr,
                                    .progress_callback = &curl_cancel_progress_callback<OpenImagesTransfer>,
                                },
                                "cannot configure Open Images ");
        set_open_images_curl_long_option(easy.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS, "HTTP/2");
        set_open_images_curl_long_option(easy.get(), CURLOPT_TCP_KEEPALIVE, 1L, "TCP keepalive");
    }
    static std::size_t write_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
        return curl_run_data_callback<OpenImagesTransfer>(size, count, opaque, [data](OpenImagesTransfer& transfer, const std::size_t bytes) {
            if (bytes > kMaximumOpenImagesJpegBytes || transfer.encoded.size() > kMaximumOpenImagesJpegBytes - bytes) {
                throw std::runtime_error("Open Images JPEG exceeds the bounded transfer size");
            }
            const auto* source = reinterpret_cast<const std::uint8_t*>(data);
            transfer.encoded.insert(transfer.encoded.end(), source, source + bytes);
            return bytes;
        });
    }
};
class OpenImageCacheWorkers {
   public:
    struct Result {
        std::uint64_t image_id = 0U;
        std::uint32_t attempt = 0U;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::vector<std::uint8_t> encoded;
        std::string retry_reason;
        std::exception_ptr fatal_error;
    };
    OpenImageCacheWorkers(const std::size_t worker_count, std::filesystem::path image_root,
                          const mmltk::common::concurrency::CancellationObservation cancellation)
        : image_root_(std::move(image_root)), cancellation_(cancellation) {
        if (worker_count == 0U) {
            inline_validator_ = std::make_unique<JpegValidator>();
            return;
        }
        std::vector<std::unique_ptr<JpegValidator>> validators;
        validators.reserve(worker_count);
        for (std::size_t worker = 0U; worker < worker_count; ++worker) { validators.push_back(std::make_unique<JpegValidator>()); }
        workers_.reserve(worker_count);
        for (std::size_t worker = 0U; worker < worker_count; ++worker) {
            workers_.emplace_back([this, jpeg = std::move(validators[worker])] { run(jpeg.get()); });
        }
    }
    OpenImageCacheWorkers(const OpenImageCacheWorkers&) = delete;
    OpenImageCacheWorkers& operator=(const OpenImageCacheWorkers&) = delete;
    ~OpenImageCacheWorkers() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            tasks_.clear();
        }
        pending_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) { worker.join(); }
        }
    }
    void submit(const std::uint64_t image_id, const std::uint32_t attempt, std::vector<std::uint8_t> encoded) {
        throw_if_benchmark_cancelled(cancellation_);
        if (inline_validator_) {
            Result result = process(Task{image_id, attempt, std::move(encoded)}, inline_validator_.get());
            {
                const std::lock_guard lock(mutex_);
                results_.push_back(std::move(result));
                ++outstanding_;
            }
            ready_.notify_one();
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            tasks_.push_back(Task{image_id, attempt, std::move(encoded)});
            ++outstanding_;
        }
        pending_.notify_one();
    }
    [[nodiscard]] bool try_pop(Result* output) {
        const std::lock_guard lock(mutex_);
        throw_if_benchmark_cancelled(cancellation_);
        if (results_.empty()) { return false; }
        *output = std::move(results_.front());
        results_.pop_front();
        --outstanding_;
        return true;
    }
    [[nodiscard]] std::size_t outstanding() const {
        const std::lock_guard lock(mutex_);
        return outstanding_;
    }
    void wait_for_result() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return cancellation_.requested() || !results_.empty() || stopping_; });
        throw_if_benchmark_cancelled(cancellation_);
    }

   private:
    struct Task {
        std::uint64_t image_id = 0U;
        std::uint32_t attempt = 0U;
        std::vector<std::uint8_t> encoded;
    };
    [[nodiscard]] Result process(Task task, JpegValidator* jpeg) const noexcept {
        Result result;
        result.image_id = task.image_id;
        result.attempt = task.attempt;
        result.encoded = std::move(task.encoded);
        try {
            throw_if_benchmark_cancelled(cancellation_);
            const auto [width, height] = jpeg->read_header(result.encoded);
            result.width = width;
            result.height = height;
            write_cached_image_atomically(cached_image_path(image_root_, result.image_id), result.encoded, cancellation_);
            throw_if_benchmark_cancelled(cancellation_);
        } catch (const InvalidJpegError& error) { result.retry_reason = error.what(); } catch (...) {
            result.fatal_error = std::current_exception();
        }
        return result;
    }
    void run(JpegValidator* jpeg) noexcept {
        while (true) {
            std::optional<Task> task = wait_pop_task(mutex_, pending_, stopping_, tasks_, cancellation_);
            if (!task) { return; }
            Result result = process(std::move(*task), jpeg);
            {
                const std::lock_guard lock(mutex_);
                if (cancellation_.requested()) {
                    stopping_ = true;
                    tasks_.clear();
                    ready_.notify_all();
                    return;
                }
                results_.push_back(std::move(result));
            }
            ready_.notify_one();
        }
    }
    std::filesystem::path image_root_;
    mmltk::common::concurrency::CancellationObservation cancellation_;
    std::unique_ptr<JpegValidator> inline_validator_;
    mutable std::mutex mutex_;
    std::condition_variable pending_;
    std::condition_variable ready_;
    std::deque<Task> tasks_;
    std::deque<Result> results_;
    std::vector<std::thread> workers_;
    std::size_t outstanding_ = 0U;
    bool stopping_ = false;
};
void download_open_images(const std::span<const std::uint64_t> expected_ids, const std::filesystem::path& image_root,
                          NormalizedAnnotationIndex* annotation_index, std::vector<QuarantinedImage>* quarantined,
                          mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress, std::uint64_t* completed_images,
                          const std::uint64_t total_images, std::uint64_t* cached_image_bytes, const std::size_t transfer_concurrency,
                          const std::size_t cache_workers, const BenchmarkTraceSink& trace) {
    ensure_curl_global();
    CurlMulti multi(curl_multi_init());
    if (!multi) { throw std::runtime_error("cannot allocate Open Images multi transfer"); }
    if (curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS,
                          common_math::checked_cast<long>(transfer_concurrency, "Open Images concurrency overflow")) != CURLM_OK) {
        throw std::runtime_error("cannot set Open Images transfer concurrency");
    }
    if (curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS,
                          common_math::checked_cast<long>(transfer_concurrency, "Open Images host concurrency overflow")) != CURLM_OK ||
        curl_multi_setopt(multi.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX) != CURLM_OK) {
        throw std::runtime_error("cannot configure Open Images HTTP multiplexing");
    }
    OpenImageCacheWorkers cache_writer(cache_workers, image_root, cancel_requested);
    struct Pending {
        std::uint64_t image_id = 0U;
        std::uint32_t attempt = 1U;
        Clock::time_point ready_at = Clock::now();
    };
    std::deque<Pending> pending;
    for (const std::uint64_t id : expected_ids) { pending.push_back(Pending{id, 1U, Clock::now()}); }
    CurlMultiTransfers<OpenImagesTransfer> active(std::move(multi), "Open Images transfer");
    active.reserve(transfer_concurrency);
    std::vector<std::vector<std::uint8_t>> reusable_buffers(transfer_concurrency);
    for (std::vector<std::uint8_t>& buffer : reusable_buffers) { buffer.reserve(kEstimatedJpegBytes); }
    const auto recycle_encoded = [&](std::vector<std::uint8_t> encoded) {
        if (encoded.capacity() > kMaximumRetainedOpenImagesBufferBytes) {
            encoded = {};
            encoded.reserve(kEstimatedJpegBytes);
        } else {
            encoded.clear();
        }
        reusable_buffers.push_back(std::move(encoded));
    };
    const auto recycle_buffer = [&](std::unique_ptr<OpenImagesTransfer>& transfer) { recycle_encoded(std::move(transfer->encoded)); };
    const auto report_image_progress = [&] {
        constexpr std::uint64_t kProgressBatch = 64U;
        if (*completed_images == total_images || *completed_images % kProgressBatch == 0U) {
            progress->source_images(BenchmarkDatasetSource::kOpenImagesV7, *completed_images, total_images);
        }
    };
    const auto retry_or_quarantine = [&](const std::uint64_t image_id, const std::uint32_t attempt, std::string reason, const bool throttled = false) {
        trace_benchmark_event(trace, "benchmark.open_images.image_attempt_failed",
                              [&] { return nlohmann::json{{"image_id", image_id}, {"attempt", attempt}, {"reason", reason}}; });
        if (attempt < kMaximumAttempts) {
            const std::uint64_t backoff_ms =
                std::min<std::uint64_t>(throttled ? 8000U : 2000U, (throttled ? 500U : 100U) << std::min<std::uint32_t>(attempt - 1U, 4U));
            pending.push_back(Pending{image_id, attempt + 1U, Clock::now() + std::chrono::milliseconds{backoff_ms}});
            return;
        }
        quarantined->push_back(QuarantinedImage{
            BenchmarkDatasetSource::kOpenImagesV7,
            image_id,
            "failed after " + std::to_string(attempt) + " attempts: " + reason,
        });
        ++*completed_images;
        report_image_progress();
        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Quarantined unavailable Open Images training image " + std::to_string(image_id));
    };
    std::size_t active_limit = transfer_concurrency;
    const std::size_t minimum_active_limit = std::max<std::size_t>(1U, transfer_concurrency / 8U);
    std::uint64_t successes_since_throttle = 0U;
    try {
        const auto drain_cache_results = [&] {
            OpenImageCacheWorkers::Result result;
            while (cache_writer.try_pop(&result)) {
                if (result.fatal_error) { std::rethrow_exception(result.fatal_error); }
                if (!result.retry_reason.empty()) {
                    retry_or_quarantine(result.image_id, result.attempt, std::move(result.retry_reason));
                } else {
                    NormalizedImage& image = find_normalized_image(annotation_index, result.image_id);
                    image.width = result.width;
                    image.height = result.height;
                    *cached_image_bytes = common_math::checked_add(*cached_image_bytes, result.encoded.size(), "Open Images cached byte total overflow");
                    ++*completed_images;
                    report_image_progress();
                }
                recycle_encoded(std::move(result.encoded));
            }
        };
        while (!pending.empty() || !active.empty() || cache_writer.outstanding() != 0U) {
            throw_if_benchmark_cancelled(cancel_requested);
            drain_cache_results();
            while (!pending.empty() && active.size() < transfer_concurrency && !reusable_buffers.empty()) {
                const Clock::time_point now = Clock::now();
                const auto ready = std::ranges::find_if(pending, [&](const Pending& task) { return task.ready_at <= now; });
                if (ready == pending.end() || active.size() >= active_limit) { break; }
                const Pending task = *ready;
                pending.erase(ready);
                std::vector<std::uint8_t> buffer = std::move(reusable_buffers.back());
                reusable_buffers.pop_back();
                active.add(std::make_unique<OpenImagesTransfer>(task.image_id, task.attempt, cancel_requested, std::move(buffer)));
            }
            active.perform();
            while (std::optional completion = active.next_completed()) {
                std::unique_ptr<OpenImagesTransfer> transfer = std::move(completion->transfer);
                if (transfer->callback_error) {
                    try {
                        std::rethrow_exception(transfer->callback_error);
                    } catch (const std::exception& error) { retry_or_quarantine(transfer->image_id, transfer->attempt, error.what()); }
                    recycle_buffer(transfer);
                    continue;
                }
                long response_code = 0L;
                (void)curl_easy_getinfo(completion->handle, CURLINFO_RESPONSE_CODE, &response_code);
                if (completion->result != CURLE_OK || response_code != 200L || transfer->encoded.empty()) {
                    const std::string reason = transfer->error_buffer[0] != '\0'
                                                   ? transfer->error_buffer.data()
                                                   : "HTTP " + std::to_string(response_code) + ": " + curl_easy_strerror(completion->result);
                    retry_or_quarantine(transfer->image_id, transfer->attempt, reason, response_code == 429L || response_code == 503L);
                    if (response_code == 429L || response_code == 503L) {
                        active_limit = std::max<std::size_t>(minimum_active_limit, active_limit * 3U / 4U);
                        successes_since_throttle = 0U;
                        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                                                  "Open Images server throttled; continuing with " + std::to_string(active_limit) + " concurrent requests");
                    }
                    recycle_buffer(transfer);
                    continue;
                }
                ++successes_since_throttle;
                if (active_limit < transfer_concurrency && successes_since_throttle >= 1024U) {
                    active_limit = std::min(transfer_concurrency, active_limit + 8U);
                    successes_since_throttle = 0U;
                    progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Open Images concurrency recovered to " + std::to_string(active_limit));
                }
                if (!has_complete_jpeg_markers(transfer->encoded)) {
                    retry_or_quarantine(transfer->image_id, transfer->attempt, "Open Images response is not a complete JPEG");
                    recycle_buffer(transfer);
                    continue;
                }
                cache_writer.submit(transfer->image_id, transfer->attempt, std::move(transfer->encoded));
            }
            if (!active.empty()) {
                active.poll(kBenchmarkTransferPollMilliseconds);
            } else if (cache_writer.outstanding() != 0U) {
                cache_writer.wait_for_result();
            } else if (!pending.empty()) {
                const Clock::time_point retry_deadline = std::ranges::min_element(pending, {}, &Pending::ready_at)->ready_at;
                // This is an externally imposed HTTP retry deadline; no producer can make it ready earlier.
                throw_if_benchmark_cancelled(cancel_requested);
                std::this_thread::sleep_until(retry_deadline);
                throw_if_benchmark_cancelled(cancel_requested);
            }
        }
        drain_cache_results();
    } catch (...) {
        active.abandon_all([](OpenImagesTransfer&) noexcept {});
        throw;
    }
}
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
struct AcquiredOpenImages {
    CachedImageDirectory directory;
    std::vector<std::uint64_t> available_image_ids;
};
struct CachedOpenImagesGroup {
    bool valid = false;
    std::uint64_t image_bytes = 0U;
    std::vector<std::uint64_t> available_image_ids;
    std::vector<std::array<std::uint32_t, 2>> dimensions;
    std::vector<QuarantinedImage> quarantined;
};
[[nodiscard]] std::string open_images_group_name(const std::size_t group_index) {
    std::array<char, 32> shard_buffer{};
    const int length = std::snprintf(shard_buffer.data(), shard_buffer.size(), "group-%06zu", group_index);
    if (length <= 0 || static_cast<std::size_t>(length) >= shard_buffer.size()) { throw std::runtime_error("cannot format Open Images image group"); }
    return std::string(shard_buffer.data(), static_cast<std::size_t>(length));
}
[[nodiscard]] CachedOpenImagesGroup discover_cached_open_images_group(const std::filesystem::path& image_root, const std::filesystem::path& completion,
                                                                      const std::string_view identity, const std::span<const std::uint64_t> requested_image_ids,
                                                                      mmltk::common::concurrency::CancellationObservation cancel_requested,
                                                                      const BenchmarkTraceSink& trace) {
    CachedOpenImagesGroup result;
    try {
        if (!std::filesystem::is_regular_file(completion)) { return result; }
        const nlohmann::json manifest = read_json_file(completion);
        const std::size_t available_count = manifest.value("image_count", std::size_t{0U});
        const std::size_t requested_count = manifest.value("requested_image_count", available_count);
        const std::string available_digest = manifest.value("selection_sha256", std::string{});
        const std::string requested_digest = manifest.value("requested_selection_sha256", available_digest);
        if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || !manifest.value("complete", false) ||
            manifest.value("identity", std::string{}) != identity || requested_count != requested_image_ids.size() ||
            available_count > requested_image_ids.size() || requested_digest != cached_image_selection_digest(requested_image_ids)) {
            return result;
        }
        if (const auto iterator = manifest.find("quarantined"); iterator != manifest.end()) {
            result.quarantined.reserve(iterator->size());
            const bool parsed = parse_quarantined_manifest_records(*iterator, requested_image_ids, [&](const std::uint64_t image_id, std::string reason) {
                result.quarantined.push_back(QuarantinedImage{
                    BenchmarkDatasetSource::kOpenImagesV7,
                    image_id,
                    std::move(reason),
                });
            });
            if (!parsed) { return CachedOpenImagesGroup{}; }
            std::ranges::sort(result.quarantined, {}, &QuarantinedImage::image_id);
            if (std::ranges::adjacent_find(result.quarantined, {}, &QuarantinedImage::image_id) != result.quarantined.end()) { return CachedOpenImagesGroup{}; }
        }
        if (result.quarantined.size() > requested_image_ids.size() || available_count != requested_image_ids.size() - result.quarantined.size()) {
            return CachedOpenImagesGroup{};
        }
        result.available_image_ids.reserve(requested_image_ids.size() - result.quarantined.size());
        std::size_t quarantine_index = 0U;
        for (const std::uint64_t image_id : requested_image_ids) {
            if (quarantine_index < result.quarantined.size() && result.quarantined[quarantine_index].image_id == image_id) {
                ++quarantine_index;
                continue;
            }
            result.available_image_ids.push_back(image_id);
        }
        if (available_count != result.available_image_ids.size() || available_digest != cached_image_selection_digest(result.available_image_ids)) {
            return CachedOpenImagesGroup{};
        }
        const auto dimensions = manifest.find("dimensions");
        if (dimensions == manifest.end() || !dimensions->is_array() || dimensions->size() != available_count * 3U) { return CachedOpenImagesGroup{}; }
        result.dimensions.reserve(available_count);
        for (std::size_t index = 0U; index < available_count; ++index) {
            const std::uint64_t image_id = (*dimensions)[index * 3U].get<std::uint64_t>();
            const std::uint32_t width = (*dimensions)[index * 3U + 1U].get<std::uint32_t>();
            const std::uint32_t height = (*dimensions)[index * 3U + 2U].get<std::uint32_t>();
            if (image_id != result.available_image_ids[index] || width == 0U || height == 0U) { return CachedOpenImagesGroup{}; }
            result.dimensions.push_back({width, height});
        }
        if (!result.available_image_ids.empty() &&
            !validate_cached_image_group(image_root, completion, identity, result.available_image_ids, &result.image_bytes, cancel_requested, trace)) {
            return CachedOpenImagesGroup{};
        }
        result.valid = true;
        return result;
    } catch (const std::exception&) {
        throw_if_benchmark_cancelled(cancel_requested);
        return CachedOpenImagesGroup{};
    }
}
void complete_open_images_group(const std::filesystem::path& image_root, const std::filesystem::path& completion, const std::string_view identity,
                                const std::span<const std::uint64_t> requested_image_ids, const std::span<const std::uint64_t> available_image_ids,
                                const std::span<const QuarantinedImage> quarantined, const NormalizedAnnotationIndex& index, const std::uint64_t image_bytes,
                                const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace) {
    nlohmann::json quarantine_records = nlohmann::json::array();
    for (const QuarantinedImage& image : quarantined) { quarantine_records.push_back({{"image_id", image.image_id}, {"reason", image.reason}}); }
    nlohmann::json dimensions = nlohmann::json::array();
    for (const std::uint64_t image_id : available_image_ids) {
        const NormalizedImage& image = find_normalized_image(index, image_id);
        if (image.width == 0U || image.height == 0U) { throw std::runtime_error("Open Images cache completion has missing dimensions"); }
        dimensions.push_back(image_id);
        dimensions.push_back(image.width);
        dimensions.push_back(image.height);
    }
    write_json_atomically(completion,
                          nlohmann::json{
                              {"schema_version", kBenchmarkCacheSchemaVersion},
                              {"complete", true},
                              {"identity", identity},
                              {"image_count", available_image_ids.size()},
                              {"image_bytes", image_bytes},
                              {"selection_sha256", cached_image_selection_digest(available_image_ids)},
                              {"requested_image_count", requested_image_ids.size()},
                              {"requested_selection_sha256", cached_image_selection_digest(requested_image_ids)},
                              {"dimensions", std::move(dimensions)},
                              {"quarantined", std::move(quarantine_records)},
                          },
                          cancellation);
    trace_benchmark_event(trace, "benchmark.images.complete", [&] {
        return nlohmann::json{
            {"root", image_root.string()}, {"identity", identity}, {"images", available_image_ids.size()}, {"quarantined", quarantined.size()}};
    });
}
[[nodiscard]] AcquiredOpenImages acquire_open_images(const BenchmarkCacheLayout& cache, NormalizedAnnotationIndex& index,
                                                     std::vector<QuarantinedImage>* quarantined,
                                                     mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress,
                                                     const int num_workers, const std::size_t cache_workers, const BenchmarkTraceSink& trace,
                                                     const std::optional<JpegDecodeProbe> decode_probe = std::nullopt) {
    const std::vector<std::uint64_t> ids = image_ids(index);
    const std::filesystem::path image_root = cache.source_images("open-images") / "train";
    prepare_cached_image_directory(image_root);
    std::filesystem::create_directories(image_root / ".groups");
    std::vector<std::uint64_t> available;
    available.reserve(ids.size());
    std::uint64_t completed_images = 0U;
    std::uint64_t cached_image_bytes = 0U;
    bool all_cache_hits = true;
    JpegValidator jpeg_validator;
    const std::size_t configured_workers = common_math::checked_cast<std::size_t>(num_workers, "Open Images configured worker count overflow");
    if (configured_workers > std::numeric_limits<std::size_t>::max() / 10U) { throw std::overflow_error("Open Images transfer concurrency overflow"); }
    const std::size_t transfer_concurrency = std::min<std::size_t>(256U, configured_workers * 10U);
    if (transfer_concurrency == 0U) { throw std::runtime_error("Open Images transfer budget must be positive"); }
    progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                              "Open Images downloader ready with " + std::to_string(transfer_concurrency) + " concurrent requests and " +
                                  (cache_workers == 0U ? std::string{"inline cache writes"} : std::to_string(cache_workers) + " cache workers"));
    for (std::size_t begin = 0U; begin < ids.size(); begin += kOpenImagesGroupImages) {
        throw_if_benchmark_cancelled(cancel_requested);
        const std::size_t end = std::min(ids.size(), begin + kOpenImagesGroupImages);
        const std::span<const std::uint64_t> group(ids.data() + begin, end - begin);
        const std::string shard = open_images_group_name(begin / kOpenImagesGroupImages);
        const std::filesystem::path completion = image_root / ".groups" / (shard + ".complete.json");
        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Waiting for Open Images " + shard + " image lock");
        ArtifactLease lease = ArtifactLease::acquire(cache.locks / ("open-images-" + shard + ".images.lock"), cancel_requested);
        if (decode_probe && std::ranges::binary_search(group, decode_probe->image_id)) {
            progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                                      "Invalidating failed Open Images JPEG " + std::to_string(decode_probe->image_id) + " under the group lock");
            remove_cache_path(completion);
            remove_cache_path(cached_image_path(image_root, decode_probe->image_id));
        }
        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Checking Open Images " + shard + " cache completion status");
        const std::string identity = "open-images-v7:" + shard + ":" + std::string(kBenchmarkCatalogRevision);
        CachedOpenImagesGroup cached_group = discover_cached_open_images_group(image_root, completion, identity, group, cancel_requested, trace);
        bool group_cache_hit = cached_group.valid;
        if (group_cache_hit) {
            for (std::size_t image_index = 0U; image_index < cached_group.available_image_ids.size(); ++image_index) {
                NormalizedImage& image = find_normalized_image(&index, cached_group.available_image_ids[image_index]);
                image.width = cached_group.dimensions[image_index][0];
                image.height = cached_group.dimensions[image_index][1];
            }
        }
        if (group_cache_hit) {
            completed_images += group.size();
            cached_image_bytes = common_math::checked_add(cached_image_bytes, cached_group.image_bytes, "Open Images cached byte total overflow");
            progress->source_images(BenchmarkDatasetSource::kOpenImagesV7, completed_images, ids.size());
            available.insert(available.end(), cached_group.available_image_ids.begin(), cached_group.available_image_ids.end());
            quarantined->insert(quarantined->end(), std::make_move_iterator(cached_group.quarantined.begin()),
                                std::make_move_iterator(cached_group.quarantined.end()));
            continue;
        }
        all_cache_hits = false;
        const std::uint64_t group_bytes_begin = cached_image_bytes;
        require_storage(cache.root, common_math::checked_multiply(group.size(), kEstimatedJpegBytes, "Open Images cache estimate overflow"),
                        "Open Images JPEG cache", trace);
        std::vector<std::uint64_t> missing_downloads;
        missing_downloads.reserve(group.size());
        std::vector<std::uint64_t> group_available;
        group_available.reserve(group.size());
        for (const std::uint64_t image_id : group) {
            const std::filesystem::path path = cached_image_path(image_root, image_id);
            std::error_code error;
            const bool regular = std::filesystem::is_regular_file(path, error) && !error;
            const std::uint64_t bytes = regular ? std::filesystem::file_size(path, error) : 0U;
            if (regular && !error && bytes > 0U) {
                try {
                    const auto [width, height] = jpeg_validator.validate_file(path);
                    NormalizedImage& image = find_normalized_image(&index, image_id);
                    image.width = width;
                    image.height = height;
                    group_available.push_back(image_id);
                    ++completed_images;
                    cached_image_bytes = common_math::checked_add(cached_image_bytes, bytes, "Open Images cached byte total overflow");
                    continue;
                } catch (const InvalidJpegError& jpeg_error) {
                    remove_cache_path(path);
                    trace_benchmark_event(trace, "benchmark.images.cache_invalid", [&] {
                        return nlohmann::json{{"source", "open-images"}, {"shard", shard}, {"image_id", image_id}, {"error", jpeg_error.what()}};
                    });
                }
            }
            { missing_downloads.push_back(image_id); }
        }
        const std::size_t quarantine_begin = quarantined->size();
        if (!missing_downloads.empty()) {
            progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Downloading Open Images " + shard + " JPEGs");
            download_open_images(missing_downloads, image_root, &index, quarantined, cancel_requested, progress, &completed_images, ids.size(),
                                 &cached_image_bytes, transfer_concurrency, cache_workers, trace);
        }
        std::unordered_set<std::uint64_t> missing;
        for (std::size_t index_position = quarantine_begin; index_position < quarantined->size(); ++index_position) {
            missing.emplace((*quarantined)[index_position].image_id);
        }
        for (const std::uint64_t image_id : missing_downloads) {
            if (!missing.contains(image_id)) { group_available.push_back(image_id); }
        }
        std::ranges::sort(group_available);
        if (decode_probe && std::ranges::binary_search(group, decode_probe->image_id) && std::ranges::binary_search(group_available, decode_probe->image_id)) {
            const std::filesystem::path probe_path = cached_image_path(image_root, decode_probe->image_id);
            try {
                progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                                          "Full-decode checking repaired Open Images JPEG " + std::to_string(decode_probe->image_id));
                jpeg_validator.validate_decodable_file(probe_path, decode_probe->expected_width, decode_probe->expected_height);
            } catch (const InvalidJpegError& error) {
                const std::uint64_t probe_bytes = std::filesystem::file_size(probe_path);
                remove_cache_path(probe_path);
                if (probe_bytes > cached_image_bytes) { throw std::runtime_error("Open Images repair byte count underflow"); }
                cached_image_bytes -= probe_bytes;
                const auto available_image = std::ranges::lower_bound(group_available, decode_probe->image_id);
                if (available_image == group_available.end() || *available_image != decode_probe->image_id) {
                    throw std::runtime_error("Open Images repair image availability changed");
                }
                group_available.erase(available_image);
                quarantined->push_back(QuarantinedImage{BenchmarkDatasetSource::kOpenImagesV7, decode_probe->image_id,
                                                        "permanently undecodable after bounded repair: " + std::string(error.what())});
                progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7,
                                          "Quarantined permanently undecodable Open Images "
                                          "training JPEG " +
                                              std::to_string(decode_probe->image_id));
                trace_benchmark_event(trace, "benchmark.images.decode_quarantine", [&] {
                    return nlohmann::json{{"source", "open-images"}, {"image_id", decode_probe->image_id}, {"reason", error.what()}};
                });
            }
        }
        const std::span<const QuarantinedImage> group_quarantined(quarantined->data() + quarantine_begin, quarantined->size() - quarantine_begin);
        if (group_available.size() + group_quarantined.size() != group.size()) {
            throw std::runtime_error("Open Images group completion count is inconsistent");
        }
        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Finalizing Open Images " + shard + " cache");
        complete_open_images_group(image_root, completion, identity, group, group_available, group_quarantined, index, cached_image_bytes - group_bytes_begin,
                                   cancel_requested, trace);
        available.insert(available.end(), group_available.begin(), group_available.end());
        progress->source_images(BenchmarkDatasetSource::kOpenImagesV7, completed_images, ids.size());
    }
    std::ranges::sort(available);
    available.erase(std::unique(available.begin(), available.end()), available.end());
    return AcquiredOpenImages{CachedImageDirectory{"open-images",
                                                   "train",
                                                   image_root,
                                                   "open-images-v7:train:" + std::string(kBenchmarkCatalogRevision),
                                                   cached_image_selection_digest(available),
                                                   available.size(),
                                                   cached_image_bytes,
                                                   all_cache_hits,
                                                   {}},
                              std::move(available)};
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
        const RgbLetterbox letterbox = compute_rgb_letterbox(image.width, image.height, resolution, resolution);
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
    common_io::UniqueFd directory(descriptor);
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
void compile_benchmark_dataset(BenchmarkCompilerConfig config) {
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
}  // namespace benchmark_internal
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
void BenchmarkDatasetCompiler::compile(BenchmarkCompilerConfig config) { benchmark_internal::compile_benchmark_dataset(std::move(config)); }
}  // namespace mmltk::backend::data
