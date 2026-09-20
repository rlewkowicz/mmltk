#include "detail/benchmark_images.h"
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filereader/Standard.hpp>
#include <filesystem>
#include <functional>
#include <mutex>
#include <rapidgzip/ParallelGzipReader.hpp>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
#include "src/backend/data/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "worker_queue.h"
namespace mmltk::backend::data::benchmark_internal {
CachedImageWriteProgress::CachedImageWriteProgress(CachedImageProgress callback, const std::uint64_t initial, const std::uint64_t expected)
    : callback_(std::move(callback)), initial_(initial), expected_(expected) {}
void CachedImageWriteProgress::completed(const std::uint64_t writes) {
    if (!callback_ || ((writes & 127U) != 0U && writes != expected_)) { return; }
    const std::lock_guard lock(mutex_);
    if (writes <= published_) { return; }
    published_ = writes;
    callback_(initial_ + writes, initial_ + expected_);
}

using mmltk::common::io::errno_error;
using mmltk::common::io::FileHandle;
using mmltk::common::math::checked_add;
using mmltk::common::math::checked_cast;
namespace {
constexpr std::size_t kArchiveBlockBytes = std::size_t{4U} * 1024U * 1024U;
constexpr std::size_t kMaximumRetainedImageBufferBytes = std::size_t{2U} * 1024U * 1024U;
constexpr std::size_t kMaximumArchiveImageBytes = std::size_t{32U} * 1024U * 1024U;
struct ArchiveDestroy {
    void operator()(archive* reader) const noexcept {
        if (reader != nullptr) { (void)archive_read_free(reader); }
    }
};
using ArchiveReader = std::unique_ptr<archive, ArchiveDestroy>;
[[nodiscard]] std::uint64_t checked_byte_add(std::uint64_t left, std::uint64_t right);
struct ParallelGzipStream {
    ParallelGzipStream(const std::filesystem::path& path, const std::size_t workers)
        : reader(std::make_unique<rapidgzip::StandardFileReader>(path), std::max<std::size_t>(1U, workers)), buffer(kArchiveBlockBytes) {
        reader.setCRC32Enabled(false);
    }
    rapidgzip::ParallelGzipReader<> reader;
    std::vector<char> buffer;
    std::string error;
};
la_ssize_t read_parallel_gzip(archive* archive_reader, void* opaque, const void** output) noexcept {
    auto& stream = *static_cast<ParallelGzipStream*>(opaque);
    try {
        const std::size_t bytes = stream.reader.read(stream.buffer.data(), stream.buffer.size());
        *output = stream.buffer.data();
        return checked_cast<la_ssize_t>(bytes, "parallel gzip read size overflow");
    } catch (const std::exception& error) { stream.error = error.what(); } catch (...) {
        stream.error = "parallel gzip decompression failed";
    }
    archive_set_error(archive_reader, EIO, "%s", stream.error.c_str());
    return ARCHIVE_FATAL;
}
class CachedImageWritePool {
   public:
    CachedImageWritePool(const std::size_t worker_count, const std::size_t expected_writes, std::filesystem::path output_root, CachedImageProgress progress,
                         const std::uint64_t initially_completed, const mmltk::common::concurrency::CancellationObservation cancellation)
        : output_root_(std::move(output_root)),
          progress_(std::move(progress), initially_completed, expected_writes),
          cancellation_(cancellation) {
        const std::size_t bounded_workers = std::min<std::size_t>(8U, worker_count);
        inline_mode_ = bounded_workers == 0U;
        const std::size_t buffer_count = inline_mode_ ? 1U : bounded_workers * 2U + 2U;
        free_buffers_.resize(buffer_count);
        for (std::vector<std::uint8_t>& buffer : free_buffers_) { buffer.reserve(std::size_t{512U} * 1024U); }
        workers_.reserve(bounded_workers);
        for (std::size_t worker = 0U; worker < bounded_workers; ++worker) {
            workers_.emplace_back([this] { run(); });
        }
    }
    CachedImageWritePool(const CachedImageWritePool&) = delete;
    CachedImageWritePool& operator=(const CachedImageWritePool&) = delete;
    ~CachedImageWritePool() { stop(); }
    [[nodiscard]] std::vector<std::uint8_t> acquire(const std::size_t size) {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [&] { return failure_ || cancellation_.requested() || !free_buffers_.empty(); });
        throw_if_benchmark_cancelled(cancellation_);
        rethrow_failure_locked();
        std::vector<std::uint8_t> buffer = std::move(free_buffers_.back());
        free_buffers_.pop_back();
        lock.unlock();
        buffer.resize(size);
        return buffer;
    }
    void submit(const std::uint64_t image_id, std::vector<std::uint8_t> encoded) {
        throw_if_benchmark_cancelled(cancellation_);
        if (inline_mode_) {
            const std::uint64_t bytes = encoded.size();
            if (!has_complete_jpeg_markers(encoded)) {
                recycle(std::move(encoded));
                throw std::runtime_error("selected archive entry is not a complete JPEG");
            }
            write_cached_image_atomically(cached_image_path(output_root_, image_id), encoded, cancellation_);
            written_ids_.push_back(image_id);
            written_bytes_ = checked_byte_add(written_bytes_, bytes);
            const std::uint64_t completed = written_ids_.size();
            progress_.completed(completed);
            recycle(std::move(encoded));
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            rethrow_failure_locked();
            tasks_.push_back(Task{image_id, std::move(encoded)});
        }
        pending_.notify_one();
    }
    void recycle(std::vector<std::uint8_t> encoded) {
        if (encoded.capacity() > kMaximumRetainedImageBufferBytes) {
            encoded = {};
        } else {
            encoded.clear();
        }
        {
            const std::lock_guard lock(mutex_);
            free_buffers_.push_back(std::move(encoded));
        }
        available_.notify_one();
    }
    [[nodiscard]] std::vector<std::uint64_t> finish(std::uint64_t* image_bytes) {
        if (inline_mode_) {
            *image_bytes = checked_byte_add(*image_bytes, written_bytes_);
            std::ranges::sort(written_ids_);
            return std::move(written_ids_);
        }
        {
            std::unique_lock lock(mutex_);
            finished_.wait(lock, [&] { return failure_ || cancellation_.requested() || (tasks_.empty() && active_ == 0U); });
            throw_if_benchmark_cancelled(cancellation_);
            rethrow_failure_locked();
            stopping_ = true;
        }
        pending_.notify_all();
        join();
        *image_bytes = checked_byte_add(*image_bytes, written_bytes_);
        std::ranges::sort(written_ids_);
        return std::move(written_ids_);
    }

