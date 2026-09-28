#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/system/cpu_affinity.h"
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/detail/worker_queue.h"
namespace mmltk::backend::data::benchmark_internal {
BenchmarkResources archive_image_resources() { return BenchmarkResources::handles(1, true, 4); }
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
constexpr std::size_t kMaximumRetainedImageBufferBytes = std::size_t{2U} * 1024U * 1024U;
constexpr std::size_t kMaximumArchiveImageBytes = std::size_t{32U} * 1024U * 1024U;
[[nodiscard]] std::uint64_t checked_byte_add(std::uint64_t left, std::uint64_t right);
class CachedImageWritePool {
 struct Buffers {
  BenchmarkAllowance allowance;
  std::mutex mutex;
  std::condition_variable available;
  std::vector<std::vector<std::uint8_t>> free;
  bool stopped = false;
  ~Buffers() {
   std::vector<std::vector<std::uint8_t>>().swap(free);
   allowance.retire_workspace();
  }
 };
 std::shared_ptr<Buffers> buffers_ = std::make_shared<Buffers>();
 struct Payload {
  std::shared_ptr<Buffers> pool;
  std::vector<std::uint8_t> encoded;
  ~Payload() {
   if (encoded.capacity() > kMaximumRetainedImageBufferBytes) std::vector<std::uint8_t>().swap(encoded);
   else encoded.clear();
   { const std::lock_guard lock(pool->mutex); if (!pool->stopped) pool->free.push_back(std::move(encoded)); }
   pool->available.notify_all();
  }
 };
 std::shared_ptr<const BenchmarkEncodedImage> retain(std::vector<std::uint8_t> encoded, std::shared_ptr<const BenchmarkEncodedImage> file) {
  auto backing = std::make_shared<Payload>(buffers_, std::move(encoded));
  return BenchmarkEncodedImage::pooled(buffers_->allowance, backing, std::span<const std::uint8_t>(backing->encoded), std::move(file));
 }
 std::shared_ptr<const BenchmarkEncodedImage> publish_file(std::uint64_t id, std::span<const std::uint8_t> encoded, BenchmarkImageHeader header) {
  return BenchmarkEncodedImage::publish(directory_, id, encoded, header, cancellation_, storage_, execution_, buffers_->allowance);
 }
public:
 CachedImageWritePool(const std::size_t worker_count, const std::size_t expected_writes, std::filesystem::path output_root, CachedImageProgress progress, const std::uint64_t initially_completed,
  const mmltk::common::concurrency::CancellationObservation cancellation, CachedImageReadySink ready, StorageReservationPool* storage, BenchmarkAllowance allowance, BenchmarkCompilePipeline* execution, int directory)
     : execution_(execution), directory_(directory), storage_(output_root, {}, storage), ready_(std::move(ready)), output_root_(std::move(output_root)), progress_(std::move(progress), initially_completed, expected_writes), cancellation_(cancellation) {
  const std::size_t bounded_workers = std::min<std::size_t>(8U, worker_count);
  inline_mode_ = bounded_workers == 0U;
  if (!inline_mode_) workers_ = std::make_unique<mmltk::common::concurrency::WorkerPool>(bounded_workers, mmltk::common::system::allowed_cpu_set(), "cache_write", bounded_workers);
  const std::size_t buffer_count = inline_mode_ ? 1U : workers_->size() * 2U + 2U;
  buffers_->allowance = std::move(allowance);
  buffers_->free.resize(buffer_count);
  for (auto& buffer : buffers_->free) buffer.reserve(std::size_t{512U} * 1024U);
  try {
   if (workers_)
    for (std::size_t lane = 0; lane < workers_->size(); ++lane) workers_->enqueue_borrowed(this, lane, [](void* owner, std::size_t) { static_cast<CachedImageWritePool*>(owner)->run(); });
  } catch (...) {
   stop();
   throw;
  }
 }
 CachedImageWritePool(const CachedImageWritePool&) = delete;
 CachedImageWritePool& operator=(const CachedImageWritePool&) = delete;
 ~CachedImageWritePool() { stop(); }
 [[nodiscard]] std::vector<std::uint8_t> acquire(const std::size_t size) {
  std::unique_lock lock(buffers_->mutex);
  while (!buffers_->stopped && !cancellation_.requested() && buffers_->free.empty()) buffers_->available.wait_for(lock, std::chrono::milliseconds(100));
  throw_if_benchmark_cancelled(cancellation_);
  if (buffers_->stopped) { lock.unlock(); const std::lock_guard failure_lock(mutex_); rethrow_failure_locked(); throw std::runtime_error("cache pool stopped"); }
  std::vector<std::uint8_t> buffer = std::move(buffers_->free.back());
  buffers_->free.pop_back();
  lock.unlock();
  buffer.resize(size);
  return buffer;
 }
 void submit(const std::uint64_t image_id, std::vector<std::uint8_t> encoded, BenchmarkImageHeader header) {
  throw_if_benchmark_cancelled(cancellation_);
  if (inline_mode_) {
   const std::uint64_t bytes = encoded.size();
   if (!has_complete_image_markers(encoded)) {
    recycle(std::move(encoded));
    throw std::runtime_error("selected archive image " + std::to_string(image_id) + " is not a complete JPEG or PNG");
   }
   auto file = publish_file(image_id, encoded, header);
   auto payload = retain(std::move(encoded), std::move(file));
   if (ready_) ready_({image_id, std::pair{header.width, header.height}, false, std::move(payload)});
   written_ids_.push_back(image_id);
   written_bytes_ = checked_byte_add(written_bytes_, bytes);
   const std::uint64_t completed = written_ids_.size();
   progress_.completed(completed);
   return;
  }
  {
   const std::lock_guard lock(mutex_);
   rethrow_failure_locked();
   tasks_.push_back(Task{image_id, std::move(encoded), header});
  }
  pending_.notify_one();
 }
 void recycle(std::vector<std::uint8_t> encoded) {
  Payload returned{buffers_, std::move(encoded)};
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
 BenchmarkCompilePipeline* execution_;
 int directory_;
 StorageReservationPool storage_;
 CachedImageReadySink ready_;
 struct Task {
  std::uint64_t image_id = 0U;
  std::vector<std::uint8_t> encoded;
  BenchmarkImageHeader header;
 };
 void run() noexcept {
  bool active = false;
  try {
   while (true) {
    auto task = wait_pop_task(mutex_, pending_, stopping_, tasks_, cancellation_, [&] {
     ++active_;
     active = true;
    });
    if (!task) {
     buffers_->available.notify_all();
     finished_.notify_all();
     return;
    }
    const std::uint64_t bytes = task->encoded.size();
    if (!has_complete_image_markers(task->encoded)) { throw std::runtime_error("selected archive image " + std::to_string(task->image_id) + " is not a complete JPEG or PNG"); }
    throw_if_benchmark_cancelled(cancellation_);
    auto file = publish_file(task->image_id, task->encoded, task->header);
    auto payload = retain(std::move(task->encoded), std::move(file));
    if (ready_) ready_({task->image_id, std::pair{task->header.width, task->header.height}, false, std::move(payload)});
    std::uint64_t completed = 0U;
    {
     const std::lock_guard lock(mutex_);
     written_ids_.push_back(task->image_id);
     written_bytes_ = checked_byte_add(written_bytes_, bytes);
     completed = written_ids_.size();
    }
    progress_.completed(completed);
    {
     const std::lock_guard lock(mutex_);
     --active_;
     active = false;
    }
    finished_.notify_one();
   }
  } catch (...) {
   {
    const std::lock_guard lock(mutex_);
    if (active) --active_;
    if (!failure_) failure_ = std::current_exception();
    stopping_ = true;
    tasks_.clear();
   }
   pending_.notify_all();
   { const std::lock_guard buffer_lock(buffers_->mutex); buffers_->stopped = true; }
   buffers_->available.notify_all();
   finished_.notify_all();
  }
 }
 void rethrow_failure_locked() const {
  if (failure_) { std::rethrow_exception(failure_); }
 }
 void join() noexcept { workers_.reset(); }
 void stop() noexcept {
  {
   const std::lock_guard lock(mutex_);
   stopping_ = true;
   tasks_.clear();
  }
  pending_.notify_all();
  { const std::lock_guard buffer_lock(buffers_->mutex); buffers_->stopped = true; }
  buffers_->available.notify_all();
  finished_.notify_all();
  join();
 }
 std::filesystem::path output_root_;
 CachedImageWriteProgress progress_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 std::mutex mutex_;
 std::condition_variable pending_;
 std::condition_variable finished_;
 std::deque<Task> tasks_;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> workers_;
 std::vector<std::uint64_t> written_ids_;
 std::uint64_t written_bytes_ = 0U;
 std::size_t active_ = 0U;
 bool stopping_ = false;
 bool inline_mode_ = false;
 std::exception_ptr failure_;
};
[[nodiscard]] std::uint64_t checked_byte_add(const std::uint64_t left, const std::uint64_t right) { return checked_add(left, right, "cached image byte total overflow"); }
[[nodiscard]] std::vector<std::uint64_t> available_image_ids(const std::span<const std::uint64_t> requested, const std::span<const CachedImageRejection> quarantined) {
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
[[nodiscard]] CachedImageDirectory make_cached_image_directory(const std::string& source, const std::string& shard, std::filesystem::path output_root, const std::string& identity,
 const std::span<const std::uint64_t> selected_image_ids, const std::uint64_t image_bytes, const bool cache_hit, std::vector<CachedImageRejection> quarantined) {
 const std::vector<std::uint64_t> available = available_image_ids(selected_image_ids, quarantined);
 return CachedImageDirectory{source, shard, std::move(output_root), identity, cached_image_selection_digest(available), available.size(), image_bytes, cache_hit, std::move(quarantined)};
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
}  // namespace
std::string cached_image_selection_digest(const std::span<const std::uint64_t> image_ids) {
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(image_ids.data()), image_ids.size_bytes())));
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
void invalidate_cached_image_proofs(const std::filesystem::path& root) {
 remove_cache_path(root / ".complete.json");
 const auto proofs = root / ".recipe-proofs";
 const auto status = std::filesystem::symlink_status(proofs);
 if (!std::filesystem::exists(status)) return;
 if (!std::filesystem::is_directory(status)) throw std::runtime_error("benchmark recipe proof root is not an ordinary directory");
 for (const auto& entry : std::filesystem::directory_iterator(proofs)) remove_cache_path(entry.path());
}
bool validate_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, const std::string_view identity,
 const std::span<const std::uint64_t> expected_image_ids, std::uint64_t* image_bytes, mmltk::common::concurrency::CancellationObservation cancel_requested, const BenchmarkTraceSink& trace,
 std::vector<CachedImageRejection>* quarantined) {
 try {
  if (expected_image_ids.empty() || !std::filesystem::is_regular_file(completion_path)) { return false; }
  const nlohmann::json manifest = read_json_file(completion_path);
  if (manifest.value("schema_version", 0U) != kBenchmarkCacheSchemaVersion || !manifest.value("complete", false) || manifest.value("identity", std::string{}) != identity) { return false; }
  std::vector<CachedImageRejection> cached_quarantine;
  std::vector<std::uint64_t> available;
  std::span<const std::uint64_t> validated_ids = expected_image_ids;
  if (quarantined != nullptr) {
   const std::size_t requested_count = manifest.value("requested_image_count", manifest.value("image_count", std::size_t{0U}));
   const std::string requested_digest = manifest.value("requested_selection_sha256", manifest.value("selection_sha256", std::string{}));
   if (requested_count != expected_image_ids.size() || requested_digest != cached_image_selection_digest(expected_image_ids)) { return false; }
   if (const auto records = manifest.find("quarantined"); records != manifest.end()) {
    cached_quarantine.reserve(records->size());
    const bool parsed = parse_quarantined_manifest_records(
     *records, expected_image_ids, [&](const std::uint64_t image_id, std::string reason) { cached_quarantine.push_back(CachedImageRejection{image_id, std::move(reason)}); });
    if (!parsed) { return false; }
    std::ranges::sort(cached_quarantine, {}, &CachedImageRejection::image_id);
    if (std::ranges::adjacent_find(cached_quarantine, {}, &CachedImageRejection::image_id) != cached_quarantine.end()) { return false; }
   }
   available = available_image_ids(expected_image_ids, cached_quarantine);
   validated_ids = available;
  }
  if (manifest.value("image_count", 0ULL) != validated_ids.size() || manifest.value("selection_sha256", std::string{}) != cached_image_selection_digest(validated_ids)) { return false; }
  if (!std::filesystem::is_directory(root)) { return false; }
  throw_if_benchmark_cancelled(cancel_requested);
  const std::uint64_t total_bytes = manifest.value("image_bytes", std::uint64_t{0U});
  if (total_bytes == 0U) { return false; }
  if (image_bytes != nullptr) { *image_bytes = total_bytes; }
  if (quarantined != nullptr) { *quarantined = std::move(cached_quarantine); }
  trace_benchmark_event(
   trace, "benchmark.images.cache_hit", [&] { return nlohmann::json{{"identity", identity}, {"images", validated_ids.size()}, {"quarantined", quarantined != nullptr ? quarantined->size() : 0U}}; });
  return true;
 } catch (const std::exception&) {
  throw_if_benchmark_cancelled(cancel_requested);
  return false;
 }
}
void complete_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, const std::string_view identity,
 const std::span<const std::uint64_t> expected_image_ids, const std::uint64_t image_bytes, const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
 const std::span<const CachedImageRejection> quarantined, StorageReservationPool* storage) {
 const std::vector<std::uint64_t> available = available_image_ids(expected_image_ids, quarantined);
 nlohmann::json quarantined_records = nlohmann::json::array();
 for (const CachedImageRejection& image : quarantined) { quarantined_records.push_back({{"image_id", image.image_id}, {"reason", image.reason}}); }
 nlohmann::json manifest{
  {"schema_version", kBenchmarkCacheSchemaVersion}, {"complete", true}, {"identity", identity}, {"image_count", available.size()}, {"image_bytes", image_bytes},
  {"selection_sha256", cached_image_selection_digest(available)}
 };
 if (!quarantined.empty()) {
  manifest["requested_image_count"] = expected_image_ids.size();
  manifest["requested_selection_sha256"] = cached_image_selection_digest(expected_image_ids);
  manifest["quarantined"] = std::move(quarantined_records);
 }
 throw_if_benchmark_cancelled(cancellation);
 write_json_atomically(completion_path, manifest, cancellation, storage);
 trace_benchmark_event(
  trace, "benchmark.images.complete", [&] { return nlohmann::json{{"root", root.string()}, {"identity", identity}, {"images", available.size()}, {"quarantined", quarantined.size()}}; });
}
CachedImageDirectory extract_selected_archive_images(ArchiveExtractionRequest request) {
 if (request.selected_image_ids.empty() || !request.image_id_parser || request.source.empty() || request.shard.empty() || request.source_identity.empty()) {
  throw std::runtime_error("benchmark archive extraction configuration is incomplete");
 }
 if (!std::ranges::is_sorted(request.selected_image_ids) || std::ranges::adjacent_find(request.selected_image_ids) != request.selected_image_ids.end()) {
  throw std::runtime_error("benchmark archive extraction IDs must be sorted and unique");
 }
 prepare_cached_image_directory(request.output_root);
 const std::filesystem::path completion = request.completion_path.empty() ? request.output_root / ".complete.json" : request.completion_path;
 const std::string& identity = request.source_identity;
 std::uint64_t image_bytes = 0U;
 std::vector<CachedImageRejection> quarantined;
 if (validate_cached_image_group(
      request.output_root, completion, identity, request.selected_image_ids, &image_bytes, request.cancel_requested, request.trace, request.quarantine_unavailable ? &quarantined : nullptr)) {
  if (request.image_ready)
   for (const auto id : request.selected_image_ids) {
    if (!std::ranges::binary_search(quarantined, id, {}, &CachedImageRejection::image_id)) request.image_ready({id});
   }
  if (request.progress) { request.progress(request.selected_image_ids.size(), request.selected_image_ids.size()); }
  return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, true, std::move(quarantined));
 }
 std::optional<BenchmarkImageDecoder> default_decoder;
 if (!request.validator) {
  default_decoder.emplace();
  request.validator = [&](std::uint64_t, std::span<const std::uint8_t> bytes) { return default_decoder->read_header(bytes); };
 }
 const std::unordered_set<std::uint64_t> selected(request.selected_image_ids.begin(), request.selected_image_ids.end());
 std::unordered_set<std::uint64_t> completed;
 completed.reserve(request.selected_image_ids.size());
 std::unordered_set<std::uint64_t> unavailable;
 unavailable.reserve(64U);
 auto directory_allowance = request.execution ? request.execution->reserve(BenchmarkResources::handles(2, true, 2), request.parent_allowance) : BenchmarkAllowance{};
 BenchmarkAllowance stream_allowance;
 const int output_descriptor = ::open(request.output_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
 if (output_descriptor < 0) { throw errno_error("cannot open cached benchmark image directory", request.output_root.string()); }
 FileHandle output_directory(output_descriptor);
 if (request.activity) { request.activity("Checking individually cached images"); }
 std::size_t inspected = 0U;
 for (const std::uint64_t image_id : request.selected_image_ids) {
  throw_if_benchmark_cancelled(request.cancel_requested);
  if (request.trace && inspected != 0U && inspected % 128U == 0U) {
   trace_benchmark_event(request.trace, "benchmark.images.cache_scan", [&] {
    return nlohmann::json{
     {"source", request.source}, {"shard", request.shard}, {"inspected_images", inspected}, {"reused_images", completed.size()}, {"total_images", request.selected_image_ids.size()}
    };
   });
  }
  if (request.trace) { ++inspected; }
  auto allowance = request.execution ? request.execution->reserve(BenchmarkResources::handles(1, true), directory_allowance) : BenchmarkAllowance{};
  std::shared_ptr<const BenchmarkEncodedImage> payload;
  try { payload = BenchmarkEncodedImage::open(output_directory.get(), image_id, request.validator, request.cancel_requested, request.execution, std::move(allowance), request.execution ? request.execution->image_input(request.output_root, image_id) : nullptr); }
  catch (const InvalidImageError&) {
   invalidate_cached_image_proofs(request.output_root); remove_cache_path(cached_image_path(request.output_root, image_id)); continue;
  }
  if (payload) {
   image_bytes = checked_byte_add(image_bytes, payload->encoded().size());
   const auto dimensions = std::pair{payload->header().width, payload->header().height};
   if (request.image_ready) request.image_ready({image_id, dimensions, false, std::move(payload)});
   completed.emplace(image_id);
  }
 }
 if (request.progress) { request.progress(completed.size(), selected.size()); }
 trace_benchmark_event(request.trace, "benchmark.images.cache_reuse",
  [&] { return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_images", selected.size()}, {"reused_images", completed.size()}, {"reused_bytes", image_bytes}}; });
 if (completed.size() == selected.size()) {
  complete_cached_image_group(request.output_root, completion, identity, request.selected_image_ids, image_bytes, request.cancel_requested, request.trace, quarantined, request.storage);
  return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, false, std::move(quarantined));
 }
 if (request.execution) request.cache_write_workers = 0;
 const std::size_t pending_writes = selected.size() - completed.size() - unavailable.size();
 if (request.activity) {
  request.activity(
   request.cache_write_workers == 0U ? "Preparing inline selected-image cache writes" : "Preparing bounded cache-write queue with " + std::to_string(request.cache_write_workers) + " workers");
 }
 std::unordered_set<std::uint64_t> scheduled;
 scheduled.reserve(pending_writes);
 std::unordered_map<std::uint64_t, std::uint64_t> required_positions;
 required_positions.reserve(pending_writes);
 BenchmarkArchive reader(request.archive_path, request.execution, checked_byte_add(64ULL << 20, request.validator_workspace_bytes), directory_allowance, request.decompression_workers, false);
 stream_allowance = reader.allowance().split_storage(64ULL << 20);
 const auto archive_work = [&](const std::function<void()>& work) { reader.cpu(work); };
 CachedImageWritePool write_pool(request.cache_write_workers, pending_writes, request.output_root, request.progress, completed.size(), request.cancel_requested, request.image_ready, request.storage, stream_allowance, request.execution, output_directory.get());
 std::vector<std::uint8_t> encoded;
 if (request.activity) { request.activity("Scanning archive headers for selected images"); }
 bool extraction_announced = false;
 std::uint64_t inspected_headers = 0U;
 while (completed.size() + scheduled.size() + unavailable.size() != selected.size()) {
  throw_if_benchmark_cancelled(request.cancel_requested);
  if (!reader.next(request.cancel_requested)) break;
  if (request.trace && (++inspected_headers % 1024U == 0U)) {
   trace_benchmark_event(request.trace, "benchmark.archive.scan", [&] {
    return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_headers", inspected_headers}, {"scheduled_images", scheduled.size()}, {"reused_images", completed.size()}};
   });
  }
  const char* pathname = reader.member().c_str();
  const std::optional<std::uint64_t> image_id = pathname != nullptr ? request.image_id_parser(pathname) : std::nullopt;
  if (image_id && selected.contains(*image_id)) {
   const auto [position, inserted] = required_positions.emplace(*image_id, reader.position());
   if (!inserted && position->second != reader.position()) throw BenchmarkArchiveError("conflicting requested benchmark image identity");
  }
  if (!image_id || !selected.contains(*image_id) || completed.contains(*image_id) || scheduled.contains(*image_id) || unavailable.contains(*image_id)) {
   continue;
  }
  reader.require_regular(kMaximumArchiveImageBytes);
  const auto entry_size = reader.size();
  if (entry_size <= 0 || static_cast<std::uint64_t>(entry_size) > kMaximumArchiveImageBytes) { throw std::runtime_error("selected benchmark archive image has an invalid size"); }
  if (!extraction_announced && request.activity) {
   request.activity("Extracting selected image payloads into the cache-write queue");
   extraction_announced = true;
  }
  encoded = write_pool.acquire(checked_cast<std::size_t>(entry_size, "benchmark archive image size overflow"));
  reader.read_into(encoded, request.cancel_requested);
  BenchmarkImageHeader header;
  {
   try {
    archive_work([&] { header = request.validator(*image_id, encoded); });
   } catch (const std::bad_alloc&) { throw; }
   catch (const std::exception& error) {
    throw_if_benchmark_cancelled(request.cancel_requested);
    if (is_benchmark_capacity_failure(error)) throw;
    trace_benchmark_event(request.trace, "benchmark.images.validation_failed",
     [&] { return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"member", pathname}, {"image_id", *image_id}, {"bytes", encoded.size()}, {"reason", error.what()}}; });
    if (!request.quarantine_unavailable) { throw; }
    quarantined.push_back(CachedImageRejection{*image_id, error.what()});
    unavailable.emplace(*image_id);
    write_pool.recycle(std::move(encoded));
    continue;
   }
  }
  // New members cannot invalidate existing subsets; replacements already invalidated during the scan.
  if (std::filesystem::exists(cached_image_path(request.output_root, *image_id))) invalidate_cached_image_proofs(request.output_root);
  try {
   write_pool.submit(*image_id, std::move(encoded), header);
   scheduled.emplace(*image_id);
  } catch (const InvalidImageError& error) {
   if (!request.quarantine_unavailable) throw;
   invalidate_cached_image_proofs(request.output_root);
   remove_cache_path(cached_image_path(request.output_root, *image_id));
   quarantined.push_back({*image_id, error.what()}); unavailable.emplace(*image_id);
  }
 }
 if (request.activity && request.cache_write_workers != 0U) { request.activity("Draining selected image cache writes"); }
 for (const std::uint64_t image_id : write_pool.finish(&image_bytes)) { completed.emplace(image_id); }
 std::vector<std::uint64_t> missing;
 if (completed.size() + unavailable.size() != selected.size()) {
  missing.reserve(selected.size() - completed.size() - unavailable.size());
  for (const std::uint64_t image_id : request.selected_image_ids) {
   if (!completed.contains(image_id) && !unavailable.contains(image_id)) { missing.push_back(image_id); }
  }
  if (!request.quarantine_unavailable) { throw std::runtime_error(missing_archive_images_message(missing)); }
  for (const std::uint64_t image_id : missing) { quarantined.push_back(CachedImageRejection{image_id, "verified official archive does not contain selected image"}); }
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
 complete_cached_image_group(request.output_root, completion, identity, request.selected_image_ids, image_bytes, request.cancel_requested, request.trace, quarantined, request.storage);
 return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, false, std::move(quarantined));
}
}  // namespace mmltk::backend::data::benchmark_internal
