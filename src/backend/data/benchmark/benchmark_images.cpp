#include "src/backend/data/detail/storage_reservation.h"
#include <condition_variable>
#include <system_error>
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_images.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include "src/backend/data/benchmark/benchmark_hash.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileExecution;
using mmltk::backend::data::detail::CompileAllowance;
using mmltk::backend::data::detail::CompileResources;
CompileResources archive_image_resources() { return CompileResources::handles(1, true, 4); }
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
constexpr std::size_t kMaximumArchiveImageBytes = BenchmarkEncodedImage::normal_window_bytes;
[[nodiscard]] std::uint64_t checked_byte_add(std::uint64_t left, std::uint64_t right);
class CachedImageWritePool {
 struct Buffers {
  std::mutex mutex;
  std::size_t free = 0;
  CompileExecution* execution = nullptr;
 };

public:
 struct Payload {
  std::shared_ptr<Buffers> pool;
  CompileAllowance allowance, output;
  bool restored_decoder = false;
  std::vector<std::uint8_t> standalone;
  std::span<std::uint8_t> encoded;
  ~Payload() {
   allowance = {};
   if (pool) {
    {
     const std::lock_guard lock(pool->mutex);
     ++pool->free;
    }
    if (pool->execution) pool->execution->notify_admission_change();
   }
  }
 };
 using Input = std::shared_ptr<Payload>;
 CachedImageWritePool(std::filesystem::path output_root, mmltk::common::concurrency::CancellationObservation cancellation, CachedImageReadySink ready,
  std::function<void(const CachedImageReady&, std::exception_ptr, BenchmarkSourceGeneration)> settled, detail::StorageReservationPool* storage, CompileAllowance parent,
  BenchmarkCompilePipeline* execution, int directory, bool retain_encoded, BenchmarkArchive& reader, const BenchmarkTraceSink& trace)
     : execution_(execution),
       directory_(directory),
       retain_encoded_(retain_encoded),
       storage_(output_root, {}, storage),
       ready_(std::move(ready)),
       settled_(std::move(settled)),
       cancellation_(cancellation),
       parent_(std::move(parent)),
       reader_(reader),
       trace_(trace),
       writes_(execution ? std::clamp<std::size_t>(execution->execution().workers() * 2, 2, 8) : 1, execution ? &execution->execution() : nullptr, [this] {
        if (execution_ && execution_->execution().resource_pressure()) reader_.suspend(parent_);
       }) {
  buffers_->free = execution ? std::clamp<std::size_t>(execution->execution().workers() * 2, 2, 8) : 1;
  buffers_->execution = execution ? &execution->execution() : nullptr;
  if (execution)
   readiness_ = std::make_unique<CompileExecution::Readiness>(execution->execution(), [this](CompileExecution::Readiness::Changes changes) {
    if (!changes.retry && !changes.interrupted) return;
    {
     const std::lock_guard lock(resource_mutex_);
     ++resource_generation_;
    }
    resource_changed_.notify_all();
   });
 }
 ~CachedImageWritePool() {
  try {
   reader_.suspend(parent_);
   writes_.drain();
  } catch (...) {}
 }
 [[nodiscard]] Input acquire(std::size_t size) {
  // Reserve a publication record before retaining another encoded input.
  writes_.make_room();
  for (;;) {
   const auto observed = execution_ ? execution_->execution().admission_generation() : 0;
   writes_.reap();
   throw_if_benchmark_cancelled(cancellation_);
   std::unique_lock lock(buffers_->mutex);
   if (buffers_->free) {
    --buffers_->free;
    break;
   }
   lock.unlock();
   reader_.suspend(parent_);
   if (!execution_) throw std::logic_error("standalone image input did not retire synchronously");
   execution_->execution().wait_for_admission_change(observed);
   if (execution_)
    if (auto failure = execution_->execution().interruption()) std::rethrow_exception(failure);
  }
  Input input;
  try {
   input = std::make_shared<Payload>();
  } catch (...) {
   const std::lock_guard lock(buffers_->mutex);
   ++buffers_->free;
   throw;
  }
  input->pool = buffers_;
  if (execution_) {
   auto& owner = execution_->execution();
   std::unique_ptr<detail::CompileResourceWait> pressure;
   try {
    for (;;) {
     std::uint64_t observed;
     {
      const std::lock_guard lock(resource_mutex_);
      observed = resource_generation_;
     }
     bool retry_after_quiesce = false;
     {
      CompileExecution::Readiness::Attempt attempt(readiness_.get());
      pressure.reset();
      writes_.reap();
      const bool active_decoder = reader_.retained_workspace_bytes() != 0;
      // The output descriptor and pinned input are unaccepted local custody.
      // A paused decoder joins that same input lineage nonblockingly before
      // either owner can wait behind a newly ready exclusive consumer.
      auto output = owner.try_reserve(CompileResources::handles(1, true), parent_);
      if (output) {
       input->output = std::move(*output);
       auto granted = owner.try_reserve({.bytes = BenchmarkEncodedImage::normal_window_bytes,
                                         .continuation_descriptors = active_decoder ? 0U : 1U,
                                         .supplied_workspace = true,
                                         .workspace_class = detail::CompileWorkClass::Extraction,
                                         .workspace_window = detail::CompileWorkspaceWindow::SharedSource},
        parent_);
       if (granted) {
        input->allowance = std::move(*granted);
        if (!active_decoder) trace_benchmark_event(trace_, "benchmark.images.decoder_admission", [&] { return nlohmann::json{{"member", reader_.member()}, {"bytes", size}}; });
        if (active_decoder || reader_.try_resume(0, input->allowance)) {
         input->restored_decoder = !active_decoder;
         attempt.commit();
         break;
        }
       }
      }
      reader_.suspend(parent_);
      input->allowance = {};
      input->output = {};
      // Self-induced pressure must quiesce even before a resource waiter exists.
      trace_benchmark_event(trace_, "benchmark.images.input_wait", [&] { return nlohmann::json{{"member", reader_.member()}, {"bytes", size}}; });
      pressure = owner.defer_resources();
      // Retiring our live decoder can itself make the complete draw fit. Probe
      // once again; subsequent failed draws wait only for external readiness.
      retry_after_quiesce = active_decoder;
     }
     if (retry_after_quiesce) continue;
     std::unique_lock lock(resource_mutex_);
     resource_changed_.wait(lock, [&] { return resource_generation_ != observed || owner.interruption(); });
     if (auto failure = owner.interruption()) std::rethrow_exception(failure);
    }
   } catch (...) {
    reader_.suspend(parent_);
    throw;
   }
   const auto bytes = input->allowance.workspace();
   input->encoded = {reinterpret_cast<std::uint8_t*>(bytes.data()), size};
  } else {
   input->standalone.resize(size);
   input->encoded = input->standalone;
  }
  return input;
 }
 void read(const Input& input, const BenchmarkArchive::MemberPosition* position = nullptr) {
  // A failed lease may have retired the decoder. Its restoration must then
  // belong to the admitted input, including an exceptional input under a small
  // configured budget. Retire this ancestry before handing bytes to a writer.
  try {
   if (position && !reader_.seek(*position, cancellation_)) throw BenchmarkArchiveError("unaccepted archive image disappeared from its opened generation");
   reader_.read_into(input->encoded, cancellation_);
  } catch (...) {
   if (input->restored_decoder) reader_.suspend(parent_);
   throw;
  }
  if (input->restored_decoder) reader_.suspend(parent_);
 }
 void submit(std::uint64_t id, Input admitted_input, BenchmarkImageHeader header, BenchmarkSourceGeneration generation) {
  throw_if_benchmark_cancelled(cancellation_);
  if (!has_complete_image_markers(admitted_input->encoded)) throw InvalidImageError("selected archive image " + std::to_string(id) + " is not a complete JPEG or PNG");
  struct Publication {
   Input input;
   BenchmarkEncodedImage::Ptr file, fact;
  };
  auto publication = std::make_shared<Publication>();
  publication->input = std::move(admitted_input);
  const auto write = [this, id, header, publication] {
   const auto& input = publication->input;
   publication->file = BenchmarkEncodedImage::publish(directory_, id, input->encoded, header, cancellation_, storage_, execution_ ? &execution_->execution() : nullptr, input->allowance);
  };
  const auto completed = [this, id, header, publication](std::exception_ptr failure) {
   auto input = std::exchange(publication->input, {});
   input->output.retire_descriptors();
   if (failure) std::rethrow_exception(failure);
   publication->fact = publication->file->header_only();
   // Published file backing lets synchronous consumers release an exceptional
   // input before reserving their own decoder workspace.
   auto payload = retain_encoded_ && !input->allowance.exceptional_workspace() && (!execution_ || !execution_->execution().resource_pressure())
                   ? BenchmarkEncodedImage::pooled(input->allowance, input, input->encoded, std::move(publication->file))
                   : std::move(publication->file);
   input.reset();
   if (ready_) ready_({id, std::pair{header.width, header.height}, execution_ != nullptr, std::move(payload)});
  };
  const auto settled = [this, id, header, publication, generation](
                        std::exception_ptr failure) { settled_({id, std::pair{header.width, header.height}, true, publication->fact}, failure, generation); };
  if (execution_) {
   // Prepare the fallible callback allocation before physical acceptance.
   std::function<void(std::exception_ptr)> settlement = settled;
   auto& owner = execution_->execution();
   for (;;) {
    const auto observed = owner.admission_generation();
    if (auto physical = execution_->files().try_submit({.completed = completed, .work = write})) {
     writes_.push(std::move(*physical), std::move(settlement));
     break;
    }
    // Full is unaccepted: no physical callback borrows this input yet. Retain
    // it while only file records are busy; real resource pressure may reject
    // this local attempt and replay the exact member from the same opened inode.
    reader_.suspend(parent_);
    trace_benchmark_event(trace_, "benchmark.images.file_wait", [&] { return nlohmann::json{{"image_id", id}, {"member", reader_.member()}}; });
    if (owner.resource_pressure()) {
     const auto position = reader_.member_position();
     const auto size = publication->input->encoded.size();
     publication->input.reset();
     // Returning our own resources cannot authorize a read/reject loop. Wait
     // for an actual file record before drawing the input/decoder again. The
     // generation is sampled before the predicate to retain every free/stop wake.
     for (;;) {
      const auto changed = owner.admission_generation();
      if (execution_->files().has_capacity()) break;
      owner.wait_for_admission_change(changed);
     }
     publication->input = acquire(size);
     read(publication->input, &position);
    } else
     owner.wait_for_admission_change(observed);
   }
  } else {
   std::exception_ptr failure;
   try {
    write();
    auto complete = completed;
    complete({});
   } catch (...) { failure = std::current_exception(); }
   settled(failure);
  }
 }
 void finish() { writes_.drain(); }

private:
 BenchmarkCompilePipeline* execution_;
 int directory_;
 bool retain_encoded_;
 detail::StorageReservationPool storage_;
 CachedImageReadySink ready_;
 std::function<void(const CachedImageReady&, std::exception_ptr, BenchmarkSourceGeneration)> settled_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 CompileAllowance parent_;
 BenchmarkArchive& reader_;
 const BenchmarkTraceSink& trace_;
 std::shared_ptr<Buffers> buffers_ = std::make_shared<Buffers>();
 std::mutex resource_mutex_;
 std::condition_variable resource_changed_;
 std::uint64_t resource_generation_ = 0;
 std::unique_ptr<CompileExecution::Readiness> readiness_;
 BenchmarkCompletionWindow writes_;
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
 } catch (const std::exception& error) {
  if (is_benchmark_capacity_failure(error) || dynamic_cast<const std::system_error*>(&error)) throw;
  throw_if_benchmark_cancelled(cancel_requested);
  return false;
 }
}
void complete_cached_image_group(const std::filesystem::path& root, const std::filesystem::path& completion_path, const std::string_view identity,
 const std::span<const std::uint64_t> expected_image_ids, const std::uint64_t image_bytes, const mmltk::common::concurrency::CancellationObservation cancellation, const BenchmarkTraceSink& trace,
 const std::span<const CachedImageRejection> quarantined, detail::StorageReservationPool* storage) {
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
  bool healthy = true;
  BenchmarkCompletionWindow pixels(request.execution ? std::clamp<std::size_t>(request.execution->execution().workers() * 2, 2, 64) : 1);
  if (request.image_ready)
   for (const auto id : request.selected_image_ids) {
    if (std::ranges::binary_search(quarantined, id, {}, &CachedImageRejection::image_id)) continue;
    pixels.make_room();
    const auto generation = request.execution ? request.execution->image_generation(request.output_root, id) : 0;
    request.image_ready({id});
    pixels.push(request.execution ? request.execution->image_completion(request.output_root, id) : CompileExecution::Completion{}, [&, id, generation](std::exception_ptr failure) {
     if (request.execution && request.execution->image_generation(request.output_root, id) != generation) throw std::runtime_error("custom cached publication generation retired");
     if (!failure) return;
     try {
      std::rethrow_exception(failure);
     } catch (const BenchmarkImageReadError&) {
     } catch (const InvalidImageError&) {}
     healthy = false;
     if (request.execution) request.execution->retire_image(request.output_root, id);
     invalidate_cached_image_proofs(request.output_root);
     remove_cache_path(cached_image_path(request.output_root, id));
    });
   }
  pixels.drain();
  if (healthy) {
   if (request.progress) { request.progress(request.selected_image_ids.size(), request.selected_image_ids.size()); }
   return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, true, std::move(quarantined));
  }
  image_bytes = 0;
  quarantined.clear();
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
 CachedImageWriteProgress resolved_progress(request.progress, 0, selected.size());
 std::unique_ptr<BenchmarkArchive> archive_reader;
 CompileAllowance directory_allowance;
 const auto quiesce = [&] {
  if (archive_reader && request.execution && request.execution->execution().resource_pressure()) archive_reader->suspend(directory_allowance);
 };
 BenchmarkCompletionWindow pixels(
  request.execution ? std::clamp<std::size_t>(request.execution->execution().workers() * 2, 2, 64) : 1, request.execution ? &request.execution->execution() : nullptr, quiesce);
 const auto publish = [&](const CachedImageReady& ready, bool source_read, bool delivered = false, std::exception_ptr publication_error = {}, std::optional<BenchmarkSourceGeneration> admitted = {}) {
  pixels.make_room();
  const auto id = ready.image_id;
  if (publication_error && !ready.payload) std::rethrow_exception(publication_error);
  const auto bytes = ready.payload ? ready.payload->size() : std::filesystem::file_size(cached_image_path(request.output_root, id));
  const auto generation = admitted ? *admitted : request.execution ? request.execution->image_generation(request.output_root, id) : 0;
  CompileExecution::Completion image_completion;
  try {
   if (publication_error) std::rethrow_exception(publication_error);
   if (!delivered && request.image_ready) request.image_ready(ready);
   if (request.execution) image_completion = request.execution->image_completion(request.output_root, id);
  } catch (const InvalidImageError&) {
   CompileExecution::CompletionSource rejected;
   rejected.settle(std::current_exception());
   image_completion = rejected.completion();
  }
  pixels.push(std::move(image_completion), [&, id, bytes, source_read, generation](std::exception_ptr failure) {
   if (request.execution && request.execution->image_generation(request.output_root, id) != generation) throw std::runtime_error("custom image publication generation retired");
   if (failure) {
    std::string reason;
    try {
     std::rethrow_exception(failure);
    } catch (const BenchmarkImageReadError& error) { reason = error.what(); } catch (const InvalidImageError& error) {
     reason = error.what();
    }
    if (request.execution) request.execution->retire_image(request.output_root, id);
    invalidate_cached_image_proofs(request.output_root);
    remove_cache_path(cached_image_path(request.output_root, id));
    if (source_read) {
     if (!request.quarantine_unavailable) std::rethrow_exception(failure);
     quarantined.push_back({id, std::move(reason)});
     unavailable.emplace(id);
     resolved_progress.completed(completed.size() + unavailable.size());
    }
    return;
   }
   if (completed.emplace(id).second) image_bytes = checked_byte_add(image_bytes, bytes);
   resolved_progress.completed(completed.size() + unavailable.size());
  });
 };
 directory_allowance = request.execution ? request.execution->execution().reserve(CompileResources::handles(1, true, 3), request.parent_allowance) : CompileAllowance{};
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
  auto allowance = request.execution ? request.execution->execution().reserve(CompileResources::handles(1, true), directory_allowance) : CompileAllowance{};
  std::shared_ptr<const BenchmarkEncodedImage> payload;
  try {
   payload = BenchmarkEncodedImage::open(output_directory.get(), image_id, request.validator, request.cancel_requested, (request.execution ? &request.execution->execution() : nullptr),
    std::move(allowance), request.execution ? request.execution->image_input(request.output_root, image_id) : nullptr);
  } catch (const InvalidImageError&) {
   invalidate_cached_image_proofs(request.output_root);
   remove_cache_path(cached_image_path(request.output_root, image_id));
   continue;
  }
  if (payload) {
   const auto dimensions = std::pair{payload->header().width, payload->header().height};
   publish({image_id, dimensions, true, std::move(payload)}, false);
  }
 }
 pixels.drain();
 if (request.progress) { request.progress(completed.size(), selected.size()); }
 trace_benchmark_event(request.trace, "benchmark.images.cache_reuse",
  [&] { return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_images", selected.size()}, {"reused_images", completed.size()}, {"reused_bytes", image_bytes}}; });
 if (completed.size() == selected.size()) {
  complete_cached_image_group(request.output_root, completion, identity, request.selected_image_ids, image_bytes, request.cancel_requested, request.trace, quarantined, request.storage);
  return make_cached_image_directory(request.source, request.shard, std::move(request.output_root), identity, request.selected_image_ids, image_bytes, false, std::move(quarantined));
 }
 const std::size_t pending_writes = selected.size() - completed.size() - unavailable.size();
 if (request.activity) request.activity("Preparing bounded selected-image cache writes");
 std::unordered_set<std::uint64_t> scheduled;
 scheduled.reserve(pending_writes);
 std::unordered_map<std::uint64_t, std::uint64_t> required_positions;
 required_positions.reserve(pending_writes);
 archive_reader =
  std::make_unique<BenchmarkArchive>(request.archive_path, (request.execution ? &request.execution->execution() : nullptr), 0, directory_allowance, request.decompression_workers, true);
 auto& reader = *archive_reader;
 CachedImageWritePool write_pool(request.output_root, request.cancel_requested, request.image_ready,
  [&](const CachedImageReady& ready, std::exception_ptr error, BenchmarkSourceGeneration generation) { publish(ready, true, true, error, generation); }, request.storage, directory_allowance,
  request.execution, output_directory.get(), request.retain_encoded, reader, request.trace);
 CachedImageWritePool::Input input;
 if (request.activity) { request.activity("Scanning archive headers for selected images"); }
 bool extraction_announced = false;
 std::uint64_t inspected_headers = 0U;
 while (scheduled.size() < pending_writes) {
  throw_if_benchmark_cancelled(request.cancel_requested);
  if (!reader.next(request.cancel_requested)) break;
  if (request.trace && (++inspected_headers % 1024U == 0U)) {
   trace_benchmark_event(request.trace, "benchmark.archive.scan", [&] {
    return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"inspected_headers", inspected_headers}, {"scheduled_images", scheduled.size()}, {"reused_images", completed.size()}};
   });
  }
  const std::string pathname = reader.member();
  const std::optional<std::uint64_t> image_id = request.image_id_parser(pathname);
  if (image_id && selected.contains(*image_id)) {
   const auto [position, inserted] = required_positions.emplace(*image_id, reader.position());
   if (!inserted && position->second != reader.position()) throw BenchmarkArchiveError("conflicting requested benchmark image identity");
  }
  if (!image_id || !selected.contains(*image_id) || completed.contains(*image_id) || scheduled.contains(*image_id) || unavailable.contains(*image_id)) { continue; }
  reader.require_regular(kMaximumArchiveImageBytes);
  const auto entry_size = reader.size();
  if (entry_size <= 0 || static_cast<std::uint64_t>(entry_size) > kMaximumArchiveImageBytes) { throw std::runtime_error("selected benchmark archive image has an invalid size"); }
  if (!extraction_announced && request.activity) {
   request.activity("Extracting selected image payloads into the cache-write queue");
   extraction_announced = true;
  }
  input = write_pool.acquire(checked_cast<std::size_t>(entry_size, "benchmark archive image size overflow"));
  const auto encoded = input->encoded;
  write_pool.read(input);
  BenchmarkImageHeader header;
  {
   try {
    const auto validate = [&](std::size_t) { header = request.validator(*image_id, encoded); };
    // The reader may already be suspended. Header work belongs to the retained
    // image input, including its exclusive arena under a small normal budget.
    if (request.execution)
     request.execution->execution().run(detail::CompileTaskKind::Header, {}, validate, input->allowance);
    else
     validate(0);
   } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
    throw_if_benchmark_cancelled(request.cancel_requested);
    if (is_benchmark_capacity_failure(error)) throw;
    trace_benchmark_event(request.trace, "benchmark.images.validation_failed",
     [&] { return nlohmann::json{{"source", request.source}, {"shard", request.shard}, {"member", pathname}, {"image_id", *image_id}, {"bytes", encoded.size()}, {"reason", error.what()}}; });
    if (!request.quarantine_unavailable) { throw; }
    quarantined.push_back(CachedImageRejection{*image_id, error.what()});
    unavailable.emplace(*image_id);
    scheduled.emplace(*image_id);
    input.reset();
    continue;
   }
  }
  // New members cannot invalidate existing subsets; replacements already invalidated during the scan.
  if (std::filesystem::exists(cached_image_path(request.output_root, *image_id))) invalidate_cached_image_proofs(request.output_root);
  try {
   write_pool.submit(*image_id, std::move(input), header, request.execution ? request.execution->image_generation(request.output_root, *image_id) : 0);
   scheduled.emplace(*image_id);
  } catch (const InvalidImageError& error) {
   if (!request.quarantine_unavailable) throw;
   invalidate_cached_image_proofs(request.output_root);
   remove_cache_path(cached_image_path(request.output_root, *image_id));
   quarantined.push_back({*image_id, error.what()});
   unavailable.emplace(*image_id);
   scheduled.emplace(*image_id);
  }
 }
 if (request.activity) { request.activity("Draining selected image cache writes"); }
 write_pool.finish();
 pixels.drain();
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
