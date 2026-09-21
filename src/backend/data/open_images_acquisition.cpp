#include "detail/open_images_acquisition.h"
#include "detail/benchmark_catalog.h"
#include "detail/benchmark_download.h"
#include "detail/benchmark_curl.h"
#include "detail/benchmark_storage.h"
#include "detail/worker_queue.h"
#include "src/common/math/checked_arithmetic.h"
#include <curl/curl.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
namespace mmltk::backend::data::benchmark_internal {
namespace common_math = mmltk::common::math;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kOpenImagesGroupImages = 4096U;
constexpr std::size_t kMaximumOpenImagesJpegBytes = std::size_t{8U} * 1024U * 1024U;
constexpr std::size_t kMaximumRetainedOpenImagesBufferBytes = std::size_t{2U} * 1024U * 1024U;
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
}  // namespace
[[nodiscard]] AcquiredOpenImages acquire_open_images(const BenchmarkCacheLayout& cache, NormalizedAnnotationIndex& index,
                                                     std::vector<QuarantinedImage>* quarantined,
                                                     mmltk::common::concurrency::CancellationObservation cancel_requested, ProgressReporter* progress,
                                                     const int num_workers, const std::size_t cache_workers, const BenchmarkTraceSink& trace,
                                                     const std::optional<JpegDecodeProbe> decode_probe) {
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
        progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Checking individually cached Open Images " + shard + " JPEGs");
        std::size_t inspected = 0U;
        for (const std::uint64_t image_id : group) {
            throw_if_benchmark_cancelled(cancel_requested);
            if (trace && inspected != 0U && inspected % 128U == 0U) {
                trace_benchmark_event(trace, "benchmark.images.cache_scan", [&] {
                    return nlohmann::json{{"source", "open-images"},
                                          {"shard", shard},
                                          {"inspected_images", inspected},
                                          {"reused_images", group_available.size()},
                                          {"total_images", group.size()}};
                });
            }
            if (trace) { ++inspected; }
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
        progress->source_images(BenchmarkDatasetSource::kOpenImagesV7, completed_images, ids.size());
        trace_benchmark_event(trace, "benchmark.images.cache_reuse", [&] {
            return nlohmann::json{{"source", "open-images"},
                                  {"shard", shard},
                                  {"inspected_images", group.size()},
                                  {"reused_images", group_available.size()},
                                  {"reused_bytes", cached_image_bytes - group_bytes_begin}};
        });
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
}  // namespace mmltk::backend::data::benchmark_internal