   private:
    struct Task {
        std::uint64_t image_id = 0U;
        std::vector<std::uint8_t> encoded;
    };
    void run() noexcept {
        while (true) {
            std::optional<Task> task = wait_pop_task(mutex_, pending_, stopping_, tasks_, cancellation_, [&] { ++active_; });
            if (!task) { return; }
            try {
                const std::uint64_t bytes = task->encoded.size();
                if (!has_complete_jpeg_markers(task->encoded)) { throw std::runtime_error("selected archive entry is not a complete JPEG"); }
                throw_if_benchmark_cancelled(cancellation_);
                write_cached_image_atomically(cached_image_path(output_root_, task->image_id), task->encoded, cancellation_);
                std::uint64_t completed = 0U;
                {
                    const std::lock_guard lock(mutex_);
                    written_ids_.push_back(task->image_id);
                    written_bytes_ = checked_byte_add(written_bytes_, bytes);
                    completed = written_ids_.size();
                }
                progress_.completed(completed);
            } catch (...) {
                const std::lock_guard lock(mutex_);
                if (!failure_) { failure_ = std::current_exception(); }
            }
            if (task->encoded.capacity() > kMaximumRetainedImageBufferBytes) {
                task->encoded = {};
            } else {
                task->encoded.clear();
            }
            {
                const std::lock_guard lock(mutex_);
                free_buffers_.push_back(std::move(task->encoded));
                --active_;
            }
            available_.notify_one();
            finished_.notify_one();
        }
    }
    void rethrow_failure_locked() const {
        if (failure_) { std::rethrow_exception(failure_); }
    }
    void join() noexcept {
        for (std::thread& worker : workers_) {
            if (worker.joinable()) { worker.join(); }
        }
    }
    void stop() noexcept {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            tasks_.clear();
        }
        pending_.notify_all();
        join();
    }
    std::filesystem::path output_root_;
    CachedImageWriteProgress progress_;
    mmltk::common::concurrency::CancellationObservation cancellation_;
    std::mutex mutex_;
    std::condition_variable pending_;
    std::condition_variable available_;
    std::condition_variable finished_;
    std::deque<Task> tasks_;
    std::vector<std::vector<std::uint8_t>> free_buffers_;
    std::vector<std::thread> workers_;
    std::vector<std::uint64_t> written_ids_;
    std::uint64_t written_bytes_ = 0U;
    std::size_t active_ = 0U;
    bool stopping_ = false;
    bool inline_mode_ = false;
    std::exception_ptr failure_;
};
[[nodiscard]] std::string archive_error(archive* reader) {
    const char* message = archive_error_string(reader);
    return message != nullptr ? message : "unknown libarchive error";
}
[[nodiscard]] std::uint64_t checked_byte_add(const std::uint64_t left, const std::uint64_t right) {
    return checked_add(left, right, "cached image byte total overflow");
}
[[nodiscard]] std::vector<std::uint64_t> available_image_ids(const std::span<const std::uint64_t> requested,
                                                             const std::span<const CachedImageRejection> quarantined) {
    std::vector<std::uint64_t> available;
    available.reserve(requested.size() - quarantined.size());
    std::size_t quarantine_index = 0U;
    for (const std::uint64_t image_id : requested) {
        if (quarantine_index < quarantined.size() && quarantined[quarantine_index].image_id == image_id) {
            ++quarantine_index;
            continue;
        }
        available.push_back(image_id);
    }
    return available;
}
// Builds the published cache-directory record from the surviving (non-quarantined) image ids.
[[nodiscard]] CachedImageDirectory make_cached_image_directory(const std::string& source, const std::string& shard, std::filesystem::path output_root,
                                                               const std::string& identity, const std::span<const std::uint64_t> selected_image_ids,
                                                               const std::uint64_t image_bytes, const bool cache_hit,
                                                               std::vector<CachedImageRejection> quarantined) {
    const std::vector<std::uint64_t> available = available_image_ids(selected_image_ids, quarantined);
    return CachedImageDirectory{source,      shard,     std::move(output_root), identity, cached_image_selection_digest(available), available.size(),
                                image_bytes, cache_hit, std::move(quarantined)};
}
[[nodiscard]] std::string missing_archive_images_message(const std::span<const std::uint64_t> missing) {
    std::string message = "verified benchmark archive is missing " + std::to_string(missing.size()) + " selected images";
    constexpr std::size_t kReportedIds = 8U;
    const std::size_t reported = std::min(missing.size(), kReportedIds);
    if (reported != 0U) {
        message += " (first IDs:";
        for (std::size_t index = 0U; index < reported; ++index) {
            message += index == 0U ? " " : ", ";
            message += std::to_string(missing[index]);
        }
        message += ')';
    }
    return message;
}
[[nodiscard]] std::uint64_t regular_file_bytes_at(const int root_descriptor, const std::uint64_t image_id) {
    std::array<char, 24> relative{};
    (void)format_cached_image_relative_path(image_id, relative);
    struct stat status{};
    if (::fstatat(root_descriptor, relative.data(), &status, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) { return 0U; }
    return static_cast<std::uint64_t>(status.st_size);
}
void require_archive_setup(archive* reader, const int status, const char* operation, const std::filesystem::path& archive_path,
                           const BenchmarkTraceSink& trace) {
    if (status != ARCHIVE_OK && status != ARCHIVE_WARN) { throw std::runtime_error(std::string("cannot ") + operation + ": " + archive_error(reader)); }
    if (status == ARCHIVE_WARN) {
        trace_benchmark_event(trace, "benchmark.archive.warning", [&] {
            return nlohmann::json{{"archive", archive_path.filename().string()}, {"operation", operation}, {"error", archive_error(reader)}};
        });
    }
}
}  // namespace
std::string cached_image_selection_digest(const std::span<const std::uint64_t> image_ids) {
    return mmltk::common::io::sha256_hex(
        mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(image_ids.data()), image_ids.size_bytes())));
}
std::filesystem::path cached_image_path(const std::filesystem::path& root, const std::uint64_t image_id) {
    std::array<char, 24> relative{};
    const std::size_t length = format_cached_image_relative_path(image_id, relative);
    return root / std::string(relative.data(), length);
}
void prepare_cached_image_directory(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    std::array<char, 3> shard{};
    for (std::uint32_t value = 0U; value <= 0xFFU; ++value) {
        const int length = std::snprintf(shard.data(), shard.size(), "%02x", value);
        if (length != 2) { throw std::runtime_error("cannot format cached benchmark image shard"); }
        std::filesystem::create_directories(root / std::string(shard.data(), 2U));
    }
}
std::size_t format_cached_image_relative_path(const std::uint64_t image_id, const std::span<char> output) {
    constexpr std::size_t kPathCharacters = 23U;
    constexpr std::size_t kRequiredBytes = kPathCharacters + 1U;
    if (output.size() < kRequiredBytes) { throw std::runtime_error("cached benchmark image path buffer is too small"); }
    const int length = std::snprintf(output.data(), output.size(), "%02llx/%016llx.jpg", static_cast<unsigned long long>(image_id & 0xFFU),
                                     static_cast<unsigned long long>(image_id));
    if (length != static_cast<int>(kPathCharacters)) { throw std::runtime_error("cannot format cached benchmark image ID"); }
    return static_cast<std::size_t>(length);
}
bool has_complete_jpeg_markers(const std::span<const std::uint8_t> encoded) noexcept {
    if (encoded.size() < 4U || encoded[0] != 0xFFU || encoded[1] != 0xD8U) { return false; }
    std::size_t end = encoded.size();
    while (end > 2U && encoded[end - 1U] == 0xFFU) { --end; }
    return end >= 4U && encoded[end - 2U] == 0xFFU && encoded[end - 1U] == 0xD9U;
}
void write_cached_image_atomically(const std::filesystem::path& path, const std::span<const std::uint8_t> encoded,
                                   const mmltk::common::concurrency::CancellationObservation cancellation) {
    if (encoded.empty()) { throw std::runtime_error("cannot cache an empty benchmark image"); }
    std::string staging_text = path.string() + ".tmp.XXXXXX";
    std::vector<char> writable_path(staging_text.begin(), staging_text.end());
    writable_path.push_back('\0');
    const int descriptor = ::mkostemp(writable_path.data(), O_CLOEXEC);
    if (descriptor < 0) { throw errno_error("cannot create cached benchmark image", staging_text); }
    staging_text.assign(writable_path.data());
    FileHandle staging(descriptor);
    staging.pwrite_all(encoded.data(), encoded.size(), 0U);
    staging = FileHandle{};
    const std::filesystem::path staging_path(staging_text);
    try {
        throw_if_benchmark_cancelled(cancellation);
        std::filesystem::rename(staging_path, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(staging_path, ignored);
        throw;
    }
}
bool validate_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, const std::string_view identity,
                                 const std::span<const std::uint64_t> expected_image_ids, std::uint64_t* image_bytes,
                                 mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace,
                                 std::vector<CachedImageRejection>* quarantined) {
    try {
        if (expected_image_ids.empty() || !std::filesystem::is_regular_file(completion_path)) { return false; }
        const nlohmann::json manifest = read_json_file(completion_path);
        if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || !manifest.value("complete", false) ||
            manifest.value("identity", std::string{}) != identity) {
            return false;
        }
        std::vector<CachedImageRejection> cached_quarantine;
        std::vector<std::uint64_t> available;
        std::span<const std::uint64_t> validated_ids = expected_image_ids;
        if (quarantined != nullptr) {
            const std::size_t requested_count = manifest.value("requested_image_count", manifest.value("image_count", std::size_t{0U}));
            const std::string requested_digest = manifest.value("requested_selection_sha256", manifest.value("selection_sha256", std::string{}));
            if (requested_count != expected_image_ids.size() || requested_digest != cached_image_selection_digest(expected_image_ids)) { return false; }
            if (const auto records = manifest.find("quarantined"); records != manifest.end()) {
                cached_quarantine.reserve(records->size());
                const bool parsed = parse_quarantined_manifest_records(*records, expected_image_ids, [&](const std::uint64_t image_id, std::string reason) {
                    cached_quarantine.push_back(CachedImageRejection{image_id, std::move(reason)});
                });
                if (!parsed) { return false; }
                std::ranges::sort(cached_quarantine, {}, &CachedImageRejection::image_id);
                if (std::ranges::adjacent_find(cached_quarantine, {}, &CachedImageRejection::image_id) != cached_quarantine.end()) { return false; }
            }
            available = available_image_ids(expected_image_ids, cached_quarantine);
            validated_ids = available;
        }
        if (manifest.value("image_count", 0ULL) != validated_ids.size() ||
            manifest.value("selection_sha256", std::string{}) != cached_image_selection_digest(validated_ids)) {
            return false;
        }
        if (!std::filesystem::is_directory(root)) { return false; }
        throw_if_benchmark_cancelled(cancel_requested);
        const std::uint64_t total_bytes = manifest.value("image_bytes", std::uint64_t{0U});
        if (total_bytes == 0U) { return false; }
        if (image_bytes != nullptr) { *image_bytes = total_bytes; }
        if (quarantined != nullptr) { *quarantined = std::move(cached_quarantine); }
        trace_benchmark_event(trace, "benchmark.images.cache_hit", [&] {
            return nlohmann::json{{"identity", identity}, {"images", validated_ids.size()}, {"quarantined", quarantined != nullptr ? quarantined->size() : 0U}};
        });
        return true;
    } catch (const std::exception&) {
        throw_if_benchmark_cancelled(cancel_requested);
        return false;
    }
}
void complete_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, const std::string_view identity,
                                 const std::span<const std::uint64_t> expected_image_ids, const std::uint64_t image_bytes,
                                 const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
                                 const std::span<const CachedImageRejection> quarantined) {
    const std::vector<std::uint64_t> available = available_image_ids(expected_image_ids, quarantined);
    nlohmann::json quarantined_records = nlohmann::json::array();
    for (const CachedImageRejection& image : quarantined) { quarantined_records.push_back({{"image_id", image.image_id}, {"reason", image.reason}}); }
    nlohmann::json manifest{{"schema_version", kBenchmarkCacheSchemaVersion},
                            {"complete", true},
                            {"identity", identity},
                            {"image_count", available.size()},
                            {"image_bytes", image_bytes},
                            {"selection_sha256", cached_image_selection_digest(available)}};
    if (!quarantined.empty()) {
        manifest["requested_image_count"] = expected_image_ids.size();
        manifest["requested_selection_sha256"] = cached_image_selection_digest(expected_image_ids);
        manifest["quarantined"] = std::move(quarantined_records);
    }
    throw_if_benchmark_cancelled(cancellation);
    write_json_atomically(completion_path, manifest, cancellation);
    trace_benchmark_event(trace, "benchmark.images.complete", [&] {
        return nlohmann::json{{"root", root.string()}, {"identity", identity}, {"images", available.size()}, {"quarantined", quarantined.size()}};
    });
}
CachedImageDirectory extract_selected_archive_images(ArchiveExtractionRequest request) {
    if (request.selected_image_ids.empty() || !request.image_id_parser || request.source.empty() || request.shard.empty() || request.source_identity.empty()) {
        throw std::runtime_error("benchmark archive extraction configuration is incomplete");
    }
    if (!std::ranges::is_sorted(request.selected_image_ids) || std::ranges::adjacent_find(request.selected_image_ids) != request.selected_image_ids.end()) {
        throw std::runtime_error("benchmark archive extraction IDs must be sorted and unique");
    }
    prepare_cached_image_directory(request.output_root);
    const std::filesystem::path completion = request.output_root / ".complete.json";
    const std::string& identity = request.source_identity;
    std::uint64_t image_bytes = 0U;
    std::vector<CachedImageRejection> quarantined;
    if (validate_cached_image_group(request.output_root, completion, identity, request.selected_image_ids, &image_bytes, request.cancel_requested,
                                    request.trace, request.quarantine_unavailable ? &quarantined : nullptr)) {
        if (request.progress) { request.progress(request.selected_image_ids.size(), request.selected_image_ids.size()); }
        return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes,
                                           true, std::move(quarantined));
    }
    const std::unordered_set<std::uint64_t> selected(request.selected_image_ids.begin(), request.selected_image_ids.end());
    std::unordered_set<std::uint64_t> completed;
    completed.reserve(request.selected_image_ids.size());
    std::unordered_set<std::uint64_t> unavailable;
    unavailable.reserve(64U);
    std::vector<std::uint8_t> encoded;
    const int output_descriptor = ::open(request.output_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (output_descriptor < 0) { throw errno_error("cannot open cached benchmark image directory", request.output_root.string()); }
    FileHandle output_directory(output_descriptor);
    if (request.activity) { request.activity("Checking individually cached JPEGs"); }
    std::size_t inspected = 0U;
    for (const std::uint64_t image_id : request.selected_image_ids) {
        throw_if_benchmark_cancelled(request.cancel_requested);
        if (request.trace && inspected != 0U && inspected % 128U == 0U) {
            trace_benchmark_event(request.trace, "benchmark.images.cache_scan", [&] {
                return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_images", inspected},
                                      {"reused_images", completed.size()}, {"total_images", request.selected_image_ids.size()}};
            });
        }
        if (request.trace) { ++inspected; }
        const std::uint64_t bytes = regular_file_bytes_at(output_directory.get(), image_id);
        if (bytes != 0U) {
            if (request.validator) {
                const std::filesystem::path path = cached_image_path(request.output_root, image_id);
                const FileHandle file = FileHandle::open_readonly(path.string());
                encoded.resize(checked_cast<std::size_t>(bytes, "cached benchmark image size overflow"));
                file.pread_all(encoded.data(), encoded.size(), 0U);
                try {
                    request.validator(image_id, encoded);
                } catch (const std::exception&) {
                    std::error_code error;
                    (void)std::filesystem::remove(path, error);
                    if (error) { throw std::filesystem::filesystem_error("cannot remove invalid cached benchmark image", path, error); }
                    continue;
                }
            }
            completed.emplace(image_id);
            image_bytes = checked_byte_add(image_bytes, bytes);
        }
    }
    if (request.progress) { request.progress(completed.size(), selected.size()); }
    trace_benchmark_event(request.trace, "benchmark.images.cache_reuse", [&] {
        return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_images", selected.size()},
                              {"reused_images", completed.size()}, {"reused_bytes", image_bytes}};
    });
    const std::size_t pending_writes = selected.size() - completed.size() - unavailable.size();
    if (request.activity) {
        request.activity(request.cache_write_workers == 0U
                             ? "Preparing inline selected-JPEG cache writes"
                             : "Preparing bounded cache-write queue with " + std::to_string(request.cache_write_workers) + " workers");
    }
    CachedImageWritePool write_pool(request.cache_write_workers, pending_writes, request.output_root, request.progress, completed.size(),
                                    request.cancel_requested);
    std::unordered_set<std::uint64_t> scheduled;
    scheduled.reserve(pending_writes);
    ArchiveReader reader(archive_read_new());
    if (!reader) { throw std::runtime_error("cannot allocate benchmark archive reader"); }
    std::unique_ptr<ParallelGzipStream> parallel_gzip;
    if (request.archive_path.extension() == ".gz" && request.decompression_workers != 0U) {
        if (request.activity) { request.activity("Parallel gzip decompression with " + std::to_string(request.decompression_workers) + " workers"); }
        parallel_gzip = std::make_unique<ParallelGzipStream>(request.archive_path, request.decompression_workers);
        require_archive_setup(reader.get(), archive_read_support_filter_none(reader.get()), "disable archive filters", request.archive_path, request.trace);
        require_archive_setup(reader.get(), archive_read_support_format_tar(reader.get()), "enable tar archive support", request.archive_path, request.trace);
        require_archive_setup(reader.get(), archive_read_open(reader.get(), parallel_gzip.get(), nullptr, read_parallel_gzip, nullptr),
                              "open parallel gzip archive", request.archive_path, request.trace);
    } else {
        if (request.activity) { request.activity("Scanning archive and extracting selected JPEGs"); }
        require_archive_setup(reader.get(), archive_read_support_filter_all(reader.get()), "enable archive filters", request.archive_path, request.trace);
        require_archive_setup(reader.get(), archive_read_support_format_tar(reader.get()), "enable tar archive support", request.archive_path, request.trace);
        require_archive_setup(reader.get(), archive_read_support_format_zip(reader.get()), "enable zip archive support", request.archive_path, request.trace);
        require_archive_setup(reader.get(), archive_read_open_filename(reader.get(), request.archive_path.c_str(), kArchiveBlockBytes),
                              "open benchmark archive", request.archive_path, request.trace);
    }
    if (request.activity) { request.activity("Scanning archive headers for selected images"); }
    bool extraction_announced = false;
    std::uint64_t inspected_headers = 0U;
    archive_entry* entry = nullptr;
    while (completed.size() + scheduled.size() + unavailable.size() != selected.size()) {
        throw_if_benchmark_cancelled(request.cancel_requested);
        int status = ARCHIVE_RETRY;
        for (int retry = 0; retry < 3 && status == ARCHIVE_RETRY; ++retry) { status = archive_read_next_header(reader.get(), &entry); }
        if (status == ARCHIVE_EOF) { break; }
        if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
            throw std::runtime_error("cannot read benchmark archive header: " + archive_error(reader.get()));
        }
        if (request.trace && (++inspected_headers % 1024U == 0U)) {
            trace_benchmark_event(request.trace, "benchmark.archive.scan", [&] {
                return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_headers", inspected_headers},
                                      {"scheduled_images", scheduled.size()}, {"reused_images", completed.size()}};
            });
        }
        const char* pathname = archive_entry_pathname(entry);
        const std::optional<std::uint64_t> image_id = pathname != nullptr ? request.image_id_parser(pathname) : std::nullopt;
        if (!image_id || !selected.contains(*image_id) || completed.contains(*image_id) || scheduled.contains(*image_id) || unavailable.contains(*image_id)) {
            int skip_status = ARCHIVE_RETRY;
            for (int retry = 0; retry < 3 && skip_status == ARCHIVE_RETRY; ++retry) { skip_status = archive_read_data_skip(reader.get()); }
            if (skip_status != ARCHIVE_OK && skip_status != ARCHIVE_WARN) { throw std::runtime_error("cannot skip benchmark archive entry"); }
            continue;
        }
        const la_int64_t entry_size = archive_entry_size(entry);
        if (entry_size <= 0 || static_cast<std::uint64_t>(entry_size) > kMaximumArchiveImageBytes) {
            throw std::runtime_error("selected benchmark archive image has an invalid size");
        }
        if (!extraction_announced && request.activity) {
            request.activity("Extracting selected JPEG payloads into the cache-write queue");
            extraction_announced = true;
        }
        encoded = write_pool.acquire(checked_cast<std::size_t>(entry_size, "benchmark archive image size overflow"));
        std::size_t offset = 0U;
        while (offset < encoded.size()) {
            la_ssize_t count = ARCHIVE_RETRY;
            for (int retry = 0; retry < 3 && count == ARCHIVE_RETRY; ++retry) {
                count = archive_read_data(reader.get(), encoded.data() + offset, encoded.size() - offset);
            }
            if (count <= 0) { throw std::runtime_error("cannot read selected benchmark archive image"); }
            offset += checked_cast<std::size_t>(count, "benchmark archive read size overflow");
        }
        if (request.validator) {
            try {
                request.validator(*image_id, encoded);
            } catch (const std::exception& error) {
                if (!request.quarantine_unavailable) { throw; }
                quarantined.push_back(CachedImageRejection{*image_id, error.what()});
                unavailable.emplace(*image_id);
                write_pool.recycle(std::move(encoded));
                continue;
            } catch (...) {
                if (!request.quarantine_unavailable) { throw; }
                quarantined.push_back(CachedImageRejection{*image_id, "benchmark JPEG validation raised a non-standard exception"});
                unavailable.emplace(*image_id);
                write_pool.recycle(std::move(encoded));
                continue;
            }
        }
        scheduled.emplace(*image_id);
        write_pool.submit(*image_id, std::move(encoded));
    }
    if (request.activity && request.cache_write_workers != 0U) { request.activity("Draining selected JPEG cache writes"); }
    for (const std::uint64_t image_id : write_pool.finish(&image_bytes)) { completed.emplace(image_id); }
    std::vector<std::uint64_t> missing;
    if (completed.size() + unavailable.size() != selected.size()) {
        missing.reserve(selected.size() - completed.size() - unavailable.size());
        for (const std::uint64_t image_id : request.selected_image_ids) {
            if (!completed.contains(image_id) && !unavailable.contains(image_id)) { missing.push_back(image_id); }
        }
        if (!request.quarantine_unavailable) { throw std::runtime_error(missing_archive_images_message(missing)); }
        for (const std::uint64_t image_id : missing) {
            quarantined.push_back(CachedImageRejection{image_id, "verified official archive does not contain selected image"});
        }
        if (request.progress) { request.progress(selected.size(), selected.size()); }
    }
    std::ranges::sort(quarantined, {}, &CachedImageRejection::image_id);
    if (request.progress) { request.progress(selected.size(), selected.size()); }
    if (request.activity) {
        if (quarantined.empty()) {
            request.activity("Publishing extracted-image cache status");
        } else {
            request.activity("Quarantined " + std::to_string(quarantined.size()) + " unavailable training images; publishing cache status");
        }
    }
    complete_cached_image_group(request.output_root, completion, identity, request.selected_image_ids, image_bytes, request.cancel_requested, request.trace,
                                quarantined);
    return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, false,
                                       std::move(quarantined));
}
}  // namespace mmltk::backend::data::benchmark_internal
