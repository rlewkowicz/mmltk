#include "src/backend/data/benchmark/detail/open_images_acquisition.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/detail/worker_queue.h"
#include "src/common/math/checked_arithmetic.h"
#include <curl/curl.h>
#include <queue>
#include "src/pch_std.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_math = mmltk::common::math;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kOpenImagesGroupImages = 4096U;
constexpr std::size_t kMaximumOpenImagesJpegBytes = std::size_t{8U} * 1024U * 1024U;
constexpr std::size_t kMaximumRetainedOpenImagesBufferBytes = std::size_t{2U} * 1024U * 1024U;
void set_open_images_curl_long_option(CURL* handle, const CURLoption option, const long value, const char* description) {
 set_curl_option_with_prefix(handle, option, value, "cannot configure Open Images ", description);
}
// One complete encoded-input/header/transfer grant follows this backing through
// Curl and cache publication. Idle slots are reusable; pressure destroys their
// backing immediately. Physical cached sockets have independent Curl custody.
struct OpenImagesBuffer {
 BenchmarkAllowance allowance;
 CurlEasy easy;
 std::unique_ptr<BenchmarkImageValidator> validator;
 std::vector<std::uint8_t> encoded;
 explicit OpenImagesBuffer(BenchmarkAllowance resources = {}) : allowance(std::move(resources)) {}
 ~OpenImagesBuffer() {
  validator.reset();
  std::vector<std::uint8_t>().swap(encoded);
  easy.reset();
  allowance = {};
 }
};
struct OpenImagesTransfer {
 std::uint64_t image_id = 0U;
 std::size_t position = 0;
 std::unique_ptr<OpenImagesBuffer> buffer;
 std::uint32_t attempt = 1U;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 CurlEasy& easy;
 std::vector<std::uint8_t>& encoded;
 std::exception_ptr callback_error;
 std::array<char, CURL_ERROR_SIZE> error_buffer{};
 OpenImagesTransfer(const std::uint64_t id, const std::uint32_t attempt_value, mmltk::common::concurrency::CancellationObservation cancel,
  std::unique_ptr<OpenImagesBuffer> input, const std::string& url)
  : image_id(id), buffer(std::move(input)), attempt(attempt_value), cancel_requested(cancel), easy(buffer->easy), encoded(buffer->encoded) {
  if (!easy) easy.reset(curl_easy_init());
  if (!easy) { throw std::runtime_error("cannot allocate Open Images transfer"); }
  encoded.clear();
  curl_easy_reset(easy.get());
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
  set_open_images_curl_long_option(easy.get(), CURLOPT_TCP_KEEPALIVE, 1L, "TCP keepalive");
 }
 static std::size_t write_callback(char* data, const std::size_t size, const std::size_t count, void* opaque) {
  return curl_run_data_callback<OpenImagesTransfer>(size, count, opaque, [data](OpenImagesTransfer& transfer, const std::size_t bytes) {
   if (bytes > kMaximumOpenImagesJpegBytes || transfer.encoded.size() > kMaximumOpenImagesJpegBytes - bytes) { throw std::runtime_error("Open Images JPEG exceeds the bounded transfer size"); }
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
  std::size_t position = 0;
  std::unique_ptr<OpenImagesBuffer> buffer;
  CachedImageReadySink ready;
  std::uint32_t attempt = 0U;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::string retry_reason;
  std::exception_ptr fatal_error;
 };
 OpenImageCacheWorkers(const std::size_t worker_count, std::filesystem::path image_root, const mmltk::common::concurrency::CancellationObservation cancellation, std::function<void()> wake, BenchmarkCompilePipeline* execution = nullptr)
     : storage_(image_root, {}, execution ? &execution->storage() : nullptr), execution_(execution), wake_(std::move(wake)), image_root_(std::move(image_root)), cancellation_(cancellation) {
  workers_ = std::make_unique<mmltk::common::concurrency::WorkerPool>(worker_count, mmltk::common::system::allowed_cpu_set(), "open_cache", worker_count);
  try {
   for (std::size_t lane = 0; lane < workers_->size(); ++lane)
    workers_->enqueue_borrowed(this, lane, [](void* owner, std::size_t) {
     static_cast<OpenImageCacheWorkers*>(owner)->run();
    });
  } catch (...) {
   stop();
   throw;
  }
 }
 OpenImageCacheWorkers(const OpenImageCacheWorkers&) = delete;
 OpenImageCacheWorkers& operator=(const OpenImageCacheWorkers&) = delete;
 ~OpenImageCacheWorkers() { stop(); }
 void submit(const std::uint64_t image_id, const std::uint32_t attempt, std::size_t position, std::unique_ptr<OpenImagesBuffer> buffer, CachedImageReadySink ready) {
  throw_if_benchmark_cancelled(cancellation_);
  {
   const std::lock_guard lock(mutex_);
   rethrow_failure_locked();
   tasks_.push_back(Task{image_id, position, std::move(buffer), std::move(ready), attempt});
   ++outstanding_;
  }
  pending_.notify_one();
 }
 [[nodiscard]] bool try_pop(Result* output) {
  const std::lock_guard lock(mutex_);
  throw_if_benchmark_cancelled(cancellation_);
  rethrow_failure_locked();
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
 void retire() noexcept { stop(); }

private:
 StorageReservationPool storage_;
 BenchmarkCompilePipeline* execution_ = nullptr;
 std::function<void()> wake_;
 struct Task {
  std::uint64_t image_id = 0U;
  std::size_t position = 0;
  std::unique_ptr<OpenImagesBuffer> buffer;
  CachedImageReadySink ready;
  std::uint32_t attempt = 0U;
 };
 [[nodiscard]] Result process(Task task) const {
  Result result;
  result.image_id = task.image_id;
  result.position = task.position;
  result.buffer = std::move(task.buffer);
  result.ready = std::move(task.ready);
  result.attempt = task.attempt;
  try {
   throw_if_benchmark_cancelled(cancellation_);
   const auto read_header = [&](std::size_t) {
    auto& validator = result.buffer->validator;
    if (!validator) validator = std::make_unique<BenchmarkImageValidator>();
    const auto dimensions = validator->read_header(result.buffer->encoded);
    result.width = dimensions.first;
    result.height = dimensions.second;
   };
   if (execution_) execution_->run(BenchmarkStage::Header, {}, read_header, result.buffer->allowance);
   else read_header(0);
   write_cached_image_atomically(cached_image_path(image_root_, result.image_id), result.buffer->encoded, cancellation_, storage_);
   if (result.ready) result.ready({image_root_, result.image_id, std::pair{result.width, result.height}, execution_ != nullptr});
   throw_if_benchmark_cancelled(cancellation_);
  } catch (const InvalidImageError& error) { result.retry_reason = error.what(); } catch (...) {
   result.fatal_error = std::current_exception();
  }
  return result;
 }
 void rethrow_failure_locked() const {
  if (failure_) std::rethrow_exception(failure_);
 }
 void stop() noexcept {
  {
   const std::lock_guard lock(mutex_);
   stopping_ = true;
   tasks_.clear();
  }
  pending_.notify_all();
  workers_.reset();
 }
 void run() noexcept {
  try {
   while (true) {
    auto task = wait_pop_task(mutex_, pending_, stopping_, tasks_, cancellation_);
    if (!task) return;
    Result result = process(std::move(*task));
    if (result.fatal_error) std::rethrow_exception(result.fatal_error);
    {
     const std::lock_guard lock(mutex_);
     results_.push_back(std::move(result));
    }
    if (wake_) wake_();
   }
  } catch (...) {
   {
    const std::lock_guard lock(mutex_);
    if (!failure_) failure_ = std::current_exception();
    stopping_ = true;
    tasks_.clear();
   }
   pending_.notify_all();
   if (wake_) wake_();
  }
 }
 std::filesystem::path image_root_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 mutable std::mutex mutex_;
 std::condition_variable pending_;
 std::deque<Task> tasks_;
 std::deque<Result> results_;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> workers_;
 std::exception_ptr failure_;
 std::size_t outstanding_ = 0U;
 bool stopping_ = false;
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
[[nodiscard]] CachedOpenImagesGroup discover_cached_open_images_group(const std::filesystem::path& image_root, const std::filesystem::path& completion, const std::string_view identity,
 const std::span<const std::uint64_t> requested_image_ids, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace) {
 CachedOpenImagesGroup result;
 try {
  if (!std::filesystem::is_regular_file(completion)) { return result; }
  const nlohmann::json manifest = read_json_file(completion);
  const std::size_t available_count = manifest.value("image_count", std::size_t{0U});
  const std::size_t requested_count = manifest.value("requested_image_count", available_count);
  const std::string available_digest = manifest.value("selection_sha256", std::string{});
  const std::string requested_digest = manifest.value("requested_selection_sha256", available_digest);
  if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || !manifest.value("complete", false) || manifest.value("identity", std::string{}) != identity ||
      requested_count != requested_image_ids.size() || available_count > requested_image_ids.size() || requested_digest != cached_image_selection_digest(requested_image_ids)) {
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
  if (result.quarantined.size() > requested_image_ids.size() || available_count != requested_image_ids.size() - result.quarantined.size()) { return CachedOpenImagesGroup{}; }
  result.available_image_ids.reserve(requested_image_ids.size() - result.quarantined.size());
  std::size_t quarantine_index = 0U;
  for (const std::uint64_t image_id : requested_image_ids) {
   if (quarantine_index < result.quarantined.size() && result.quarantined[quarantine_index].image_id == image_id) {
    ++quarantine_index;
    continue;
   }
   result.available_image_ids.push_back(image_id);
  }
  if (available_count != result.available_image_ids.size() || available_digest != cached_image_selection_digest(result.available_image_ids)) { return CachedOpenImagesGroup{}; }
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
  if (!result.available_image_ids.empty()) {
   // The dimensions/quarantine owner already admitted this same proof. Reuse
   // its fields instead of reopening JSON and rehashing the selection in the
   // generic directory-proof helper. Actual files are checked by their readers.
   if (!std::filesystem::is_directory(image_root)) return CachedOpenImagesGroup{};
   throw_if_benchmark_cancelled(cancel_requested);
   result.image_bytes = manifest.value("image_bytes", std::uint64_t{0});
   if (!result.image_bytes) return CachedOpenImagesGroup{};
   trace_benchmark_event(trace, "benchmark.images.cache_hit", [&] {
    return nlohmann::json{{"identity", identity}, {"images", result.available_image_ids.size()}, {"quarantined", 0}};
   });
  }
  result.valid = true;
  return result;
 } catch (const std::exception& error) {
  if (is_benchmark_capacity_failure(error)) throw;
  throw_if_benchmark_cancelled(cancel_requested);
  return CachedOpenImagesGroup{};
 }
}
void complete_open_images_group(const std::filesystem::path& image_root, const std::filesystem::path& completion, const std::string_view identity,
 const std::span<const std::uint64_t> requested_image_ids, const std::span<const std::uint64_t> available_image_ids, const std::span<const QuarantinedImage> quarantined,
 std::span<const NormalizedImage> images, const std::uint64_t image_bytes, const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, StorageReservationPool* storage) {
 nlohmann::json quarantine_records = nlohmann::json::array();
 for (const QuarantinedImage& image : quarantined) { quarantine_records.push_back({{"image_id", image.image_id}, {"reason", image.reason}}); }
 nlohmann::json dimensions = nlohmann::json::array();
 std::size_t available = 0;
 for (const auto& image : images) {
  if (available == available_image_ids.size()) break;
  if (image.source_image_id != available_image_ids[available]) continue;
  const auto image_id = available_image_ids[available++];
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
  cancellation, storage);
 trace_benchmark_event(trace, "benchmark.images.complete",
  [&] { return nlohmann::json{{"root", image_root.string()}, {"identity", identity}, {"images", available_image_ids.size()}, {"quarantined", quarantined.size()}}; });
}
}  // namespace
[[nodiscard]] AcquiredOpenImages acquire_open_images(const BenchmarkCacheLayout& cache, NormalizedAnnotationIndex& index, std::vector<QuarantinedImage>* quarantined,
 mmltk::common::concurrency::CancellationObservation cancellation, ProgressReporter* progress, int num_workers, std::size_t cache_workers, const BenchmarkTraceSink& trace,
 std::optional<ImageDecodeProbe> decode_probe, BenchmarkCompilePipeline* execution, const std::function<std::string(std::uint64_t)>& image_url) {
 const auto ids = image_ids(index);
 const auto image_root = cache.source_images("open-images") / "train";
 prepare_cached_image_directory(image_root);
 std::filesystem::create_directories(image_root / ".groups");
 const auto cpus = execution ? execution->workers() : common_math::checked_cast<std::size_t>(num_workers, "Open Images worker count overflow");
 if (!cpus) throw std::runtime_error("Open Images transfer budget must be positive");
 auto concurrency = std::min<std::size_t>(256, common_math::checked_multiply(cpus, std::size_t{10}, "Open Images concurrency overflow"));
 std::unique_ptr<BenchmarkCurl> local_transport;
 if (!execution) local_transport = std::make_unique<BenchmarkCurl>(cpus);
 auto& transport = execution ? execution->curl() : *local_transport;
 // The group lease commits its input/proof descriptor; reject only an
 // impossible complete descriptor promise, preserving oversized image bytes.
 const auto group_resources = BenchmarkResources::handles(1, true, 1);
 CurlMultiTransfers<OpenImagesTransfer> active(transport, BenchmarkCurl::Class::OpenImages, cancellation, group_resources);
 // This I/O owner persists across groups. Header CPU work uses the shared lanes.
 OpenImageCacheWorkers cache_writer(std::max<std::size_t>(1, execution ? 1 : cache_workers), image_root, cancellation, [&] { active.wake(); }, execution);
 std::vector<std::unique_ptr<OpenImagesBuffer>> buffers;
 enum class ImageState : std::uint8_t { Pending, Available, Quarantined };
 struct Group {
  std::size_t begin = 0, count = 0, completed = 0;
  std::string name, identity;
  std::filesystem::path completion;
  std::shared_ptr<ArtifactLease> lease;
  BenchmarkSourcePublication publication;
  std::vector<ImageState> images;
  std::vector<QuarantinedImage> quarantined;
  std::uint64_t bytes = 0;
  bool cache_hit = false;
  std::optional<CachedImageReady> repaired;
  std::uint64_t control_bytes = 0;
 };
 struct Pending {
  std::size_t position;
  std::uint32_t attempt = 1;
  Clock::time_point deadline{};
  bool operator>(const Pending& other) const { return deadline > other.deadline || (deadline == other.deadline && position > other.position); }
 };
 std::deque<Pending> fresh;
 std::priority_queue<Pending, std::vector<Pending>, std::greater<>> retries;
 std::unordered_map<std::size_t, std::unique_ptr<Group>> groups;
 std::deque<std::size_t> waiting_groups, scanning_groups;
 std::priority_queue<Pending, std::vector<Pending>, std::greater<>> locked_groups;
 for (std::size_t begin = 0; begin < ids.size(); begin += kOpenImagesGroupImages) waiting_groups.push_back(begin);
 std::vector<std::size_t> finished;
 std::vector<std::uint64_t> available;
 available.reserve(ids.size());
 std::uint64_t cached_bytes = 0, successes = 0;
 bool all_cache_hits = true, retry_turn = true;
 std::size_t active_limit = concurrency;
 auto minimum_limit = std::max<std::size_t>(1, concurrency / 8);
 const auto repair_bytes = decode_probe ? common_math::checked_multiply(common_math::checked_multiply(std::uint64_t{decode_probe->expected_width}, std::uint64_t{decode_probe->expected_height}, "Open Images repair workspace overflow"), std::uint64_t{96}, "Open Images repair workspace overflow") : 0;
 const auto payload_bytes = common_math::checked_add(std::uint64_t{kMaximumOpenImagesJpegBytes * 2}, repair_bytes, "Open Images input workspace overflow");
 auto input_resources = benchmark_curl_input_resources(payload_bytes);
 input_resources.producer = true;
 // 2 KiB per image bounds live state, pending/retry records, proof JSON and
 // quarantine text; 64 KiB covers group strings, maps and vector spare capacity.
 // These records contain no encoded/decoded/parser backing. Admission is bounded
 // by max(one complete group, the compile byte target), plus physical locks.
 const auto group_control = [](std::size_t count) { return std::uint64_t{64U << 10} + count * std::uint64_t{2048}; };
 const auto control_limit = std::max(group_control(std::min(kOpenImagesGroupImages, ids.size())), execution ? execution->transient_target() : group_control(kOpenImagesGroupImages) * concurrency);
 std::uint64_t control_bytes = 0;
 const auto report = [&](Group& group) { progress->transfers().images(BenchmarkDatasetSource::kOpenImagesV7, group.name, group.completed, ids.size(), *progress); };
 const auto finish_image = [&](Group& group) {
  ++group.completed;
  if (group.completed == group.count || group.completed % 64 == 0) report(group);
  if (group.completed == group.count) { finished.push_back(group.begin); active.wake(); }
 };
 const auto retry = [&](const Pending& task, std::string reason, bool throttle = false) {
  const auto id = ids[task.position];
  trace_benchmark_event(trace, "benchmark.open_images.image_attempt_failed", [&] { return nlohmann::json{{"image_id", id}, {"attempt", task.attempt}, {"reason", reason}}; });
  if (task.attempt < kMaximumAttempts) {
   const auto delay = std::min<std::uint64_t>(throttle ? 8000 : 2000, (throttle ? 500U : 100U) << std::min<std::uint32_t>(task.attempt - 1, 4));
   retries.push({task.position, task.attempt + 1, Clock::now() + std::chrono::milliseconds{delay}});
  } else {
   auto& group = *groups.at(task.position / kOpenImagesGroupImages * kOpenImagesGroupImages);
   group.images[task.position - group.begin] = ImageState::Quarantined;
   group.quarantined.push_back({BenchmarkDatasetSource::kOpenImagesV7, id, "failed after " + std::to_string(task.attempt) + " attempts: " + reason});
   finish_image(group);
   progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Quarantined unavailable Open Images training image " + std::to_string(id));
  }
 };
 const auto publish = [&](Group& group, const CachedImageReady& value) {
  if (decode_probe && value.image_id == decode_probe->image_id) group.repaired = value;
  else if (group.publication) group.publication(value);
 };
 const auto recycle = [&](std::unique_ptr<OpenImagesBuffer> buffer) {
  auto& encoded = buffer->encoded;
  if (encoded.capacity() > kMaximumRetainedOpenImagesBufferBytes) { std::vector<std::uint8_t>().swap(encoded); encoded.reserve(kEstimatedJpegBytes); }
  else encoded.clear();
  buffers.push_back(std::move(buffer));
 };
 struct RetireAcquisition {
  CurlMultiTransfers<OpenImagesTransfer>& active;
  OpenImageCacheWorkers& writer;
  ~RetireAcquisition() { active.abandon_all([](OpenImagesTransfer&) noexcept {}); writer.retire(); }
 } retire{active, cache_writer};
 while (!waiting_groups.empty() || !locked_groups.empty() || !groups.empty()) {
  throw_if_benchmark_cancelled(cancellation);
  OpenImageCacheWorkers::Result result;
  while (cache_writer.try_pop(&result)) {
   if (result.fatal_error) std::rethrow_exception(result.fatal_error);
   if (!result.retry_reason.empty()) retry({result.position, result.attempt}, std::move(result.retry_reason));
   else {
    auto& group = *groups.at(result.position / kOpenImagesGroupImages * kOpenImagesGroupImages);
    auto& image = index.images[result.position]; image.width = result.width; image.height = result.height;
    group.bytes = common_math::checked_add(group.bytes, result.buffer->encoded.size(), "Open Images cached byte total overflow");
    group.images[result.position - group.begin] = ImageState::Available;
    if (decode_probe && image.source_image_id == decode_probe->image_id) {
     const auto path = cached_image_path(image_root, image.source_image_id);
     try {
      progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Full-decode checking repaired Open Images JPEG " + std::to_string(image.source_image_id));
      const auto check = [&](std::size_t) {
       BenchmarkImageValidator validator;
       validator.validate_decodable_file(path, decode_probe->expected_width, decode_probe->expected_height);
      };
      // The saved file must be decoded while this result still exclusively owns
      // the complete input/repair slot. Sharing its allowance with a recycled
      // buffer would not reserve capacity for two simultaneous consumers.
      if (execution) execution->run(BenchmarkStage::Pixels, {repair_bytes, 1, true}, check, result.buffer->allowance);
      else check(0);
     } catch (const InvalidImageError& error) {
      const auto bytes = std::filesystem::file_size(path); remove_cache_path(path);
      if (bytes > group.bytes) throw std::runtime_error("Open Images repair byte count underflow");
      group.bytes -= bytes;
      group.images[result.position - group.begin] = ImageState::Quarantined;
      group.repaired.reset();
      group.quarantined.push_back({BenchmarkDatasetSource::kOpenImagesV7, image.source_image_id, "permanently undecodable after bounded repair: " + std::string(error.what())});
      progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Quarantined permanently undecodable Open Images training image " + std::to_string(image.source_image_id));
      trace_benchmark_event(trace, "benchmark.images.decode_quarantine", [&] { return nlohmann::json{{"source", "open-images"}, {"image_id", image.source_image_id}, {"reason", error.what()}}; });
     }
    }
    finish_image(group);
    recycle(std::move(result.buffer));
   }
   result = {};
  }
  while (auto completion = active.next_completed()) {
   auto& transfer = *completion->transfer;
   long status = 0; (void)curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &status);
   Pending task{transfer.position, transfer.attempt};
   if (transfer.callback_error) {
    try { std::rethrow_exception(transfer.callback_error); }
    catch (const std::exception& error) { if (is_benchmark_capacity_failure(error)) throw; retry(task, error.what()); }
    recycle(std::move(transfer.buffer));
    continue;
   }
   throw_if_benchmark_cancelled(cancellation);
   reject_local_curl_failure(completion->result);
   if (completion->result != CURLE_OK || status != 200 || transfer.encoded.empty()) {
    const bool throttle = status == 429 || status == 503;
    retry(task, transfer.error_buffer[0] ? transfer.error_buffer.data() : "HTTP " + std::to_string(status) + ": " + curl_easy_strerror(completion->result), throttle);
    if (throttle) {
     active_limit = std::max(minimum_limit, active_limit * 3 / 4); successes = 0;
     progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Open Images server throttled; continuing with " + std::to_string(active_limit) + " concurrent requests");
    }
    recycle(std::move(transfer.buffer));
    continue;
   }
   if (++successes >= 1024 && active_limit < concurrency) {
    active_limit = std::min(concurrency, active_limit + 8); successes = 0;
    progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Open Images concurrency recovered to " + std::to_string(active_limit));
   }
   if (!has_complete_image_markers(transfer.encoded)) { retry(task, "Open Images response is not a complete JPEG or PNG"); recycle(std::move(transfer.buffer)); continue; }
   auto& group = *groups.at(task.position / kOpenImagesGroupImages * kOpenImagesGroupImages);
   auto* group_pointer = &group;
   cache_writer.submit(transfer.image_id, task.attempt, task.position, std::move(transfer.buffer),
    [&, group_pointer](const CachedImageReady& value) { publish(*group_pointer, value); });
  }
  auto settling_groups = std::exchange(finished, {});
  for (const auto begin : settling_groups) {
   auto& group = *groups.at(begin);
   if (group.repaired && group.publication) group.publication(*group.repaired);
   group.repaired.reset();
   BenchmarkAllowance proof;
   if (execution && !group.cache_hit) {
    // A free slot can lend its descriptor for this synchronous proof. If every
    // slot is in use, draw the group's own commitment;
    // never block this controller while another slot must finish to release it.
    if (!buffers.empty()) proof = buffers.back()->allowance;
    else {
     auto admitted = execution->try_reserve(BenchmarkResources::handles(1, true), group.lease->allowance());
     if (!admitted) { finished.push_back(begin); continue; }
     proof = std::move(*admitted);
    }
   }
   std::vector<std::uint64_t> group_available;
   group_available.reserve(group.count - group.quarantined.size());
   for (std::size_t position = 0; position < group.count; ++position) if (group.images[position] == ImageState::Available) group_available.push_back(ids[group.begin + position]);
   if (group_available.size() + group.quarantined.size() != group.count) throw std::runtime_error("Open Images group completion count is inconsistent");
   if (!group.cache_hit) complete_open_images_group(image_root, group.completion, group.identity, std::span(ids).subspan(group.begin, group.count), group_available, group.quarantined,
    std::span(index.images).subspan(group.begin, group.count), group.bytes, cancellation, trace, execution ? &execution->storage() : nullptr);
   cached_bytes = common_math::checked_add(cached_bytes, group.bytes, "Open Images cached byte total overflow");
   available.insert(available.end(), group_available.begin(), group_available.end());
   quarantined->insert(quarantined->end(), std::make_move_iterator(group.quarantined.begin()), std::make_move_iterator(group.quarantined.end()));
   report(group);
   control_bytes -= group.control_bytes;
   groups.erase(begin);
  }
  // Idle capacity must not monopolize the byte ledger during delayed retries.
  const bool ready_retry = !retries.empty() && retries.top().deadline <= Clock::now();
  const bool pressure = execution && execution->resource_pressure();
  if (pressure || (fresh.empty() && !ready_retry)) buffers.clear();
  enum class GroupAdmission { Ready, Locked, Capacity };
  const auto admit_group = [&](std::size_t begin) {
   const auto count = std::min(kOpenImagesGroupImages, ids.size() - begin);
   const auto group_ids = std::span(ids).subspan(begin, count);
   const auto name = open_images_group_name(begin / kOpenImagesGroupImages);
   const auto controls = group_control(count);
   if (controls > control_limit - control_bytes || pressure) return GroupAdmission::Capacity;
   auto lease_resources = group_resources;
   lease_resources.bytes = controls;
   BenchmarkAllowance control;
   if (execution) {
    auto value = execution->try_reserve(lease_resources);
    if (!value) return GroupAdmission::Capacity;
    control = std::move(*value);
   }
   auto lease = ArtifactLease::try_acquire_charged(cache.locks / ("open-images-" + name + ".images.lock"), cancellation, std::move(control));
   if (!lease) return GroupAdmission::Locked;
   auto group = std::make_unique<Group>(); group->begin = begin; group->count = count; group->name = name; group->lease = std::move(lease); group->control_bytes = controls;
   group->completion = image_root / ".groups" / (name + ".complete.json");
   group->identity = "open-images-v7:" + name + ":" + std::string(kBenchmarkCatalogRevision);
   group->images.resize(count, ImageState::Pending);
   if (execution) group->publication = execution->source_publication(image_root, group->lease, decode_probe ? std::optional(decode_probe->image_id) : std::nullopt, true);
   if (decode_probe && std::ranges::binary_search(group_ids, decode_probe->image_id)) { remove_cache_path(group->completion); remove_cache_path(cached_image_path(image_root, decode_probe->image_id)); }
   control_bytes += controls;
   groups.emplace(begin, std::move(group));
   scanning_groups.push_back(begin);
   return GroupAdmission::Ready;
  };
  while (!waiting_groups.empty() || (!locked_groups.empty() && locked_groups.top().deadline <= Clock::now())) {
   std::size_t begin;
   if (!locked_groups.empty() && locked_groups.top().deadline <= Clock::now()) { begin = locked_groups.top().position; locked_groups.pop(); }
   else { begin = waiting_groups.front(); waiting_groups.pop_front(); }
   const auto result = admit_group(begin);
   if (result == GroupAdmission::Locked) locked_groups.push({begin, 1, Clock::now() + std::chrono::milliseconds{100}});
   if (result != GroupAdmission::Capacity) continue;
   // Full groups have equal demand. The final short group is the sole possible
   // smaller candidate; try it directly without scanning all pending groups.
   if (!waiting_groups.empty() && ids.size() - waiting_groups.back() < kOpenImagesGroupImages) {
    const auto tail = waiting_groups.back(); waiting_groups.pop_back();
    const auto tail_result = admit_group(tail);
    if (tail_result == GroupAdmission::Locked) locked_groups.push({tail, 1, Clock::now() + std::chrono::milliseconds{100}});
    else if (tail_result == GroupAdmission::Capacity) waiting_groups.push_back(tail);
   }
   waiting_groups.push_front(begin);
   break;
  }
  while (!pressure && !scanning_groups.empty()) {
   const auto begin = scanning_groups.front();
   auto* group = groups.at(begin).get();
   const auto count = group->count;
   const auto& name = group->name;
   const auto group_ids = std::span(ids).subspan(begin, count);
   std::unique_ptr<OpenImagesBuffer> scan;
   if (!buffers.empty()) { scan = std::move(buffers.back()); buffers.pop_back(); }
   else {
    BenchmarkAllowance allowance;
    if (execution) {
     auto value = execution->try_reserve(input_resources, group->lease->allowance());
     if (!value) break;
     allowance = std::move(*value);
    }
    scan = std::make_unique<OpenImagesBuffer>(std::move(allowance));
   }
   auto cached = discover_cached_open_images_group(image_root, group->completion, group->identity, group_ids, cancellation, trace);
   group->cache_hit = cached.valid;
   if (cached.valid) {
    std::fill(group->images.begin(), group->images.end(), ImageState::Quarantined);
    std::size_t position = 0;
    for (std::size_t i = 0; i < cached.available_image_ids.size(); ++i) {
     while (ids[begin + position] != cached.available_image_ids[i]) { group->images[position++] = ImageState::Quarantined; }
     auto& image = index.images[begin + position]; image.width = cached.dimensions[i][0]; image.height = cached.dimensions[i][1];
     group->images[position++] = ImageState::Available;
     publish(*group, {image_root, image.source_image_id, std::pair{image.width, image.height}});
    }
    group->bytes = cached.image_bytes; group->quarantined = std::move(cached.quarantined); group->completed = count;
    finished.push_back(begin); active.wake();
   } else {
    all_cache_hits = false;
    std::vector<std::uint64_t> sizes(count);
    std::vector<std::unique_ptr<OpenImagesBuffer>> scans;
    scans.push_back(std::move(scan));
    while (scans.size() < std::min(cpus, count)) {
     if (!buffers.empty()) { scans.push_back(std::move(buffers.back())); buffers.pop_back(); }
     else {
      BenchmarkAllowance allowance;
      if (execution) {
       auto value = execution->try_reserve(input_resources, group->lease->allowance());
       if (!value) break;
       allowance = std::move(*value);
      }
      scans.push_back(std::make_unique<OpenImagesBuffer>(std::move(allowance)));
     }
    }
    const auto inspect = [&](std::size_t local, BenchmarkImageValidator& validator) {
     const auto position = begin + local; const auto id = ids[position]; const auto path = cached_image_path(image_root, id);
     std::error_code error;
     if (!std::filesystem::is_regular_file(path, error) || error) return;
     const auto bytes = std::filesystem::file_size(path, error);
     if (error || !bytes) return;
     try {
      const auto dimensions = validator.validate_file(path);
      index.images[position].width = dimensions.first; index.images[position].height = dimensions.second;
      sizes[local] = bytes; group->images[local] = ImageState::Available;
     } catch (const InvalidImageError& failure) {
      remove_cache_path(path);
      trace_benchmark_event(trace, "benchmark.images.cache_invalid", [&] { return nlohmann::json{{"source", "open-images"}, {"shard", name}, {"image_id", id}, {"error", failure.what()}}; });
     }
    };
    const auto inspect_group = [&](std::size_t chunk) {
     auto& validator = scans[chunk]->validator;
     if (!validator) validator = std::make_unique<BenchmarkImageValidator>();
     const auto first = count * chunk / scans.size(), last = count * (chunk + 1) / scans.size();
     for (std::size_t local = first; local < last; ++local) {
      throw_if_benchmark_cancelled(cancellation); inspect(local, *validator);
      if (execution && local % 64 == 63) execution->cooperate();
     }
    };
    // Every runnable chunk already owns one complete input/work slot. Jobs
    // borrow those grants until for_each joins; no second byte admission occurs.
    if (execution) execution->for_each(BenchmarkStage::Header, scans.size(), {}, inspect_group);
    else for (std::size_t chunk = 0; chunk < scans.size(); ++chunk) inspect_group(chunk);
    for (auto& input : scans) recycle(std::move(input));
    for (std::size_t local = 0; local < count; ++local) {
     const auto position = begin + local;
     if (group->images[local] == ImageState::Available) {
      group->bytes = common_math::checked_add(group->bytes, sizes[local], "Open Images cached byte total overflow");
      ++group->completed;
      publish(*group, {image_root, ids[position], std::pair{index.images[position].width, index.images[position].height}});
     } else fresh.push_back({position});
    }
    if (group->completed == count) { finished.push_back(begin); active.wake(); }
   }
   if (scan) recycle(std::move(scan));
   report(*group);
   scanning_groups.pop_front();
  }
  while (!pressure && active.size() < active_limit && cache_writer.outstanding() + active.size() < concurrency) {
   const bool due = !retries.empty() && retries.top().deadline <= Clock::now();
   if (fresh.empty() && !due) break;
   const auto position = due && (retry_turn || fresh.empty()) ? retries.top().position : fresh.front().position;
   auto& pending_group = *groups.at(position / kOpenImagesGroupImages * kOpenImagesGroupImages);
   std::unique_ptr<OpenImagesBuffer> buffer;
   if (!buffers.empty()) { buffer = std::move(buffers.back()); buffers.pop_back(); }
   else {
    BenchmarkAllowance allowance;
    if (execution) {
     auto value = execution->try_reserve(input_resources, pending_group.lease->allowance()); if (!value) break;
     allowance = std::move(*value);
    }
    buffer = std::make_unique<OpenImagesBuffer>(std::move(allowance));
   }
   Pending task;
   if (due && (retry_turn || fresh.empty())) { task = retries.top(); retries.pop(); retry_turn = false; }
   else { task = fresh.front(); fresh.pop_front(); retry_turn = true; }
   auto transfer = std::make_unique<OpenImagesTransfer>(ids[task.position], task.attempt, cancellation, std::move(buffer), image_url ? image_url(ids[task.position]) : open_images_train_image_url(ids[task.position]));
   transfer->position = task.position;
   auto input = transfer->buffer->allowance;
   active.add(std::move(transfer), false, std::move(input));
  }
  if (waiting_groups.empty() && locked_groups.empty() && groups.empty()) break;
  auto deadline = Clock::now() + std::chrono::milliseconds{kBenchmarkTransferPollMilliseconds};
  if (!retries.empty() && retries.top().deadline > Clock::now()) deadline = std::min(deadline, retries.top().deadline);
  if (!locked_groups.empty()) deadline = std::min(deadline, locked_groups.top().deadline);
  active.wait_until(deadline);
 }
 std::ranges::sort(available);
 return {{"open-images", "train", image_root, "open-images-v7:train:" + std::string(kBenchmarkCatalogRevision), cached_image_selection_digest(available), available.size(), cached_bytes, all_cache_hits, {}}, std::move(available)};
}
}  // namespace mmltk::backend::data::benchmark_internal
