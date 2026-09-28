#include "src/backend/data/benchmark/detail/open_images_acquisition.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include <fcntl.h>
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_catalog.h"
#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/detail/worker_queue.h"
#include "src/common/math/checked_arithmetic.h"
#include <curl/curl.h>
#include <queue>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include "src/pch_std.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/numa_topology.h"
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
  allowance.retire_workspace();
  allowance = {};
 }
};
struct OpenImagesInput {
 std::uint64_t image_id = 0U;
 std::size_t position = 0;
 std::uint32_t attempt = 1U;
 std::unique_ptr<OpenImagesBuffer> buffer;
 bool warm = false;
 std::unique_ptr<BenchmarkEncodedImage::Opened> opened;
 std::uint64_t admission_observed = 0;
};
struct OpenImagesTransfer {
 OpenImagesInput input;
 mmltk::common::concurrency::CancellationObservation cancel_requested = {};
 CurlEasy& easy;
 std::vector<std::uint8_t>& encoded;
 std::exception_ptr callback_error;
 std::array<char, CURL_ERROR_SIZE> error_buffer{};
 OpenImagesTransfer(OpenImagesInput work, mmltk::common::concurrency::CancellationObservation cancel, const std::string& url)
  : input(std::move(work)), cancel_requested(cancel), easy(input.buffer->easy), encoded(input.buffer->encoded) {
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
  OpenImagesInput input;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::string retry_reason{};
  std::exception_ptr fatal_error{};
  std::shared_ptr<const BenchmarkEncodedImage> file_payload;
 };
 OpenImageCacheWorkers(const std::size_t worker_count, std::filesystem::path image_root, const mmltk::common::concurrency::CancellationObservation cancellation, std::function<void()> wake, BenchmarkCompilePipeline* execution, int directory, std::function<void(std::uint64_t)> warm_read)
     : storage_(image_root, {}, execution ? &execution->storage() : nullptr), execution_(execution), directory_(directory), warm_read_(std::move(warm_read)), wake_(std::move(wake)), image_root_(std::move(image_root)), cancellation_(cancellation) {
  const auto cpus = execution ? std::vector<int>(execution->cpus().begin(), execution->cpus().end()) : mmltk::common::system::allowed_cpu_set();
  const auto topology = mmltk::common::system::NumaTopology::Capture();
  const auto node = std::ranges::find(topology.cpus, cpus.front(), &mmltk::common::system::CpuTopology::cpu)->node;
  const auto placement = mmltk::common::system::resolve_placement(topology, node, -1, cpus);
  // Explicit placement keeps these bounded I/O waiters independent even on a
  // one-CPU assignment. Every header/decode still enters the shared CPU owner.
  workers_ = std::make_unique<mmltk::common::concurrency::WorkerPool>(worker_count, placement.cpus, "open_cache", worker_count, &placement);
  try {
   for (std::size_t lane = 0; lane < workers_->size(); ++lane)
    workers_->enqueue_borrowed(this, lane, [](void* owner, std::size_t lane) {
     static_cast<OpenImageCacheWorkers*>(owner)->run(lane);
    });
  } catch (...) {
   stop();
   throw;
  }
 }
 OpenImageCacheWorkers(const OpenImageCacheWorkers&) = delete;
 OpenImageCacheWorkers& operator=(const OpenImageCacheWorkers&) = delete;
 ~OpenImageCacheWorkers() { stop(); }
 void submit(OpenImagesInput input) {
  throw_if_benchmark_cancelled(cancellation_);
  {
   const std::lock_guard lock(mutex_);
   rethrow_failure_locked();
   if (input.warm) { warm_tasks_.push_back(std::move(input)); ++warm_outstanding_; }
   else tasks_.push_back(std::move(input));
   ++outstanding_;
  }
  pending_.notify_all();
 }
 [[nodiscard]] bool try_pop(Result* output) {
  const std::lock_guard lock(mutex_);
  throw_if_benchmark_cancelled(cancellation_);
  rethrow_failure_locked();
  // Deferred opens remain outstanding and keep their place in the eight-input
  // bound. Only final results reach the controller's group/proof mutation.
  while (!results_.empty() && results_.front().input.opened) {
   deferred_.push_back(std::move(results_.front().input));
   results_.pop_front();
  }
  if (results_.empty()) { return false; }
  *output = std::move(results_.front());
  results_.pop_front();
  --outstanding_;
  if (output->input.warm) --warm_outstanding_;
  return true;
 }
 [[nodiscard]] std::size_t outstanding() const {
  const std::lock_guard lock(mutex_);
  return outstanding_ - warm_outstanding_;
 }
 [[nodiscard]] std::size_t warm_outstanding() const { const std::lock_guard lock(mutex_); return warm_outstanding_; }
 void resume_deferred() {
  if (!execution_) return;
  const auto generation = execution_->admission_generation();
  bool resumed = false;
  {
   const std::lock_guard lock(mutex_);
   rethrow_failure_locked();
   const auto count = deferred_.size();
   for (std::size_t i = 0; i < count; ++i) {
    auto input = std::move(deferred_.front()); deferred_.pop_front();
    if (input.admission_observed == generation) deferred_.push_back(std::move(input));
    else { warm_tasks_.push_back(std::move(input)); resumed = true; }
   }
  }
  if (resumed) pending_.notify_all();
 }
 void retire() noexcept { stop(); }

private:
 StorageReservationPool storage_;
 BenchmarkCompilePipeline* execution_ = nullptr;
 int directory_;
 std::function<void(std::uint64_t)> warm_read_;
 std::function<void()> wake_;
 [[nodiscard]] Result process(OpenImagesInput input) const {
  Result result{std::move(input)};
  try {
   throw_if_benchmark_cancelled(cancellation_);
   if (result.input.warm) {
    if (!result.input.opened) {
     if (warm_read_) warm_read_(result.input.image_id);
     result.input.opened = BenchmarkEncodedImage::open_deferred(directory_, result.input.image_id, cancellation_, result.input.buffer->allowance,
      execution_ ? execution_->image_input(image_root_, result.input.image_id) : nullptr);
     if (!result.input.opened) return result;
    }
    // Observe before the nonblocking attempt, so a release racing its failure
    // cannot be lost at the controller's next admission-event retry.
    result.input.admission_observed = execution_ ? execution_->admission_generation() : 0;
    result.file_payload = result.input.opened->try_read(execution_, cancellation_);
    if (result.file_payload) {
     result.input.opened.reset();
     result.width = result.file_payload->header().width; result.height = result.file_payload->header().height;
    }
    return result;
   }
   const auto read_header = [&](std::size_t) {
    auto& validator = result.input.buffer->validator;
    if (!validator) validator = std::make_unique<BenchmarkImageValidator>();
    const auto dimensions = validator->read_header(result.input.buffer->encoded);
    result.width = dimensions.first;
    result.height = dimensions.second;
   };
   if (execution_) execution_->run(BenchmarkStage::Header, {}, read_header, result.input.buffer->allowance);
   else read_header(0);
   result.file_payload = BenchmarkEncodedImage::publish(directory_, result.input.image_id, result.input.buffer->encoded,
    result.input.buffer->validator->admitted_header(), cancellation_, storage_, execution_, result.input.buffer->allowance);
   throw_if_benchmark_cancelled(cancellation_);
  } catch (const InvalidImageError& error) { result.input.opened.reset(); result.retry_reason = error.what(); } catch (...) {
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
   tasks_.clear(); warm_tasks_.clear(); deferred_.clear(); results_.clear();
  }
  pending_.notify_all();
  workers_.reset();
 }
 void run(std::size_t lane) noexcept {
  try {
   bool prefer_warm = true;
   while (true) {
    std::optional<OpenImagesInput> task;
    {
     std::unique_lock lock(mutex_);
     // Lane zero is available to completed HTTP bodies even while all warm
     // readers are blocked in filesystem I/O. The remaining bounded I/O lanes
     // alternate ready writes and warm reads; CPU headers use the shared owner.
     pending_.wait(lock, [&] { return stopping_ || cancellation_.requested() || !tasks_.empty() || (lane && !warm_tasks_.empty()); });
     if (stopping_ || cancellation_.requested()) return;
     auto& queue = lane && !warm_tasks_.empty() && (prefer_warm || tasks_.empty()) ? warm_tasks_ : tasks_;
     task.emplace(std::move(queue.front())); queue.pop_front(); prefer_warm = !prefer_warm;
    }
    Result result = process(std::move(*task));
    if (result.fatal_error) std::rethrow_exception(result.fatal_error);
    {
     const std::lock_guard lock(mutex_);
     if (stopping_) return;
     results_.push_back(std::move(result));
    }
    if (wake_) wake_();
   }
  } catch (...) {
   {
    const std::lock_guard lock(mutex_);
    if (!failure_) failure_ = std::current_exception();
    stopping_ = true;
    tasks_.clear(); warm_tasks_.clear(); deferred_.clear(); results_.clear();
   }
   pending_.notify_all();
   if (wake_) wake_();
  }
 }
 std::filesystem::path image_root_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 mutable std::mutex mutex_;
 std::condition_variable pending_;
 std::deque<OpenImagesInput> tasks_, warm_tasks_, deferred_;
 std::deque<Result> results_;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> workers_;
 std::exception_ptr failure_;
 std::size_t outstanding_ = 0U, warm_outstanding_ = 0U;
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
 BenchmarkCompilePipeline& execution, const std::uint64_t image_bytes, const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace, StorageReservationPool* storage) {
 nlohmann::json quarantine_records = nlohmann::json::array();
 for (const QuarantinedImage& image : quarantined) { quarantine_records.push_back({{"image_id", image.image_id}, {"reason", image.reason}}); }
 nlohmann::json dimensions = nlohmann::json::array();
 for (const auto image_id : available_image_ids) {
  const auto geometry = execution.geometry(image_root, image_id);
  if (!geometry || geometry->width == 0 || geometry->height == 0) throw std::runtime_error("Open Images cache completion has missing dimensions");
  dimensions.push_back(image_id); dimensions.push_back(geometry->width); dimensions.push_back(geometry->height);
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
class OpenImagesAcquisition final {
 using Cancellation = mmltk::common::concurrency::CancellationObservation;
 static constexpr std::size_t kScanGrain = 64;
 // This pool owns the one admission and retained-backing policy used by scans,
 // HTTP, cache publication, and the saved-file repair check.
 class Inputs final {
  struct State {
   std::mutex mutex;
   std::array<std::vector<std::unique_ptr<OpenImagesBuffer>>, 2> idle;
   std::function<void()> wake;
   bool closed = false;
   void recycle(std::unique_ptr<OpenImagesBuffer> input, bool warm = false) noexcept {
    if (!input) return;
    if (input->encoded.capacity() > kMaximumRetainedOpenImagesBufferBytes) std::vector<std::uint8_t>().swap(input->encoded);
    else input->encoded.clear();
    { const std::lock_guard lock(mutex);
     if (!closed) { try { idle[warm].push_back(std::move(input)); } catch (...) {} }
    }
    if (wake) wake();
   }
  };
  BenchmarkCompilePipeline* execution_;
  std::array<BenchmarkResources, 2> demand_;
  std::shared_ptr<State> state_ = std::make_shared<State>();
 public:
  Inputs(BenchmarkCompilePipeline* execution, std::uint64_t repair_bytes, std::function<void()> wake) : execution_(execution),
   demand_{benchmark_curl_input_resources(common_math::checked_add(std::uint64_t{kMaximumOpenImagesJpegBytes * 2}, repair_bytes, "Open Images input workspace overflow")),
    BenchmarkResources::handles(1, true)} {
   // Warm slots contain only request/control records and a possible descriptor.
   // Their parser and mapped extent are admitted together after fstat.
   demand_[0].producer = true; state_->wake = std::move(wake);
  }
  ~Inputs() { close(); }
  void close() noexcept { const std::lock_guard lock(state_->mutex); state_->closed = true; for (auto& idle : state_->idle) idle.clear(); }
  std::unique_ptr<OpenImagesBuffer> acquire(const BenchmarkAllowance& parent, bool warm = false) {
   { const std::lock_guard lock(state_->mutex);
    auto& idle = state_->idle[warm];
    if (!idle.empty()) { auto input = std::move(idle.back()); idle.pop_back(); return input; }
   }
   BenchmarkAllowance allowance;
   if (execution_) { auto value = execution_->try_reserve(demand_[warm], parent); if (!value) return {}; allowance = std::move(*value); }
   return std::make_unique<OpenImagesBuffer>(std::move(allowance));
  }
  void recycle(std::unique_ptr<OpenImagesBuffer> input, bool warm = false) { state_->recycle(std::move(input), warm); }
  std::shared_ptr<const BenchmarkEncodedImage> retain(OpenImagesInput input, std::shared_ptr<const BenchmarkEncodedImage> file) {
   auto owned = std::shared_ptr<OpenImagesInput>(new OpenImagesInput(std::move(input)), [state = state_](OpenImagesInput* value) {
    auto buffer = std::move(value->buffer); delete value; state->recycle(std::move(buffer));
   });
   return BenchmarkEncodedImage::pooled(owned->buffer->allowance, owned, std::span<const std::uint8_t>(owned->buffer->encoded), std::move(file));
  }
  void retire_idle() noexcept { const std::lock_guard lock(state_->mutex); for (auto& idle : state_->idle) idle.clear(); }
  BenchmarkAllowance proof_allowance() const {
   const std::lock_guard lock(state_->mutex);
   for (const auto& idle : state_->idle) if (!idle.empty()) return idle.back()->allowance;
   return {};
  }
 };

 const BenchmarkCacheLayout& cache;
 const NormalizedAnnotationReadView& index;
 std::vector<QuarantinedImage>* quarantined;
 Cancellation cancellation;
 ProgressReporter* progress;
 const BenchmarkTraceSink& trace;
 std::optional<ImageDecodeProbe> decode_probe;
 BenchmarkCompilePipeline* execution;
 const std::function<std::string(std::uint64_t)>& image_url;
 std::vector<std::uint64_t> ids;
 std::filesystem::path image_root;
 std::size_t cpus;
 std::unique_ptr<BenchmarkCurl> local_transport;
 BenchmarkCurl& transport;
 std::size_t concurrency;
 BenchmarkResources group_resources = BenchmarkResources::handles(1, true, 1);
 enum class ImageState : std::uint8_t { Pending, Available, Quarantined };
 struct Group {
  std::size_t begin = 0, count = 0, completed = 0, scan_cursor = 0;
  std::string name, identity;
  std::filesystem::path completion;
  std::shared_ptr<ArtifactLease> lease;
  BenchmarkSourcePublication publication;
  std::vector<ImageState> images;
  std::vector<QuarantinedImage> quarantined;
  std::uint64_t bytes = 0;
  bool cache_hit = false, proof_checked = false;
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
 // Credit precedes backing so it is the last of this pair to retire. Capacity
 // is reserved once for the maximum live-group envelope, not for all groups.
 BenchmarkAllowance finished_allowance;
 std::vector<std::size_t> finished;
 std::vector<std::uint64_t> available;
 std::uint64_t cached_bytes = 0, successes = 0, control_bytes = 0;
 bool all_cache_hits = true, retry_turn = true;
 std::size_t active_limit, minimum_limit;
 std::uint64_t repair_bytes, control_limit;
 Inputs inputs;
 // These are last and explicitly retired before any callback/group borrow.
 CurlMultiTransfers<OpenImagesTransfer> active;
 BenchmarkAllowance directory_allowance;
 common_io::FileHandle directory;
 OpenImageCacheWorkers cache_writer;
 static std::uint64_t group_control(std::size_t count) { return std::uint64_t{64U << 10} + count * std::uint64_t{2048}; }
 static common_io::FileHandle open_directory(const std::filesystem::path& root) {
  const int descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) throw common_io::errno_error("cannot open Open Images cache directory");
  return common_io::FileHandle(descriptor);
 }
 static std::filesystem::path prepare_root(const BenchmarkCacheLayout& cache) {
  auto root = cache.source_images("open-images") / "train";
  prepare_cached_image_directory(root);
  std::filesystem::create_directories(root / ".groups");
  return root;
 }
 static std::size_t cpu_count(int workers, BenchmarkCompilePipeline* execution) {
  const auto count = execution ? execution->workers() : common_math::checked_cast<std::size_t>(workers, "Open Images worker count overflow");
  if (!count) throw std::runtime_error("Open Images transfer budget must be positive");
  return count;
 }
 static std::uint64_t repair_workspace(const std::optional<ImageDecodeProbe>& probe) {
  return probe ? common_math::checked_multiply(common_math::checked_multiply(std::uint64_t{probe->expected_width}, std::uint64_t{probe->expected_height}, "Open Images repair workspace overflow"), std::uint64_t{96}, "Open Images repair workspace overflow") : 0;
 }
 void prepare_completions() {
  // All groups except the final short one have identical control demand.
  const auto full = ids.size() / kOpenImagesGroupImages, tail = ids.size() % kOpenImagesGroupImages;
  const auto full_limit = std::min<std::uint64_t>(full, control_limit / group_control(kOpenImagesGroupImages));
  const auto with_tail = tail ? std::min<std::uint64_t>(full, (control_limit - group_control(tail)) / group_control(kOpenImagesGroupImages)) + 1 : 0;
  auto bound = common_math::checked_cast<std::size_t>(std::max(full_limit, with_tail), "Open Images group envelope overflow");
  if (execution) {
   const auto fixed = benchmark_curl_envelope().demand(1).descriptors;
   const auto ceiling = execution->descriptor_ceiling(group_resources);
   bound = std::min(bound, (ceiling - fixed) / (group_resources.descriptors + group_resources.continuation_descriptors));
  }
  auto demand = BenchmarkResources::handles(0);
  demand.bytes = common_math::checked_multiply(static_cast<std::uint64_t>(bound), std::uint64_t{sizeof(std::size_t)}, "Open Images completion capacity overflow");
  if (execution) finished_allowance = execution->reserve(demand);
  finished.reserve(bound);
  // Account the implementation's actual retained capacity before publishing it.
  if (execution && finished.capacity() != bound) {
   demand.bytes = common_math::checked_multiply(static_cast<std::uint64_t>(finished.capacity()), std::uint64_t{sizeof(std::size_t)}, "Open Images completion capacity overflow");
   finished_allowance = execution->reserve(demand);
  }
 }
 void queue_finished(const Group& group) {
  if (finished.size() == finished.capacity()) throw std::logic_error("Open Images completion exceeded its admitted group envelope");
  finished.push_back(group.begin);
  active.wake();
 }
 void report(Group& group) { progress->transfers().images(BenchmarkDatasetSource::kOpenImagesV7, group.name, group.completed, ids.size(), *progress); }
 void finish_image(Group& group) {
  ++group.completed;
  if (group.completed == group.count || group.completed % 64 == 0) report(group);
  if (group.completed == group.count) queue_finished(group);
 }
 void retry(const Pending& task, std::string reason, bool throttle = false) {
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
 }
 void publish(Group& group, const CachedImageReady& value) {
  if (group.publication) group.publication(value);
 }
 void consume_cache_results() {
  OpenImageCacheWorkers::Result result;
  while (cache_writer.try_pop(&result)) {
   if (result.fatal_error) std::rethrow_exception(result.fatal_error);
   if (result.input.warm) {
    auto& group = *groups.at(result.input.position / kOpenImagesGroupImages * kOpenImagesGroupImages);
    const auto id = result.input.image_id;
    if (!result.retry_reason.empty()) {
     remove_cache_path(cached_image_path(image_root, id));
     trace_benchmark_event(trace, "benchmark.images.cache_invalid", [&] { return nlohmann::json{{"source", "open-images"}, {"shard", group.name}, {"image_id", id}, {"error", result.retry_reason}}; });
    }
    if (result.file_payload) {
     group.bytes = common_math::checked_add(group.bytes, result.file_payload->size(), "Open Images cached byte total overflow");
     group.images[result.input.position - group.begin] = ImageState::Available;
     finish_image(group);
     publish(group, {id, std::pair{result.width, result.height}, true, std::move(result.file_payload)});
    } else fresh.push_back({result.input.position});
    inputs.recycle(std::move(result.input.buffer), true);
    result = {};
    continue;
   }
   if (!result.retry_reason.empty()) retry({result.input.position, result.input.attempt}, std::move(result.retry_reason));
   else {
    auto& group = *groups.at(result.input.position / kOpenImagesGroupImages * kOpenImagesGroupImages);
    const auto& image = index.image(result.input.position);
    group.bytes = common_math::checked_add(group.bytes, result.input.buffer->encoded.size(), "Open Images cached byte total overflow");
    group.images[result.input.position - group.begin] = ImageState::Available;
    const auto local = result.input.position - group.begin;
    auto payload = inputs.retain(std::move(result.input), std::move(result.file_payload));
    const CachedImageReady ready{image.source_image_id, std::pair{result.width, result.height}, execution != nullptr, payload};
    if (decode_probe && image.source_image_id == decode_probe->image_id) {
     const auto path = cached_image_path(image_root, image.source_image_id);
     try {
      progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Full-decode checking repaired Open Images JPEG " + std::to_string(image.source_image_id));
      try {
       if (!group.publication.consume(ready)) {
        // Standalone acquisition has no canonical pixel consumer. Its saved
        // cache artifact still supplies the one acceptance decode.
        BenchmarkImageValidator validator;
        validator.validate_decodable_file(path, decode_probe->expected_width, decode_probe->expected_height);
       }
      } catch (const BenchmarkImageReadError& error) { throw InvalidImageError(error.what()); }
     } catch (const InvalidImageError& error) {
      const auto bytes = std::filesystem::file_size(path); remove_cache_path(path);
      if (bytes > group.bytes) throw std::runtime_error("Open Images repair byte count underflow");
      group.bytes -= bytes;
      group.images[local] = ImageState::Quarantined;
      group.quarantined.push_back({BenchmarkDatasetSource::kOpenImagesV7, image.source_image_id, "permanently undecodable after bounded repair: " + std::string(error.what())});
      progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Quarantined permanently undecodable Open Images training image " + std::to_string(image.source_image_id));
      trace_benchmark_event(trace, "benchmark.images.decode_quarantine", [&] { return nlohmann::json{{"source", "open-images"}, {"image_id", image.source_image_id}, {"reason", error.what()}}; });
     }
    }
    finish_image(group);
    if (group.images[local] == ImageState::Available) publish(group, ready);
   }
   result = {};
  }
 }
 void consume_transfer_results() {
  while (auto completion = active.next_completed()) {
   auto& transfer = *completion->transfer;
   long status = 0; (void)curl_easy_getinfo(transfer.easy.get(), CURLINFO_RESPONSE_CODE, &status);
   Pending task{transfer.input.position, transfer.input.attempt};
   if (transfer.callback_error) {
    try { std::rethrow_exception(transfer.callback_error); }
    catch (const std::exception& error) { if (is_benchmark_capacity_failure(error)) throw; retry(task, error.what()); }
    inputs.recycle(std::move(transfer.input.buffer));
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
    inputs.recycle(std::move(transfer.input.buffer));
    continue;
   }
   if (++successes >= 1024 && active_limit < concurrency) {
    active_limit = std::min(concurrency, active_limit + 8); successes = 0;
    progress->source_activity(BenchmarkDatasetSource::kOpenImagesV7, "Open Images concurrency recovered to " + std::to_string(active_limit));
   }
   if (!has_complete_image_markers(transfer.encoded)) { retry(task, "Open Images response is not a complete JPEG or PNG"); inputs.recycle(std::move(transfer.input.buffer)); continue; }
   cache_writer.submit(std::move(transfer.input));
  }
 }
 void settle_groups() {
  std::size_t deferred = 0;
  for (const auto begin : finished) {
   auto& group = *groups.at(begin);
   BenchmarkAllowance proof;
   if (execution && !group.cache_hit) {
    // A free slot can lend its descriptor for this synchronous proof. If every
    // slot is in use, draw the group's own commitment;
    // never block this controller while another slot must finish to release it.
    if (auto idle = inputs.proof_allowance()) proof = std::move(idle);
    else {
     auto admitted = execution->try_reserve(BenchmarkResources::handles(1, true), group.lease->allowance());
     if (!admitted) { finished[deferred++] = begin; continue; }
     proof = std::move(*admitted);
    }
   }
   std::vector<std::uint64_t> group_available;
   group_available.reserve(group.count - group.quarantined.size());
   for (std::size_t position = 0; position < group.count; ++position) if (group.images[position] == ImageState::Available) group_available.push_back(ids[group.begin + position]);
   if (group_available.size() + group.quarantined.size() != group.count) throw std::runtime_error("Open Images group completion count is inconsistent");
   if (!group.cache_hit) complete_open_images_group(image_root, group.completion, group.identity, std::span(ids).subspan(group.begin, group.count), group_available, group.quarantined,
    *execution, group.bytes, cancellation, trace, execution ? &execution->storage() : nullptr);
   cached_bytes = common_math::checked_add(cached_bytes, group.bytes, "Open Images cached byte total overflow");
   available.insert(available.end(), group_available.begin(), group_available.end());
   quarantined->insert(quarantined->end(), std::make_move_iterator(group.quarantined.begin()), std::make_move_iterator(group.quarantined.end()));
   report(group);
   control_bytes -= group.control_bytes;
   groups.erase(begin);
  }
  finished.resize(deferred);
 }
 enum class GroupAdmission { Ready, Locked, Capacity };
 GroupAdmission admit_group(std::size_t begin, bool pressure) {
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
 }
 void admit_groups(bool pressure) {
  while (!waiting_groups.empty() || (!locked_groups.empty() && locked_groups.top().deadline <= Clock::now())) {
   std::size_t begin;
   if (!locked_groups.empty() && locked_groups.top().deadline <= Clock::now()) { begin = locked_groups.top().position; locked_groups.pop(); }
   else { begin = waiting_groups.front(); waiting_groups.pop_front(); }
   const auto result = admit_group(begin, pressure);
   if (result == GroupAdmission::Locked) locked_groups.push({begin, 1, Clock::now() + std::chrono::milliseconds{100}});
   if (result != GroupAdmission::Capacity) continue;
   // Full groups have equal demand. The final short group is the sole possible
   // smaller candidate; try it directly without scanning all pending groups.
   if (!waiting_groups.empty() && ids.size() - waiting_groups.back() < kOpenImagesGroupImages) {
    const auto tail = waiting_groups.back(); waiting_groups.pop_back();
    const auto tail_result = admit_group(tail, pressure);
    if (tail_result == GroupAdmission::Locked) locked_groups.push({tail, 1, Clock::now() + std::chrono::milliseconds{100}});
    else if (tail_result == GroupAdmission::Capacity) waiting_groups.push_back(tail);
   }
   waiting_groups.push_front(begin);
   break;
  }
 }
 void admit_http(bool pressure) {
  while (!pressure && active.size() < active_limit && cache_writer.outstanding() + active.size() < concurrency) {
   const bool due = !retries.empty() && retries.top().deadline <= Clock::now();
   if (fresh.empty() && !due) break;
   const auto position = due && (retry_turn || fresh.empty()) ? retries.top().position : fresh.front().position;
   auto& pending_group = *groups.at(position / kOpenImagesGroupImages * kOpenImagesGroupImages);
   auto buffer = inputs.acquire(pending_group.lease->allowance());
   if (!buffer) break;
   Pending task;
   if (due && (retry_turn || fresh.empty())) { task = retries.top(); retries.pop(); retry_turn = false; }
   else { task = fresh.front(); fresh.pop_front(); retry_turn = true; }
   OpenImagesInput work{ids[task.position], task.position, task.attempt, std::move(buffer)};
   auto transfer = std::make_unique<OpenImagesTransfer>(std::move(work), cancellation, image_url ? image_url(ids[task.position]) : open_images_train_image_url(ids[task.position]));
   auto input = transfer->input.buffer->allowance;
   active.add(std::move(transfer), false, std::move(input));
  }
 }
 bool scan_chunk(bool pressure) {
  if (pressure || scanning_groups.empty()) return false;
  const auto begin = scanning_groups.front();
  auto& group = *groups.at(begin);
  if (!group.proof_checked) {
   auto input = inputs.acquire(group.lease->allowance());
   if (!input) return false;
   group.proof_checked = true;
   auto cached = discover_cached_open_images_group(image_root, group.completion, group.identity, std::span(ids).subspan(begin, group.count), cancellation, trace);
   group.cache_hit = cached.valid;
   if (cached.valid) {
    std::fill(group.images.begin(), group.images.end(), ImageState::Quarantined);
    std::size_t position = 0;
    for (std::size_t i = 0; i < cached.available_image_ids.size(); ++i) {
     while (ids[begin + position] != cached.available_image_ids[i]) ++position;
     const auto& image = index.image(begin + position);
     group.images[position++] = ImageState::Available;
     publish(group, {image.source_image_id, std::pair{cached.dimensions[i][0], cached.dimensions[i][1]}});
    }
    group.bytes = cached.image_bytes; group.quarantined = std::move(cached.quarantined); group.completed = group.count;
    queue_finished(group);
    inputs.recycle(std::move(input)); scanning_groups.pop_front(); report(group);
    return true;
   }
   all_cache_hits = false;
   inputs.recycle(std::move(input));
  }
  // Queue a bounded grain of independently admitted I/O requests. Complete
  // mapped-result backing is charged by the I/O worker before result enqueue.
  const auto begin_cursor = group.scan_cursor;
  while (group.scan_cursor < group.count && group.scan_cursor - begin_cursor < kScanGrain && cache_writer.warm_outstanding() < 8) {
   auto input = inputs.acquire(group.lease->allowance(), true);
   if (!input) break;
   const auto position = begin + group.scan_cursor;
   cache_writer.submit({ids[position], position, 1, std::move(input), true});
   ++group.scan_cursor;
  }
  if (group.scan_cursor == group.count) scanning_groups.pop_front();
  report(group);
  return group.scan_cursor != begin_cursor;
 }
public:
 OpenImagesAcquisition(const BenchmarkCacheLayout& cache_value, const NormalizedAnnotationReadView& index_value, std::vector<QuarantinedImage>* quarantined_value,
  Cancellation cancel, ProgressReporter* reporter, int workers, std::size_t cache_workers, const BenchmarkTraceSink& trace_value,
  std::optional<ImageDecodeProbe> probe, BenchmarkCompilePipeline* pipeline, const std::function<std::string(std::uint64_t)>& urls, const std::function<void(std::uint64_t)>& warm_read)
  : cache(cache_value), index(index_value), quarantined(quarantined_value), cancellation(cancel), progress(reporter), trace(trace_value), decode_probe(probe), execution(pipeline), image_url(urls),
    ids(image_ids(index)), image_root(prepare_root(cache)), cpus(cpu_count(workers, execution)), local_transport(execution ? nullptr : std::make_unique<BenchmarkCurl>(cpus)),
    transport(execution ? execution->curl() : *local_transport), concurrency(transport.limit(BenchmarkCurl::Class::OpenImages)),
    active_limit(concurrency), minimum_limit(std::max<std::size_t>(1, concurrency / 8)), repair_bytes(repair_workspace(probe)),
    control_limit(std::max(group_control(std::min(kOpenImagesGroupImages, ids.size())), execution ? execution->transient_target() : group_control(kOpenImagesGroupImages) * concurrency)),
    inputs(execution, repair_bytes, transport.admission_wakeup()), active(transport, BenchmarkCurl::Class::OpenImages, cancellation, group_resources),
    directory_allowance(execution ? execution->reserve(BenchmarkResources::handles(1, true)) : BenchmarkAllowance{}), directory(open_directory(image_root)),
    cache_writer(std::clamp<std::size_t>(cache_workers, 3, 8), image_root, cancellation, [this] { active.wake(); }, execution, directory.get(), warm_read) {
  for (std::size_t begin = 0; begin < ids.size(); begin += kOpenImagesGroupImages) waiting_groups.push_back(begin);
  available.reserve(ids.size());
  prepare_completions();
 }
 ~OpenImagesAcquisition() {
  inputs.close();
  active.abandon_all([](OpenImagesTransfer&) noexcept {});
  cache_writer.retire();
 }
 OpenImagesAcquisition(const OpenImagesAcquisition&) = delete;
 OpenImagesAcquisition& operator=(const OpenImagesAcquisition&) = delete;
 AcquiredOpenImages run() {
  while (!waiting_groups.empty() || !locked_groups.empty() || !groups.empty()) {
   throw_if_benchmark_cancelled(cancellation);
   consume_cache_results();
   consume_transfer_results();
   settle_groups();
   const bool pressure = execution && execution->resource_pressure();
   if (pressure) inputs.retire_idle();
   admit_groups(pressure);
   // Ready HTTP gets a turn both before and after one bounded local scan.
   admit_http(pressure);
   const bool scanned = scan_chunk(pressure);
   admit_http(pressure);
   cache_writer.resume_deferred();
   if (waiting_groups.empty() && locked_groups.empty() && groups.empty()) break;
   if (scanned) continue;
   // Only an actual park or resource pressure retires useful idle backing.
   inputs.retire_idle();
   cache_writer.resume_deferred();
   auto deadline = Clock::now() + std::chrono::milliseconds{kBenchmarkTransferPollMilliseconds};
   if (!retries.empty() && retries.top().deadline > Clock::now()) deadline = std::min(deadline, retries.top().deadline);
   if (!locked_groups.empty()) deadline = std::min(deadline, locked_groups.top().deadline);
   active.wait_until(deadline);
  }
  std::ranges::sort(available);
  return {{"open-images", "train", image_root, "open-images-v7:train:" + std::string(kBenchmarkCatalogRevision), cached_image_selection_digest(available), available.size(), cached_bytes, all_cache_hits, {}}, std::move(available)};
 }
};
}  // namespace
AcquiredOpenImages acquire_open_images(const BenchmarkCacheLayout& cache, const NormalizedAnnotationReadView& index, std::vector<QuarantinedImage>* quarantined,
 mmltk::common::concurrency::CancellationObservation cancellation, ProgressReporter* progress, int num_workers, std::size_t cache_workers, const BenchmarkTraceSink& trace,
 std::optional<ImageDecodeProbe> decode_probe, BenchmarkCompilePipeline* execution, const std::function<std::string(std::uint64_t)>& image_url, const std::function<void(std::uint64_t)>& warm_read) {
 std::unique_ptr<BenchmarkCompilePipeline> standalone;
 if (!execution) { standalone = std::make_unique<BenchmarkCompilePipeline>(std::max(1, num_workers), std::span<const int>{}, BenchmarkExecutionLimits{}, cancellation); execution = standalone.get(); }
 OpenImagesAcquisition acquisition(cache, index, quarantined, cancellation, progress, num_workers, cache_workers, trace, decode_probe, execution, image_url, warm_read);
 return acquisition.run();
}
}  // namespace mmltk::backend::data::benchmark_internal
