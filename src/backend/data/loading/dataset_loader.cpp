#include "src/backend/data/loading/dataset_loader.h"
#include <cuda_runtime_api.h>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/backend/data/compiled/compiled_image_stream.h"
#include "src/common/system/cpu_affinity.h"
#include "src/frameworks/gpu/cuda/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda/cuda_error.h"
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::data {
using mmltk::common::system::clamp_worker_count_to_cpus;
namespace {
constexpr size_t kShuffleChunkImages = 8;
constexpr size_t kShuffleBlockImages = 256;
struct ShuffleLayout {
 size_t chunk_images, chunks_per_block, num_chunks, num_blocks;
};
ShuffleLayout shuffle_layout(size_t images, size_t batch_size) {
 const auto chunk_images = std::min(images, std::max(kShuffleChunkImages, batch_size));
 const auto chunks_per_block = std::max<size_t>(1, kShuffleBlockImages / chunk_images);
 const auto num_chunks = images / chunk_images + (images % chunk_images != 0);
 return {chunk_images, chunks_per_block, num_chunks, num_chunks / chunks_per_block + (num_chunks % chunks_per_block != 0)};
}
void validate_config(const DatasetLoader::Config& config) {
 if (config.compiled_path.empty()) { throw std::invalid_argument("compiled_path must not be empty"); }
 if (config.batch_size == 0) { throw std::invalid_argument("batch_size must be greater than zero"); }
 if (config.prefetch_factor <= 0) { throw std::invalid_argument("prefetch_factor must be greater than zero"); }
 if (config.gather_workers < 0) { throw std::invalid_argument("gather_workers must be non-negative"); }
 if (config.batch_shard_count == 0) { throw std::invalid_argument("batch_shard_count must be greater than zero"); }
 if (config.batch_shard_rank >= config.batch_shard_count) { throw std::invalid_argument("batch_shard_rank must be less than batch_shard_count"); }
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void build_block_shuffled_order(
 std::vector<uint32_t>& order, std::span<const std::uint32_t> eligible, size_t batch_size, std::mt19937_64& rng, std::vector<size_t>& block_order, std::vector<size_t>& chunk_order) {
 if (eligible.empty()) { return; }
 const auto total_images = eligible.size();
 const auto [chunk_images, chunks_per_block, num_chunks, num_blocks] = shuffle_layout(total_images, batch_size);
 mmltk::common::logging::profile_set_value("loader.shuffle.chunk_images", chunk_images);
 mmltk::common::logging::profile_set_value("loader.shuffle.block_images", chunk_images * chunks_per_block);
 mmltk::common::logging::profile_set_value("loader.shuffle.chunks", num_chunks);
 mmltk::common::logging::profile_set_value("loader.shuffle.blocks", num_blocks);
 block_order.resize(num_blocks);
 std::iota(block_order.begin(), block_order.end(), 0);
 std::shuffle(block_order.begin(), block_order.end(), rng);
 chunk_order.resize(chunks_per_block);
 size_t out = 0;
 for (size_t block_id : block_order) {
  const size_t chunk_begin = block_id * chunks_per_block;
  const size_t chunk_end = std::min(chunk_begin + chunks_per_block, num_chunks);
  const size_t chunk_count = chunk_end - chunk_begin;
  chunk_order.resize(chunk_count);
  std::iota(chunk_order.begin(), chunk_order.end(), chunk_begin);
  std::shuffle(chunk_order.begin(), chunk_order.end(), rng);
  for (size_t chunk_id : chunk_order) {
   const size_t image_begin = chunk_id * chunk_images;
   const size_t image_end = std::min(image_begin + chunk_images, total_images);
   for (size_t image = image_begin; image < image_end; ++image) { order[out++] = eligible[image]; }
  }
 }
 if (out != total_images) { throw std::runtime_error("block shuffle failed to populate the full epoch order"); }
}
// NOLINTEND(bugprone-easily-swappable-parameters)
}  // namespace
struct DatasetLoader::Impl {
 enum class State : uint8_t { Free, Reading, Ready, CheckedOut, Released };
 struct Slot {
  State state = State::Free;
  size_t batch = 0, start = 0, count = 0;
  uint64_t lease = 0;
  bool transfer_pending = false, consumer_pending = false;
  const float* host = nullptr;
  std::vector<CompiledImageRead> reads;
 };
 Config config;
 std::shared_ptr<const CompiledDataset> source;
 std::unique_ptr<CompiledImageStream> stream;
 std::vector<uint32_t> order;
 std::vector<uint32_t> eligible;
 std::shared_ptr<const DatasetIndexSchedule> explicit_schedule;
 std::size_t resume_offset = 0;
 const std::vector<std::uint32_t>& indices() const { return explicit_schedule ? explicit_schedule->image_indices : order; }
 std::vector<size_t> shuffled_blocks, shuffled_chunks;
 size_t batch_count = 0;
 std::vector<Slot> slots;
 std::vector<size_t> batch_slots;
 size_t submitted = 0, consumed = 0;
 uint64_t next_lease = 1;
 bool epoch = false, stopping = false, resetting = false;
 std::exception_ptr failure;
 std::mutex mutex;
 std::condition_variable changed;
 void check_failure() const {
  if (failure) std::rethrow_exception(failure);
  if (stopping) throw std::runtime_error("dataset loader stopped");
 }
 bool checked_out() const {
  return std::ranges::any_of(slots, [](const Slot& slot) { return slot.state == State::CheckedOut; });
 }
 Slot& require(const Batch& batch, const char* operation, bool released = false) {
  if (batch.owner != this) throw std::runtime_error(std::string(operation) + ": foreign batch lease");
  if (batch.slot_index >= slots.size()) throw std::runtime_error(std::string(operation) + ": batch slot index out of range");
  auto& slot = slots[batch.slot_index];
  if (slot.lease != batch.lease_id) throw std::runtime_error(std::string(operation) + ": stale batch lease");
  if (slot.state != State::CheckedOut && !(released && slot.state == State::Released)) throw std::runtime_error(std::string(operation) + ": batch is not checked out");
  return slot;
 }
 static void read_complete(void* context, const size_t index, std::exception_ptr error, const bool read) noexcept {
  auto& owner = *static_cast<Impl*>(context);
  {
   std::lock_guard lock(owner.mutex);
   if (error && !owner.failure) owner.failure = error;
   auto& slot = owner.slots[index];
   if (error || !read) {
    slot.state = State::Released;
    slot.transfer_pending = false;
   } else {
    slot.state = State::Ready;
   }
  }
  owner.changed.notify_all();
 }
 void completed(const size_t index, const std::exception_ptr error, const bool consumer) noexcept {
  {
   std::lock_guard lock(mutex);
   if (error && !failure) failure = error;
   (consumer ? slots[index].consumer_pending : slots[index].transfer_pending) = false;
   if (!failure && !stopping && !resetting) {
    try {
     refill(index);
    } catch (...) { failure = std::current_exception(); }
   }
  }
  changed.notify_all();
 }
 static void transfer_complete(void* context, const size_t index, std::exception_ptr error) noexcept { static_cast<Impl*>(context)->completed(index, error, false); }
 static void consumer_complete(void* context, const size_t index, std::exception_ptr error) noexcept { static_cast<Impl*>(context)->completed(index, error, true); }
 void rebuild_schedule() {
  const size_t images = indices().size();
  const size_t batches = config.drop_last ? images / config.batch_size : images / config.batch_size + (images % config.batch_size != 0);
  const size_t first = explicit_schedule ? resume_offset / config.batch_size : config.batch_shard_rank;
  const size_t step = explicit_schedule ? 1 : config.batch_shard_count;
  batch_count = first < batches ? (batches - 1 - first) / step + 1 : 0;
 }
 size_t batch_start(size_t batch) const { return explicit_schedule ? resume_offset + batch * config.batch_size : (config.batch_shard_rank + batch * config.batch_shard_count) * config.batch_size; }
 void refill(const size_t index) {
  check_failure();
  auto& slot = slots[index];
  if (slot.state == State::Released && !slot.transfer_pending && !slot.consumer_pending) slot.state = State::Free;
  if (submitted == batch_count || slot.state != State::Free) return;
  slot.batch = submitted;
  slot.start = batch_start(submitted);
  slot.count = std::min(config.batch_size, indices().size() - slot.start);
  slot.lease = next_lease++;
  slot.reads.clear();
  bool contiguous = true;
  for (size_t image = 0; image < slot.count; ++image) {
   slot.reads.push_back({indices()[slot.start + image], image * source->header().image_stride});
   contiguous &= source->pixel_offset(indices()[slot.start + image]) == source->pixel_offset(indices()[slot.start]) + image * source->header().image_stride;
  }
  slot.host = contiguous ? source->image_pixels(indices()[slot.start]) : nullptr;
  slot.transfer_pending = true;
  slot.state = State::Reading;
  batch_slots[submitted++ % batch_slots.size()] = index;
  try {
   stream->submit(index, *source, slot.reads, {.context = this, .complete = read_complete}, {.context = this, .complete = transfer_complete});
  } catch (...) {
   slot.transfer_pending = false;
   slot.state = State::Released;
   failure = std::current_exception();
   changed.notify_all();
   throw;
  }
 }
 void release(const Batch& batch, void* consumer_stream, const bool has_consumer) {
  std::lock_guard lock(mutex);
  auto& slot = require(batch, "release_batch");
  if (has_consumer && stopping) {
   mmltk::frameworks::gpu::CudaDeviceScope scope(config.device_id);
   mmltk::frameworks::gpu::ensure_cuda_ok(scope.status(), "stopped dataset device binding");
   const auto settled = cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(consumer_stream));
   mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "stopped dataset caller restoration");
   mmltk::frameworks::gpu::ensure_cuda_ok(settled, "stopped dataset consumer completion");
  } else if (has_consumer) {
   stream->release(batch.slot_index, consumer_stream, {.context = this, .complete = consumer_complete});
   slot.consumer_pending = true;
  }
  slot.state = State::Released;
  if (!failure && !stopping && !resetting) {
   try {
    refill(batch.slot_index);
   } catch (...) { failure = std::current_exception(); }
  }
  changed.notify_all();
 }
};
DatasetLoader::DatasetLoader(const Config& config, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement, decltype(&cudaEventRecord) record_consumer)
    : impl_(std::make_unique<Impl>()) {
 validate_config(config);
 if (!record_consumer) throw std::invalid_argument("dataset consumer completion operation is unavailable");
 auto& state = *impl_;
 state.config = config;
 if (config.source && !std::filesystem::equivalent(config.source->path(), config.compiled_path)) throw std::invalid_argument("shared compiled source differs from loader path");
 state.source = config.source ? config.source
                              : std::make_shared<const CompiledDataset>(
                                 CompiledDataset::open(config.compiled_path, config.shuffle ? CompiledDataset::AccessPattern::Normal : CompiledDataset::AccessPattern::Sequential));
 state.eligible.reserve(state.source->header().num_images);
 for (const auto slot : state.source->eligible_slots()) state.eligible.push_back(slot);
 state.order = state.eligible;
 if (config.shuffle) {
  const auto layout = shuffle_layout(state.order.size(), config.batch_size);
  state.shuffled_blocks.reserve(layout.num_blocks);
  state.shuffled_chunks.reserve(layout.chunks_per_block);
 }
 state.rebuild_schedule();
 const auto execution = config.execution
                         ? *config.execution
                         : mmltk::frameworks::gpu::resolve_device_execution(config.device_id, mmltk::common::system::NumaTopology::Capture(), config.loading.numa_node, config.cpu_affinity);
 const auto& cpus = execution.placement.cpus;
 const int requested = config.gather_workers > 0 ? config.gather_workers : config.prefetch_factor;
 const auto useful_slots = std::max<std::size_t>(1U, std::min(static_cast<std::size_t>(config.prefetch_factor), state.batch_count));
 const auto chunks = std::max<std::size_t>(1, CompiledDataset::gather_chunks(config.batch_size, state.source->header().image_stride, state.source->header().max_label_bytes));
 const auto per_slot = config.loading.h2d_dataloader ? std::min<std::size_t>(chunks, requested) : 1U;
 const auto useful_workers = useful_slots > static_cast<std::size_t>(requested) / per_slot ? static_cast<std::size_t>(requested) : useful_slots * per_slot;
 const auto workers = static_cast<size_t>(clamp_worker_count_to_cpus(static_cast<int>(useful_workers), cpus.size(), 1, 1));
 state.slots.resize(useful_slots);
 state.batch_slots.resize(useful_slots);
 // Explicit epochs may repeat eligible slots to fill the configured batch.
 const auto maximum_count = config.batch_size;
 state.stream = std::make_unique<CompiledImageStream>(
  CompiledImageStream::Config{
   .slots = state.slots.size(),
   .workers = workers,
   .device = config.device_id,
   .cpu_affinity = config.cpu_affinity,
   .loading = config.loading,
   .execution = execution,
   .reader_pool = config.reader_pool,
   .record_consumer = record_consumer
  },
  std::move(retirement));
 const auto stride = static_cast<size_t>(state.source->header().image_stride);
 const auto label_pitch = state.source->header().max_label_bytes;
 if (config.batch_size > std::numeric_limits<size_t>::max() / stride || (label_pitch && config.batch_size > std::numeric_limits<size_t>::max() / label_pitch))
  throw std::overflow_error("dataset batch storage size overflow");
 mmltk::frameworks::gpu::CudaDeviceScope scope(config.device_id);
 if (!scope) { mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader device binding"); }
 try {
  mmltk::frameworks::gpu::ensure_cuda_ok(cudaFree(nullptr), "dataset loader context initialization");
  state.stream->bind_current_context();
  for (size_t index = 0; maximum_count != 0U && index < state.slots.size(); ++index) {
   state.stream->prepare_host(index, maximum_count * stride);
   state.stream->prepare_reads(index, maximum_count, label_pitch);
   state.stream->prepare_device(index, maximum_count * stride);
   state.slots[index].reads.reserve(maximum_count);
  }
 } catch (...) {
  const auto error = std::current_exception();
  mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader caller restoration");
  std::rethrow_exception(error);
 }
 mmltk::frameworks::gpu::ensure_cuda_ok(scope.Finalize(), "dataset loader caller restoration");
}
void DatasetLoader::stop_workers() {
 {
  std::lock_guard lock(impl_->mutex);
  impl_->stopping = true;
 }
 impl_->changed.notify_all();
 impl_->stream->stop_workers();
}
DatasetLoader::~DatasetLoader() {
 {
  std::lock_guard lock(impl_->mutex);
  if (impl_->checked_out()) std::terminate();
  impl_->stopping = true;
 }
 impl_->changed.notify_all();
 impl_->stream->cancel_reads();
 // Callbacks refer to Impl, so physically destroy the stream before its
 // mutex, source mapping, order, and batch records leave scope.
 impl_->stream.reset();
}
void DatasetLoader::begin_epoch() { begin_epoch({}, 0); }
void DatasetLoader::begin_epoch(std::shared_ptr<const DatasetIndexSchedule> schedule, std::size_t resume_offset) {
 if (schedule) {
  if (resume_offset > schedule->image_indices.size() || resume_offset % impl_->config.batch_size || (!schedule->draw_keys.empty() && schedule->draw_keys.size() != schedule->image_indices.size()))
   throw std::invalid_argument("invalid explicit dataset schedule or resume offset");
  if (!schedule->microbatch_keys.empty() &&
      schedule->microbatch_keys.size() != (schedule->image_indices.size() / impl_->config.batch_size + (schedule->image_indices.size() % impl_->config.batch_size != 0)))
   throw std::invalid_argument("explicit microbatch key count differs");
  for (auto image : schedule->image_indices)
   if (!impl_->source->eligible(image)) throw std::invalid_argument("scheduled image is outside dataset");
 } else if (resume_offset)
  throw std::invalid_argument("resume offset requires an explicit dataset schedule");
 {
  std::lock_guard lock(impl_->mutex);
  impl_->check_failure();
  if (impl_->stopping) throw std::runtime_error("begin_epoch cannot restart a stopped dataset loader");
  if (impl_->checked_out()) throw std::runtime_error("begin_epoch requires all checked-out batches to be released");
  impl_->resetting = true;
 }
 impl_->stream->cancel_reads();
 impl_->stream->synchronize();
 std::lock_guard lock(impl_->mutex);
 impl_->check_failure();
 impl_->explicit_schedule = std::move(schedule);
 impl_->resume_offset = resume_offset;
 if (!impl_->explicit_schedule && impl_->config.shuffle) {
  std::mt19937_64 rng(impl_->config.seed++);
  build_block_shuffled_order(impl_->order, impl_->eligible, impl_->config.batch_size, rng, impl_->shuffled_blocks, impl_->shuffled_chunks);
 }
 impl_->rebuild_schedule();
 impl_->submitted = impl_->consumed = 0;
 for (auto& slot : impl_->slots) slot.state = Impl::State::Free;
 impl_->epoch = true;
 impl_->resetting = false;
 for (size_t index = 0; index < impl_->slots.size(); ++index) impl_->refill(index);
}
bool DatasetLoader::next_batch(Batch& out) { return next_batch(out, {}); }
Batch DatasetLoader::describe_batch(const DatasetIndexSchedule& schedule, std::size_t microbatch, std::size_t global_batch, std::size_t begin, std::size_t count) const {
 if (!global_batch || begin > global_batch || count > global_batch - begin || microbatch >= schedule.image_indices.size() / global_batch ||
     schedule.draw_keys.size() != schedule.image_indices.size() || microbatch >= schedule.microbatch_keys.size())
  throw std::invalid_argument("scheduled annotation batch shape differs");
 const auto offset = microbatch * global_batch + begin;
 for (const auto image : std::span{schedule.image_indices}.subspan(offset, count))
  if (!impl_->source->eligible(image)) throw std::invalid_argument("scheduled annotation image is outside dataset");
 return {
  .draw_keys = std::span{schedule.draw_keys}.subspan(offset, count),
  .microbatch_key = schedule.microbatch_keys[microbatch],
  .num_images = count,
  .source = impl_->source.get(),
  .image_indices = schedule.image_indices.data() + offset,
 };
}
bool DatasetLoader::next_batch(Batch& out, std::stop_token stop) {
 if (stop.stop_requested()) return false;
 const auto wake = [this] {
  std::lock_guard lock(impl_->mutex);
  impl_->changed.notify_all();
 };
 std::optional<std::stop_callback<decltype(wake)>> cancellation;
 if (stop.stop_possible()) cancellation.emplace(stop, wake);
 {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stopping) return false;
 }
 if (!impl_->epoch) begin_epoch();
 std::unique_lock lock(impl_->mutex);
 for (;;) {
  if (impl_->stopping) return false;
  impl_->check_failure();
  if (stop.stop_requested()) return false;
  if (impl_->consumed == impl_->batch_count) return false;
  if (impl_->consumed == impl_->submitted) {
   impl_->changed.wait(lock);
   continue;
  }
  const auto index = impl_->batch_slots[impl_->consumed % impl_->batch_slots.size()];
  auto& slot = impl_->slots[index];
  if (slot.batch == impl_->consumed && slot.state == Impl::State::Ready) {
   slot.state = Impl::State::CheckedOut;
   ++impl_->consumed;
   out = {
    .draw_keys = impl_->explicit_schedule && !impl_->explicit_schedule->draw_keys.empty() ? std::span<const std::uint64_t>{impl_->explicit_schedule->draw_keys}.subspan(slot.start, slot.count)
                                                                                          : std::span<const std::uint64_t>{},
    .microbatch_key = impl_->explicit_schedule && !impl_->explicit_schedule->microbatch_keys.empty() ? impl_->explicit_schedule->microbatch_keys.at(slot.start / impl_->config.batch_size) : 0,
    .num_images = slot.count,
    .device_images = static_cast<const float*>(impl_->stream->device_storage(index).data()),
    .source = impl_->source.get(),
    .annotation_views = impl_->stream->annotations(index).data(),
    .image_indices = impl_->indices().data() + slot.start,
    .slot_index = index,
    .lease_id = slot.lease,
    .owner = impl_.get(),
    .image_custody = impl_->stream->storage_custody(),
    .image_capacity_bytes = slot.count * impl_->source->header().image_stride
   };
   return true;
  }
  impl_->changed.wait(lock);
 }
}
std::span<const float> DatasetLoader::host_images(const Batch& batch) {
 std::lock_guard lock(impl_->mutex);
 auto& slot = impl_->require(batch, "host_images");
 if (!slot.host) slot.host = reinterpret_cast<const float*>(impl_->stream->host_images(batch.slot_index).data());
 return {slot.host, slot.count * image_stride() / sizeof(float)};
}
void DatasetLoader::wait_batch(const Batch& batch) {
 std::lock_guard lock(impl_->mutex);
 impl_->require(batch, "wait_batch", true);
 impl_->stream->wait_transfer(batch.slot_index);
}
void DatasetLoader::handoff_batch(const Batch& batch, void* stream) {
 std::lock_guard lock(impl_->mutex);
 impl_->require(batch, "handoff_batch");
 impl_->stream->handoff(batch.slot_index, stream);
}
void DatasetLoader::release_batch(const Batch& batch) { impl_->release(batch, nullptr, false); }
void DatasetLoader::release_batch(const Batch& batch, void* stream) { impl_->release(batch, stream, true); }
void DatasetLoader::synchronize() { impl_->stream->synchronize(); }
size_t DatasetLoader::num_images() const { return impl_->source->header().num_images; }
size_t DatasetLoader::num_batches() const { return impl_->batch_count; }
uint32_t DatasetLoader::image_width() const { return impl_->source->header().image_width; }
mmltk::backend::imaging::resample::ImageResizeMode DatasetLoader::resize_mode() const { return impl_->source->header().resize_mode; }
const ImageEntry& DatasetLoader::image_entry(std::uint32_t index) const {
 if (!impl_->source->eligible(index)) throw std::out_of_range("dataset image geometry index");
 return impl_->source->image_entry(index);
}
mmltk::backend::imaging::resample::ImageResizeGeometry DatasetLoader::geometry(std::uint32_t index) const {
 if (!impl_->source->eligible(index)) throw std::out_of_range("dataset resize geometry index");
 return impl_->source->geometry(index);
}
uint32_t DatasetLoader::image_height() const { return impl_->source->header().image_height; }
uint32_t DatasetLoader::num_classes() const { return impl_->source->header().num_classes; }
uint32_t DatasetLoader::max_instances_per_image() const { return impl_->source->header().max_instances_per_image; }
const std::shared_ptr<const catalog::ClassCatalog>& DatasetLoader::class_catalog() const noexcept { return impl_->source->class_catalog(); }
const std::shared_ptr<const CompiledDataset>& DatasetLoader::compiled_source() const noexcept { return impl_->source; }
const char* DatasetLoader::class_name(uint32_t id) const {
 if (id >= num_classes()) throw std::out_of_range("class id out of range");
 return impl_->source->class_names()[id].c_str();
}
size_t DatasetLoader::image_stride() const { return impl_->source->header().image_stride; }
size_t DatasetLoader::num_label_instances() const { return impl_->source->label_count(); }
size_t DatasetLoader::num_rle_pairs() const { return impl_->source->run_count(); }
bool DatasetLoader::masks_available() const noexcept { return impl_->source->masks_available(); }
ImageAnnotations DatasetLoader::annotations(std::uint32_t slot) const { return impl_->source->annotations(slot); }
const float* DatasetLoader::image_pixels(std::uint32_t slot) const { return impl_->source->image_pixels(slot); }
}  // namespace mmltk::backend::data
