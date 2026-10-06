#include "src/backend/data/compiled/compiled_image_stream.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/detail/file_work_queue.h"
#include <cuda_runtime_api.h>
#include <cuda.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include "src/common/io/noexcept_io.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/cuda/cuda_error.h"
#include "src/frameworks/gpu/image/image_failure.h"
#include "src/frameworks/gpu/cuda/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/cuda/cuda_priority.h"
#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include "src/frameworks/gpu/memory/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::data {
namespace gpu = mmltk::frameworks::gpu;
namespace {
template <class Operation>
void on_context(const CUcontext context, Operation&& operation) {
 if (context == nullptr) throw std::logic_error("compiled image stream has no owning CUDA context");
 gpu::ensure_cuda_driver_ok(cuCtxPushCurrent(context), "compiled image stream context binding");
 CUcontext popped = nullptr;
 try {
  operation();
 } catch (...) {
  const auto failure = std::current_exception();
  gpu::ensure_cuda_driver_ok(cuCtxPopCurrent(&popped), "compiled image stream context restoration");
  std::rethrow_exception(failure);
 }
 gpu::ensure_cuda_driver_ok(cuCtxPopCurrent(&popped), "compiled image stream context restoration");
}
template <class Emit>
void observe_read(Emit&& emit) noexcept {
 // Optional observations never become a read or physical-retirement failure.
 try {
  mmltk::common::logging::debug(std::forward<Emit>(emit));
 } catch (...) {}
}
}  // namespace
struct CompiledImageStream::Buffer::Impl {
 explicit Impl(const bool use_host, const mmltk::common::system::ExecutionPlacement* selected) : pinned(use_host) {
  if (selected) placement = *selected;
  if (use_host && !selected) throw std::invalid_argument("host stream storage requires resolved placement");
 }
 mmltk::common::system::ExecutionPlacement placement;
 std::unique_ptr<gpu::PinnedHostBuffer> host;
 bool mapped = false;
 std::unique_ptr<gpu::GdrMappedBuffer> gdr;
 gpu::GdrMappedBuffer::ReadLease lease;
 gpu::CudaHighWaterAllocation<void*> allocation;
 std::size_t capacity = 0;
 bool pinned;
 cudaError_t free(void* value) const noexcept { return cudaFree(value); }
};
CompiledImageStream::Buffer::Buffer(const bool pinned, const mmltk::common::system::ExecutionPlacement* placement, const bool mapped) : impl_(std::make_unique<Impl>(pinned, placement)) {
 impl_->mapped = mapped;
}
// Physical release is explicit after stream settlement. A failed system keeps
// the owning stream in its runtime custody; its buffers are never freed blindly.
CompiledImageStream::Buffer::~Buffer() = default;
void CompiledImageStream::Buffer::ensure_bytes(const std::size_t bytes) {
 auto& state = *impl_;
 if (state.mapped) {
  if (!state.gdr) {
   CUcontext context{};
   gpu::ensure_cuda_driver_ok(cuCtxGetCurrent(&context), "mapped image owner context");
   state.gdr = std::make_unique<gpu::GdrMappedBuffer>(context, 2U);
  }
  state.lease = {};
  state.gdr->ensure_bytes(bytes);
  state.capacity = state.gdr->capacity_bytes();
  return;
 }
 if (state.pinned) {
  if (!state.host) {
   CUcontext context{};
   gpu::ensure_cuda_driver_ok(cuCtxGetCurrent(&context), "local pinned host owner context");
   state.host = std::make_unique<gpu::PinnedHostBuffer>(context, state.placement, true);
  }
  state.host->ensure_bytes(bytes);
  state.capacity = state.host->capacity_bytes();
  return;
 }
 gpu::ensure_cuda_ok(state.allocation.RetryPending([&](void* value) { return state.free(value); }).failure, "compiled image staging pending release");
 if (bytes <= state.capacity) return;
 gpu::ensure_cuda_ok(state.allocation.AllocateCandidate([&](void*& value) { return cudaMalloc(&value, bytes); }).failure, "compiled image staging allocation");
 gpu::ensure_cuda_ok(state.allocation.PromoteCandidate([&](void* value) { return state.free(value); }).failure, "compiled image staging replacement");
 state.capacity = bytes;
}
void* CompiledImageStream::Buffer::data() const noexcept { return impl_->gdr ? reinterpret_cast<void*>(impl_->lease.device_data()) : impl_->host ? impl_->host->data() : impl_->allocation.active(); }
std::size_t CompiledImageStream::Buffer::capacity_bytes() const noexcept { return impl_->capacity; }
bool CompiledImageStream::Buffer::owns_allocation() const noexcept { return (impl_->gdr && impl_->capacity != 0) || (impl_->host && impl_->host->data()) || !impl_->allocation.empty(); }
int CompiledImageStream::Buffer::reset() noexcept {
 if (impl_->gdr) {
  try {
   impl_->lease = {};
   impl_->gdr->close();
   impl_->gdr.reset();
   impl_->capacity = 0;
  } catch (...) { return cudaErrorUnknown; }
  return cudaSuccess;
 }
 if (impl_->host) {
  const auto error = impl_->host->ReleaseSettled();
  if (error == CUDA_SUCCESS) impl_->capacity = 0;
  return error == CUDA_SUCCESS ? cudaSuccess : cudaErrorUnknown;
 }
 const auto result = impl_->allocation.ReleaseAll([&](void* value) { return impl_->free(value); });
 if (result.released()) impl_->capacity = 0;
 return result.failure;
}
void CompiledImageStream::Buffer::begin_write() { impl_->lease = {}; }
void CompiledImageStream::Buffer::write(std::size_t offset, std::span<const std::byte> bytes) {
 if (!impl_->gdr->write(offset, bytes)) throw std::runtime_error("mapped image write cancelled");
}
void CompiledImageStream::Buffer::publish(void* stream) {
 impl_->lease = impl_->gdr->borrow();
 impl_->lease.record_consumed(reinterpret_cast<CUstream>(stream));
}
void CompiledImageStream::Buffer::consumed(void* stream) {
 if (impl_->gdr) impl_->lease.record_consumed(reinterpret_cast<CUstream>(stream));
}
struct CompiledImageStream::Impl {
 using BorrowedTask = mmltk::common::concurrency::WorkerPool::BorrowedTask;
 enum class ReadStage { Idle, Preparing, PrefaultWaiting, PrefaultRunning, PrefaultReturning, Gathering, Finishing };
 struct GatherTask {
  GatherTask(CompiledImageStream* owner, std::size_t index) : dispatch(owner, index, [](void* context, std::size_t task) { static_cast<CompiledImageStream*>(context)->gather_chunk(task); }) {}
  BorrowedTask dispatch;
  CompiledDataset::UsedRangeCursor cursor;
  std::size_t slot = 0, bytes = 0;
  bool active = false;
 };
 struct Slot {
  explicit Slot(const mmltk::common::system::ExecutionPlacement& placement, bool demanded_host, CompiledImageStream* stream_owner, std::size_t slot_index)
      : host(true, &placement), metadata(true, &placement), labels(true, &placement), owner(stream_owner), index(slot_index), dispatch(stream_owner, slot_index, [](void* context, std::size_t slot) {
         static_cast<CompiledImageStream*>(context)->read_slot(slot);
        }) {
   if (demanded_host) host_policy.emplace(mmltk::common::system::ExecutionPolicyRequest{placement.cpus, {}, 0, placement.numa_node, -10, true});
  }
  Buffer host;
  Buffer metadata;
  Buffer labels;
  std::optional<mmltk::common::system::PreparedExecutionPolicy> host_policy;
  std::vector<ImageAnnotations> annotations;
  std::size_t label_pitch = 0;
  std::vector<CompiledImageRead> reads;
  CompiledImageStream* owner;
  std::size_t index;
  BorrowedTask dispatch;
  ReadStage stage = ReadStage::Idle;
  bool dispatched = false;
  std::size_t gathers = 0;
  CompiledDataset::UsedRangeCursor cursor, preparation;
  const CompiledDataset* source = nullptr;
  ReadObserver observer;
  CompletionObserver transfer_observer;
  std::atomic<bool> cancelled{false};
  bool reading = false;
  std::size_t read_callbacks = 0U;
  std::size_t callbacks = 0U;
  std::size_t transfer_callbacks = 0U;
  bool read = false;
  bool host_materialized = false;
  std::exception_ptr read_failure;
  cudaEvent_t transfer = nullptr;
  cudaEvent_t consumer = nullptr;
  bool transfer_pending = false;
  bool consumer_pending = false;
  bool unfenced = false;
  cudaStream_t unfenced_stream = nullptr;
 };
 struct Completion {
  std::size_t slot = 0;
  CompletionObserver observer;
 };
 struct CompletionQueue {
  std::vector<Completion> jobs;
  std::size_t head = 0, queued = 0, active = 0;
  bool started = false;
  std::thread worker;
  [[nodiscard]] bool idle() const noexcept { return queued == 0U && active == 0U; }
 };
 explicit Impl(Config selected, CompiledImageStream* owner)
     : config(std::move(selected)),
       execution(config.execution ? *config.execution : gpu::resolve_device_execution(config.device, mmltk::common::system::NumaTopology::Capture(), config.loading.numa_node, config.cpu_affinity)),
       cpus(execution.placement.cpus),
       private_pool(!config.reader_pool),
       pool(config.reader_pool ? config.reader_pool : std::make_shared<mmltk::common::concurrency::WorkerPool>(config.workers, cpus, "compiled-read", config.slots, &execution.placement, true)),
       slots(config.slots),
       devices(config.slots) {
  if (execution.device != config.device || (config.loading.numa_node >= 0 && config.loading.numa_node != execution.placement.numa_node))
   throw std::invalid_argument("compiled stream placement contradicts selected GPU/NUMA node");
  for (std::size_t worker = 0; worker < pool->size(); ++worker) {
   const auto& policy = pool->policy(worker);
   if (policy.numa_node != execution.placement.numa_node || policy.affinity.empty() || !std::ranges::all_of(policy.affinity, [&](const int cpu) { return std::ranges::find(cpus, cpu) != cpus.end(); }))
    throw std::invalid_argument("compiled reader pool contradicts selected CPU/NUMA placement");
   mmltk::common::logging::debug([&](auto& log) {
    log.debug(
     "event=execution.placement owner=compiled-read device={} pci={} node={} eligible={} worker={} "
     "cpu={} nice={} scheduler={} io_class={} io_priority={}",
     execution.device, execution.pci_identity, execution.placement.numa_node, mmltk::common::system::format_cpu_list(cpus), worker, policy.affinity.front(), policy.nice_value, policy.scheduler_policy,
     policy.io_class, policy.io_priority_data);
   });
  }
  for (std::size_t index = 0; index < slots.size(); ++index) slots[index] = std::make_unique<Slot>(execution.placement, !config.loading.h2d_dataloader, owner, index);
  const auto task_count = std::min(config.workers, pool->size());
  tasks.reserve(task_count);
  for (std::size_t task = 0; task < task_count; ++task) tasks.push_back(std::make_unique<GatherTask>(owner, task));
  if (config.storage_growth.grown) config.storage_growth.grown(config.storage_growth.context, StorageKind::ReadTasks, 0, 0, task_count * sizeof(GatherTask));
  if (config.prefault)
   files = std::make_unique<detail::FileWorkQueue>(*pool, std::min(task_count, slots.size()),
    detail::FileWorkQueue::RuntimeReadiness{owner, [](void* stream, void* operation) noexcept { static_cast<CompiledImageStream*>(stream)->prefault_returned(static_cast<Slot*>(operation)->index); }});
  for (auto& device : devices) device = std::make_unique<Buffer>(false, nullptr, !config.loading.h2d_dataloader);
  for (auto& queue : completions) queue.jobs.resize(config.slots);
 }
 Config config;
 template <class Storage, class Operation>
 void prepare_storage(StorageKind kind, std::size_t index, Storage& storage, Operation&& operation) {
  const auto observer = config.storage_growth;
  if (!observer.grown) {
   operation();
   return;
  }
  const auto capacity = [&] {
   if constexpr (requires { storage.capacity_bytes(); })
    return storage.capacity_bytes();
   else
    return storage.capacity() * sizeof(typename Storage::value_type);
  };
  const auto before = capacity();
  operation();
  const auto after = capacity();
  if (after > before) observer.grown(observer.context, kind, index, before, after);
 }
 gpu::DeviceExecution execution;
 std::vector<int> cpus;
 bool private_pool;
 std::shared_ptr<mmltk::common::concurrency::WorkerPool> pool;
 std::vector<std::unique_ptr<Slot>> slots;
 std::vector<std::unique_ptr<GatherTask>> tasks;
 std::unique_ptr<detail::FileWorkQueue> files;
 std::size_t next_slot = 0;
 std::size_t read_workers = 0;
 std::vector<std::unique_ptr<Buffer>> devices;
 CUcontext context = nullptr;
 cudaStream_t copy = nullptr;
 bool initialized = false;
 std::mutex mutex;
 std::mutex submission;
 std::condition_variable changed;
 // Upload completion never waits behind a consumer on an unrelated stream.
 // Each queue retains at most one pending event per physical slot.
 std::array<CompletionQueue, 2> completions;
 bool stopping = false;
 std::exception_ptr failure;
 bool completion_failed = false;
 void enqueue(const std::size_t index, const bool consumer, cudaStream_t stream, CompletionObserver observer) {
  std::lock_guard lock(mutex);
  if (failure) std::rethrow_exception(failure);
  auto& slot = *slots.at(index);
  auto& pending = consumer ? slot.consumer_pending : slot.transfer_pending;
  auto& queue = completions[consumer];
  if (stopping || pending || queue.queued == queue.jobs.size()) throw std::logic_error("compiled image completion capacity unavailable");
  const auto record = consumer ? config.record_consumer : &cudaEventRecord;
  gpu::ensure_cuda_ok(record(consumer ? slot.consumer : slot.transfer, stream), "compiled image completion event");
  queue.jobs[(queue.head + queue.queued) % queue.jobs.size()] = {index, observer};
  pending = true;
  ++slot.callbacks;
  if (!consumer) ++slot.transfer_callbacks;
  if (slot.unfenced && slot.unfenced_stream == stream) slot.unfenced = false;
  ++queue.queued;
  changed.notify_all();
 }
 void settle_unfenced(Slot& slot) {
  cudaStream_t stream = nullptr;
  {
   std::lock_guard lock(mutex);
   if (!slot.unfenced) return;
   stream = slot.unfenced_stream;
  }
  gpu::ensure_cuda_ok(cudaStreamSynchronize(stream), "compiled image partial upload settlement");
  std::lock_guard lock(mutex);
  slot.unfenced = false;
 }
 void settle_unfenced() {
  for (auto& slot : slots) settle_unfenced(*slot);
 }
 void complete_loop(const bool consumer) noexcept {
  auto& queue = completions[consumer];
  try {
   (void)mmltk::common::system::apply_worker_execution_policy({cpus, consumer ? "compiled-retire" : "compiled-upload", config.workers + consumer, execution.placement.numa_node, -10, false});
   gpu::ensure_cuda_driver_ok(cuCtxSetCurrent(context), "compiled image completion worker context");
   {
    std::lock_guard lock(mutex);
    queue.started = true;
   }
   changed.notify_all();
   for (;;) {
    Completion job;
    {
     std::unique_lock lock(mutex);
     changed.wait(lock, [&] { return stopping || queue.queued != 0; });
     if (queue.queued == 0) return;
     job = queue.jobs[queue.head];
     queue.head = (queue.head + 1) % queue.jobs.size();
     --queue.queued;
     ++queue.active;
     // Retain this slot's observer order even though unrelated uploads can
     // now finish while this worker waits for a consumer's physical event.
     if (consumer) changed.wait(lock, [&] { return slots[job.slot]->transfer_callbacks == 0U; });
    }
    std::exception_ptr error;
    try {
     const auto& slot = *slots[job.slot];
     gpu::ensure_cuda_ok(cudaEventSynchronize(consumer ? slot.consumer : slot.transfer), "compiled image GPU completion");
    } catch (...) { error = std::current_exception(); }
    observe_read([&](auto& log) { log.debug("event=compiled.stream.retirement stream={} slot={} consumer={} failed={}", reinterpret_cast<std::uintptr_t>(this), job.slot, consumer, bool(error)); });
    {
     std::lock_guard lock(mutex);
     if (error) {
      completion_failed = true;
      if (!failure) failure = error;
     }
     auto& slot = *slots[job.slot];
     (consumer ? slot.consumer_pending : slot.transfer_pending) = false;
    }
    // This is an ordinary system-owned worker, never a CUDA host
    // callback. Context errors still deliver a terminal notification.
    if (job.observer.complete) job.observer.complete(job.observer.context, job.slot, error);
    {
     std::lock_guard lock(mutex);
     --slots[job.slot]->callbacks;
     if (!consumer) --slots[job.slot]->transfer_callbacks;
     --queue.active;
    }
    changed.notify_all();
   }
  } catch (...) {
   std::unique_lock lock(mutex);
   if (!failure) failure = std::current_exception();
   completion_failed |= !queue.idle();
   while (queue.queued != 0) {
    const auto job = queue.jobs[queue.head];
    queue.head = (queue.head + 1) % queue.jobs.size();
    --queue.queued;
    auto& slot = *slots[job.slot];
    (consumer ? slot.consumer_pending : slot.transfer_pending) = false;
    const auto error = failure;
    lock.unlock();
    if (job.observer.complete) job.observer.complete(job.observer.context, job.slot, error);
    lock.lock();
    --slot.callbacks;
    if (!consumer) --slot.transfer_callbacks;
   }
   changed.notify_all();
  }
 }
};
struct CompiledImageStream::Retention {
 explicit Retention(std::shared_ptr<gpu::TerminalCudaRetirementOwner> authority)
     : terminal(authority ? std::move(authority) : std::make_shared<gpu::TerminalCudaRetirementOwner>(1U)), lease(gpu::ReserveTerminalCudaLease(*terminal)) {}
 ~Retention() noexcept {
  if (state) std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(state)), cudaErrorUnknown);
 }
 std::shared_ptr<gpu::TerminalCudaRetirementOwner> terminal;
 gpu::TerminalCudaRetirementLease lease;
 std::shared_ptr<Impl> state;
};
CompiledImageStream::CompiledImageStream(Config config, std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement) {
 if (!config.settle || !config.record_consumer || config.slots == 0 || config.slots > std::numeric_limits<std::size_t>::max() / 2 || config.workers == 0 ||
     config.workers > static_cast<std::size_t>(std::numeric_limits<int>::max()))
  throw std::invalid_argument("invalid compiled image stream capacity");
 retention_ = std::make_unique<Retention>(std::move(retirement));
 impl_ = std::make_shared<Impl>(std::move(config), this);
}
const gpu::DeviceExecution& CompiledImageStream::execution() const noexcept { return impl_->execution; }
void CompiledImageStream::bind_current_context() {
 CUcontext current = nullptr;
 gpu::ensure_cuda_driver_ok(cuCtxGetCurrent(&current), "compiled image owner context");
 if (current == nullptr) throw std::logic_error("compiled image stream initialization requires the owning context to be current");
 if (impl_->initialized) {
  if (current != impl_->context) throw std::logic_error("compiled image stream cannot change owning CUDA context");
  return;
 }
 if (impl_->failure) std::rethrow_exception(impl_->failure);
 CUdevice device = -1;
 gpu::ensure_cuda_driver_ok(cuCtxGetDevice(&device), "compiled image owner device");
 if (device != impl_->config.device) throw std::logic_error("compiled image stream owning device mismatch");
 impl_->context = current;
 try {
  for (auto& slot : impl_->slots) {
   for (auto* event : {&slot->transfer, &slot->consumer})
    gpu::ensure_cuda_ok(cudaEventCreateWithFlags(event, cudaEventDisableTiming | cudaEventBlockingSync), "compiled image stream event allocation");
  }
  if (impl_->config.loading.h2d_dataloader) gpu::ensure_cuda_ok(gpu::cuda_stream_create_with_highest_priority(&impl_->copy, cudaStreamNonBlocking), "compiled image copy stream");
  for (const bool consumer : {false, true}) {
   if (consumer || impl_->config.loading.h2d_dataloader)
    impl_->completions[consumer].worker = std::thread([this, consumer] { impl_->complete_loop(consumer); });
   else
    impl_->completions[consumer].started = true;
  }
  {
   std::unique_lock lock(impl_->mutex);
   impl_->changed.wait(lock, [&] { return std::ranges::all_of(impl_->completions, &Impl::CompletionQueue::started) || impl_->failure; });
   if (impl_->failure) std::rethrow_exception(impl_->failure);
  }
  impl_->initialized = true;
 } catch (...) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->failure) impl_->failure = std::current_exception();
  throw;
 }
}
CompiledImageStream::~CompiledImageStream() {
 try {
  close();
 } catch (...) {
  const auto failure = std::current_exception();
  retention_->state = std::move(impl_);
  try {
   if (mmltk::common::logging::enabled(spdlog::level::critical)) {
    mmltk::common::logging::critical([&](spdlog::logger& log) {
     try {
      std::rethrow_exception(failure);
     } catch (const std::exception& error) { log.critical("compiled image stream retained unsafe CUDA resources on device {}: {}", retention_->state->config.device, error.what()); } catch (...) {
      log.critical("compiled image stream retained unsafe CUDA resources after an unknown failure");
     }
    });
    return;
   }
  } catch (...) {}
  mmltk::common::io::write_all_noexcept(STDERR_FILENO, "fatal: compiled image stream retained unsafe CUDA resources\n");
 }
}
void CompiledImageStream::stop_workers() {
 {
  std::lock_guard lock(impl_->mutex);
  impl_->stopping = true;
 }
 cancel_reads();
 if (impl_->files) impl_->files->cancel();
 if (impl_->pool) {
  // Shared pools retain unrelated work. A private pool also settles external
  // catalog tasks before its final owner releases the workers.
  std::exception_ptr failure;
  try {
   wait_reads();
  } catch (...) { failure = std::current_exception(); }
  if (impl_->files) {
   impl_->files->join();
   impl_->files.reset();
  }
  impl_->pool.reset();
  impl_->config.reader_pool.reset();
  if (failure) {
   std::lock_guard lock(impl_->mutex);
   if (!impl_->failure) impl_->failure = failure;
  }
 }
 impl_->changed.notify_all();
 for (auto& queue : impl_->completions)
  if (queue.worker.joinable()) queue.worker.join();
}
void CompiledImageStream::close() {
 stop_workers();
 if (impl_->context == nullptr) return;
 on_context(impl_->context, [&] {
  if (impl_->completion_failed) std::rethrow_exception(impl_->failure);
  impl_->settle_unfenced();
  if (impl_->copy) gpu::ensure_cuda_ok(impl_->config.settle(impl_->copy), "compiled image stream destruction settlement");
  gpu::ensure_cuda_ok(static_cast<cudaError_t>(reset_storage()), "compiled image storage destruction");
  for (auto& slot : impl_->slots) {
   for (auto* event : {&slot->transfer, &slot->consumer}) {
    if (*event) {
     gpu::ensure_cuda_ok(cudaEventDestroy(*event), "compiled image event destruction");
     *event = nullptr;
    }
   }
  }
  if (impl_->copy) {
   gpu::ensure_cuda_ok(cudaStreamDestroy(impl_->copy), "compiled image stream destruction");
   impl_->copy = nullptr;
  }
 });
 impl_->context = nullptr;
 impl_->initialized = false;
}
void CompiledImageStream::prepare_host(const std::size_t index, const std::size_t bytes) {
 std::lock_guard lock(impl_->mutex);
 auto& slot = *impl_->slots.at(index);
 if (impl_->failure) std::rethrow_exception(impl_->failure);
 if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced) throw std::logic_error("compiled image host storage is still borrowed");
 on_context(impl_->context, [&] { impl_->prepare_storage(StorageKind::HostImages, index, slot.host, [&] { slot.host.ensure_bytes(bytes); }); });
}
void CompiledImageStream::prepare_reads(std::size_t index, std::size_t count, std::optional<std::size_t> max_label_bytes) {
 const std::lock_guard lock(impl_->mutex);
 auto& slot = *impl_->slots.at(index);
 if (impl_->failure) std::rethrow_exception(impl_->failure);
 if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced) throw std::logic_error("compiled read storage is still borrowed");
 if (max_label_bytes && (*max_label_bytes > COMPILED_LABEL_LIMIT || *max_label_bytes % sizeof(RLEPair))) throw std::invalid_argument("invalid prepared maximum label size");
 if (max_label_bytes || !slot.annotations.empty()) {
  count = std::max(count, slot.annotations.size());
  const auto pitch = std::max(slot.label_pitch, max_label_bytes.value_or(0));
  const auto bytes = mmltk::common::math::checked_multiply(count, pitch, "compiled batch labels overflow");
  if (bytes) on_context(impl_->context, [&] { impl_->prepare_storage(StorageKind::Labels, index, slot.labels, [&] { slot.labels.ensure_bytes(bytes); }); });
  impl_->prepare_storage(StorageKind::AnnotationViews, index, slot.annotations, [&] { slot.annotations.resize(count); });
  slot.label_pitch = pitch;
 }
 impl_->prepare_storage(StorageKind::ReadDescriptors, index, slot.reads, [&] { slot.reads.reserve(count); });
}
const CompiledImageStream::Buffer& CompiledImageStream::label_storage(std::size_t index) const { return impl_->slots.at(index)->labels; }
std::span<const ImageAnnotations> CompiledImageStream::annotations(std::size_t index) const {
 const auto& slot = *impl_->slots.at(index);
 return {slot.annotations.data(), slot.annotations.empty() ? 0U : slot.reads.size()};
}
void CompiledImageStream::prepare_images(std::size_t index, std::size_t bytes) {
 prepare_host(index, bytes);
 prepare_device(index, bytes);
}
void CompiledImageStream::prepare_metadata(std::size_t index, std::size_t bytes) {
 auto& slot = *impl_->slots.at(index);
 std::lock_guard lock(impl_->mutex);
 if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced) throw std::logic_error("compiled image metadata is still borrowed");
 on_context(impl_->context, [&] { impl_->prepare_storage(StorageKind::Metadata, index, slot.metadata, [&] { slot.metadata.ensure_bytes(bytes); }); });
}
const CompiledImageStream::Buffer& CompiledImageStream::metadata_storage(std::size_t index) { return impl_->slots.at(index)->metadata; }
void CompiledImageStream::prepare_device(const std::size_t index, const std::size_t bytes) {
 auto& buffer = *impl_->devices.at(index);
 {
  std::lock_guard lock(impl_->mutex);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
 }
 if (bytes <= buffer.capacity_bytes()) return;
 {
  std::lock_guard lock(impl_->mutex);
  const auto& slot = *impl_->slots.at(index);
  if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced) throw std::logic_error("compiled image device storage is still borrowed");
 }
 on_context(impl_->context, [&] {
  // Slots have independent storage. Growing this idle slot does not
  // settle unrelated reads or GPU consumers.
  try {
   impl_->prepare_storage(StorageKind::DeviceImages, index, buffer, [&] { buffer.ensure_bytes(bytes); });
  } catch (const std::exception& error) {
   if (gpu::find_image_failure<gpu::GdrTransportUnavailable>(std::current_exception())) throw;
   throw std::runtime_error("compiled image loading on CUDA device " + std::to_string(impl_->config.device) +
                            (impl_->config.loading.h2d_dataloader ? " using H2D: " : " requires GDRCopy (omit --gdrcopy for H2D): ") + error.what());
  }
 });
}
std::span<const std::byte> CompiledImageStream::host_images(const std::size_t index) {
 if (!wait_read(index)) throw std::runtime_error("CPU image view requested for a cancelled read");
 auto& slot = *impl_->slots.at(index);
 std::size_t bytes = 0;
 for (const auto& read : slot.reads) bytes = std::max(bytes, read.destination_offset + slot.source->header().image_stride);
 bool materialize = false;
 {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->config.loading.h2d_dataloader && !slot.host_materialized) {
   if (bytes > slot.host.capacity_bytes()) throw std::logic_error("CPU image storage was not prepared");
   if (slot.reading) throw std::logic_error("CPU image materialization already active");
   slot.reading = true;
   ++slot.read_callbacks;
   materialize = true;
  }
 }
 if (materialize) {
  std::exception_ptr failure;
  bool complete = false;
  try {
   // The caller can hold its loader lease lock. Never wait behind a reader
   // callback that needs that lock, or depend on a pool detached during stop.
   mmltk::common::system::ScopedExecutionPolicy policy(*slot.host_policy);
   // Explicit prefault already completed before this slot was published.
   // CPU demand remains valid after reader stop and uses no service queue.
   complete = slot.source->read_images(slot.reads, {static_cast<std::byte*>(slot.host.data()), bytes}, slot.cancelled);
   policy.Restore();
  } catch (...) { failure = std::current_exception(); }
  {
   const std::lock_guard lock(impl_->mutex);
   slot.host_materialized = complete && !failure;
   slot.read_failure = failure;
   slot.reading = false;
   --slot.read_callbacks;
   impl_->changed.notify_all();
  }
  if (failure) std::rethrow_exception(failure);
  if (!complete) throw std::runtime_error("CPU image view read cancelled");
 }
 return {static_cast<const std::byte*>(slot.host.data()), bytes};
}
const CompiledImageStream::Buffer& CompiledImageStream::host_storage(const std::size_t slot) const { return impl_->slots.at(slot)->host; }
const CompiledImageStream::Buffer& CompiledImageStream::device_storage(const std::size_t index) const { return *impl_->devices.at(index); }
mmltk::common::concurrency::WorkerPool& CompiledImageStream::workers() noexcept { return *impl_->pool; }
void CompiledImageStream::submit(
 const std::size_t index, const CompiledDataset& source, const std::span<const CompiledImageRead> reads, const ReadObserver observer, const CompletionObserver transfer) {
 auto& slot = *impl_->slots.at(index);
 const auto stride = static_cast<std::size_t>(source.header().image_stride);
 const auto capacity = impl_->config.loading.h2d_dataloader ? slot.host.capacity_bytes() : device_storage(index).capacity_bytes();
 if (stride == 0 || reads.size() > capacity / stride) throw std::out_of_range("compiled image read list exceeds slot capacity");
 {
  if (reads.size() * stride > device_storage(index).capacity_bytes()) throw std::out_of_range("compiled image batch exceeds device storage");
  for (std::size_t image = 0; image < reads.size(); ++image)
   if (reads[image].destination_offset != image * stride) throw std::invalid_argument("compiled image batch upload requires packed image destinations");
 }
 {
  std::lock_guard lock(impl_->mutex);
  if (impl_->failure) std::rethrow_exception(impl_->failure);
  if (!impl_->initialized) throw std::logic_error("compiled image stream must bind its context before submitting reads");
  if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced || impl_->stopping) throw std::logic_error("compiled image slot is still in use");
  if (reads.size() > slot.reads.capacity() || (!slot.annotations.empty() && reads.size() > slot.annotations.size())) throw std::out_of_range("compiled read descriptors were not prepared");
  if (!slot.annotations.empty() && source.header().max_label_bytes > slot.label_pitch) throw std::out_of_range("compiled labels exceed prepared maximum");
  slot.reads.assign(reads.begin(), reads.end());
  slot.cancelled.store(false, std::memory_order_release);
  slot.reading = true;
  slot.stage = Impl::ReadStage::Preparing;
  slot.dispatched = true;
  ++slot.read_callbacks;
  slot.read = false;
  slot.host_materialized = false;
  slot.read_failure = {};
  slot.source = &source;
  slot.observer = observer;
  slot.transfer_observer = transfer;
  ++impl_->read_workers;
 }
 try {
  impl_->pool->enqueue_borrowed(slot.dispatch);
 } catch (...) {
  std::lock_guard lock(impl_->mutex);
  slot.reading = false;
  slot.stage = Impl::ReadStage::Idle;
  slot.dispatched = false;
  --slot.read_callbacks;
  --impl_->read_workers;
  impl_->changed.notify_all();
  throw;
 }
}
void CompiledImageStream::read_slot(const std::size_t index) noexcept {
 auto& current = *impl_->slots[index];
 const auto& source = *current.source;
 observe_read([&](auto& log) {
  log.debug("event=compiled.stream.read stream={} slot={} source={} images={} h2d={} prefault={}", reinterpret_cast<std::uintptr_t>(impl_.get()), index, source.path().string(), current.reads.size(),
   impl_->config.loading.h2d_dataloader, impl_->config.prefault);
 });
 try {
  if (!current.cancelled.load(std::memory_order_acquire) && (!current.observer.before || current.observer.before(current.observer.context, index))) {
   // All physical order and range metadata are ready before touching labels.
   if (!std::ranges::is_sorted(current.reads, {}, &CompiledImageRead::index)) std::ranges::sort(current.reads, {}, &CompiledImageRead::index);
   for (const auto& read : current.reads) {
    const auto& entry = source.image_entry(read.index);
    if (!current.annotations.empty()) {
     const auto position = read.destination_offset / source.header().image_stride;
     auto* destination = static_cast<std::byte*>(current.labels.data());
     if (entry.label_bytes) destination += position * current.label_pitch;
     current.annotations[position] = annotation_view(entry, {destination, entry.label_bytes});
    }
   }
   const auto labels = current.annotations.empty() ? std::optional<std::size_t>{} : current.label_pitch;
   current.cursor = source.used_ranges(current.reads, labels);
   current.preparation = source.used_ranges(current.reads, labels, true);
   auto advice = current.preparation;
   {
    mmltk::common::logging::ScopedProfile profile{"compiled.stream.advice"};
    while (!current.cancelled.load(std::memory_order_acquire)) {
     const auto range = advice.next();
     if (!range) break;
     operate_range(index, ReadOperation::Advice, *range);
    }
   }
   if (!impl_->config.loading.h2d_dataloader && !current.cancelled.load(std::memory_order_acquire)) on_context(impl_->context, [&] { impl_->devices[index]->begin_write(); });
  } else
   current.cancelled.store(true, std::memory_order_release);
 } catch (...) { current.read_failure = std::current_exception(); }
 {
  const std::lock_guard lock(impl_->mutex);
  current.dispatched = false;
  current.stage = current.read_failure || current.cancelled.load(std::memory_order_acquire) ? Impl::ReadStage::Finishing : impl_->files ? Impl::ReadStage::PrefaultWaiting : Impl::ReadStage::Gathering;
 }
 dispatch_reads();
 read_worker_returned();
}
void CompiledImageStream::prefault_returned(std::size_t index) noexcept {
 {
  const std::lock_guard lock(impl_->mutex);
  auto& slot = *impl_->slots[index];
  slot.stage = slot.read_failure || slot.cancelled.load(std::memory_order_acquire) ? Impl::ReadStage::Finishing
               : slot.preparation.empty()                                          ? Impl::ReadStage::Gathering
                                                                                   : Impl::ReadStage::PrefaultWaiting;
 }
 // Actual capacity is free now. Both this continuation and any previously
 // full request stay armed under the same mutex as their admission attempt.
 dispatch_reads();
 read_worker_returned();
}
void CompiledImageStream::dispatch_reads() noexcept {
 for (;;) {
  std::optional<std::size_t> finish;
  {
   const std::lock_guard lock(impl_->mutex);
   for (std::size_t turn = 0; turn < impl_->slots.size(); ++turn) {
    const auto index = (impl_->next_slot + turn) % impl_->slots.size();
    auto& slot = *impl_->slots[index];
    if (slot.dispatched || slot.stage == Impl::ReadStage::Idle || slot.stage == Impl::ReadStage::Preparing || slot.stage == Impl::ReadStage::PrefaultRunning ||
        slot.stage == Impl::ReadStage::PrefaultReturning)
     continue;
    try {
     if (slot.read_failure || slot.cancelled.load(std::memory_order_acquire)) {
      if (slot.gathers) continue;
      slot.stage = Impl::ReadStage::Finishing;
     }
     if (slot.stage == Impl::ReadStage::PrefaultWaiting) {
      auto accepted = impl_->files->try_submit({.context = &slot, .call = [](void* context) {
       auto& current = *static_cast<Impl::Slot*>(context);
       mmltk::common::logging::ScopedProfile profile{"compiled.stream.prefault"};
       std::size_t remaining = CompiledDataset::read_chunk_bytes;
       while (remaining && !current.cancelled.load(std::memory_order_acquire)) {
        const auto range = current.preparation.next(remaining);
        if (!range) break;
        current.owner->operate_range(current.index, ReadOperation::Prefault, *range);
        remaining -= range->bytes;
       }
      }, .completed = [current = &slot](std::exception_ptr error) {
       const std::lock_guard completion(current->owner->impl_->mutex);
       if (error && !current->cancelled.load(std::memory_order_acquire)) current->read_failure = error;
       current->stage = Impl::ReadStage::PrefaultReturning;
      }});
      if (accepted) {
       slot.stage = Impl::ReadStage::PrefaultRunning;
       ++impl_->read_workers;
      }
      // Full leaves the same cursor/request waiting for capacity return.
      continue;
     }
     if (slot.stage == Impl::ReadStage::Gathering) {
      if (!impl_->config.loading.h2d_dataloader && slot.gathers) continue;
      for (auto& task : impl_->tasks) {
       if (slot.cursor.empty()) break;
       if (task->active) continue;
       task->cursor = slot.cursor;
       task->bytes = 0;
       while (task->bytes < CompiledDataset::read_chunk_bytes) {
        const auto range = slot.cursor.next(CompiledDataset::read_chunk_bytes - task->bytes);
        if (!range) break;
        task->bytes += range->bytes;
       }
       task->slot = index;
       impl_->pool->enqueue_borrowed(task->dispatch);
       task->active = true;
       ++slot.gathers;
       ++impl_->read_workers;
       if (!impl_->config.loading.h2d_dataloader) break;
      }
      if (slot.gathers || !slot.cursor.empty()) continue;
      slot.stage = Impl::ReadStage::Finishing;
     }
    } catch (...) {
     if (!slot.read_failure) slot.read_failure = std::current_exception();
     if (slot.gathers) continue;
     slot.stage = Impl::ReadStage::Finishing;
    }
    if (slot.stage == Impl::ReadStage::Finishing && !finish) {
     slot.dispatched = true;
     finish = index;
    }
   }
   if (finish) impl_->next_slot = (*finish + 1) % impl_->slots.size();
  }
  if (!finish) return;
  finish_read(*finish);
 }
}
void CompiledImageStream::gather_chunk(const std::size_t index) noexcept {
 auto& task = *impl_->tasks[index];
 auto& slot = *impl_->slots[task.slot];
 std::exception_ptr failure;
 try {
  mmltk::common::logging::ScopedProfile profile{"compiled.stream.gather"};
  const auto gather = [&] {
   auto remaining = task.bytes;
   while (remaining && !slot.cancelled.load(std::memory_order_acquire)) {
    const auto range = task.cursor.next(remaining);
    if (!range) throw std::logic_error("compiled gather cursor ended before its prepared chunk");
    operate_range(task.slot, ReadOperation::Gather, *range);
    remaining -= range->bytes;
   }
  };
  if (impl_->config.loading.h2d_dataloader)
   gather();
  else
   on_context(impl_->context, gather);
 } catch (...) { failure = std::current_exception(); }
 {
  const std::lock_guard lock(impl_->mutex);
  if (failure && !slot.read_failure) slot.read_failure = failure;
  --slot.gathers;
  task.active = false;
  // Yield to a peer slot before assigning this same prepared record again.
  impl_->next_slot = (task.slot + 1) % impl_->slots.size();
 }
 dispatch_reads();
 read_worker_returned();
}
void CompiledImageStream::operate_range(std::size_t index, ReadOperation operation, const CompiledDataset::UsedRange& range) {
 auto& slot = *impl_->slots[index];
 if (slot.observer.range) slot.observer.range(slot.observer.context, index, operation, range);
 if (operation == ReadOperation::Gather) {
  auto& destination = range.section == CompiledDataset::ReadSection::Labels ? slot.labels : impl_->config.loading.h2d_dataloader ? slot.host : *impl_->devices[index];
  const bool mapped = range.section == CompiledDataset::ReadSection::Pixels && !impl_->config.loading.h2d_dataloader;
  slot.source->gather_range(range, {&destination, destination.capacity_bytes(), mapped ? +[](void* context, std::size_t offset, std::span<const std::byte> bytes) {
   static_cast<Buffer*>(context)->write(offset, bytes);
  } : +[](void* context, std::size_t offset, std::span<const std::byte> bytes) { std::memcpy(static_cast<std::byte*>(static_cast<Buffer*>(context)->data()) + offset, bytes.data(), bytes.size()); }});
  mmltk::common::logging::profile_add_value(range.section == CompiledDataset::ReadSection::Labels ? "compiled.stream.labels.bytes" : "compiled.stream.pixels.bytes", range.bytes);
 } else
  slot.source->prepare_range(range, operation == ReadOperation::Prefault);
 observe_read([&](auto& log) {
  log.debug("event=compiled.stream.range stream={} slot={} operation={} section={} source={} destination={} bytes={}", reinterpret_cast<std::uintptr_t>(impl_.get()), index,
   operation == ReadOperation::Advice     ? "advice"
   : operation == ReadOperation::Prefault ? "prefault"
                                          : "gather",
   range.section == CompiledDataset::ReadSection::Labels ? "labels" : "pixels", range.source_offset, range.destination_offset, range.bytes);
 });
}
void CompiledImageStream::read_worker_returned() noexcept {
 const std::lock_guard lock(impl_->mutex);
 --impl_->read_workers;
 // The final borrowed invocation makes no owner access after this boundary.
 impl_->changed.notify_all();
}
void CompiledImageStream::finish_read(const std::size_t index) noexcept {
 auto& current = *impl_->slots[index];
 const auto observer = current.observer;
 const auto transfer = current.transfer_observer;
 auto failure = current.read_failure;
 bool read = !failure && !current.cancelled.load(std::memory_order_acquire);
 bool transfer_submitted = false;
 try {
  if (read)
   on_context(impl_->context, [&] {
    if (impl_->config.loading.h2d_dataloader) {
     const std::lock_guard submission(impl_->submission);
     upload(index, current.reads.size() * current.source->header().image_stride, impl_->copy);
     impl_->enqueue(index, false, impl_->copy, transfer);
     transfer_submitted = true;
    } else
     impl_->devices[index]->publish(nullptr);
   });
 } catch (...) { failure = std::current_exception(); }
 observe_read([&](auto& log) {
  log.debug("event=compiled.stream.read_complete stream={} slot={} read={} failed={} uploaded={}", reinterpret_cast<std::uintptr_t>(impl_.get()), index, read, bool(failure), transfer_submitted);
 });
 {
  const std::lock_guard lock(impl_->mutex);
  current.read = read;
  current.read_failure = failure;
  current.reading = false;
  current.stage = Impl::ReadStage::Idle;
  current.dispatched = false;
 }
 if (!transfer_submitted && transfer.complete) transfer.complete(transfer.context, index, failure);
 if (observer.complete) observer.complete(observer.context, index, failure, read);
 {
  const std::lock_guard lock(impl_->mutex);
  --current.read_callbacks;
  impl_->changed.notify_all();
 }
}
void CompiledImageStream::cancel_reads() noexcept {
 {
  const std::lock_guard lock(impl_->mutex);
  for (auto& slot : impl_->slots)
   if (slot->reading) slot->cancelled.store(true, std::memory_order_release);
  impl_->changed.notify_all();
 }
 dispatch_reads();
}
void CompiledImageStream::cancel_read(const std::size_t index) noexcept {
 {
  const std::lock_guard lock(impl_->mutex);
  auto& slot = *impl_->slots[index];
  if (slot.reading) slot.cancelled.store(true, std::memory_order_release);
  impl_->changed.notify_all();
 }
 dispatch_reads();
}
void CompiledImageStream::wait_reads() {
 if (impl_->pool && impl_->private_pool) { impl_->pool->wait_idle(); }
 std::unique_lock lock(impl_->mutex);
 impl_->changed.wait(lock, [&] { return impl_->read_workers == 0 && std::ranges::all_of(impl_->slots, [](const auto& slot) { return !slot->reading && slot->read_callbacks == 0; }); });
}
bool CompiledImageStream::wait_read(const std::size_t index) {
 std::unique_lock lock(impl_->mutex);
 const auto& slot = *impl_->slots.at(index);
 impl_->changed.wait(lock, [&] { return !slot.reading; });
 if (slot.read_failure) std::rethrow_exception(slot.read_failure);
 return slot.read;
}
void CompiledImageStream::upload(const std::size_t slot, const std::size_t bytes, void* stream) {
 const auto& device = device_storage(slot);
 const auto& host = host_storage(slot);
 if (bytes > device.capacity_bytes() || bytes > host.capacity_bytes()) throw std::out_of_range("compiled image upload exceeds slot storage");
 {
  std::lock_guard lock(impl_->mutex);
  auto& physical = *impl_->slots.at(slot);
  if (physical.unfenced && physical.unfenced_stream != reinterpret_cast<cudaStream_t>(stream)) throw std::logic_error("compiled image uploads require one transfer boundary");
  physical.unfenced = true;
  physical.unfenced_stream = reinterpret_cast<cudaStream_t>(stream);
 }
 mmltk::common::logging::profile_add_value("compiled.stream.h2d.bytes", bytes);
 mmltk::common::logging::profile_add_value("compiled.stream.h2d.submissions", 1U);
 gpu::ensure_cuda_ok(cudaMemcpyAsync(device.data(), host.data(), bytes, cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)), "compiled image upload");
 observe_read([&](auto& log) { log.debug("event=compiled.stream.upload stream={} slot={} bytes={}", reinterpret_cast<std::uintptr_t>(impl_.get()), slot, bytes); });
}
void CompiledImageStream::wait_transfer(const std::size_t slot) {
 if (!impl_->config.loading.h2d_dataloader) {
  (void)wait_read(slot);
  return;
 }
 on_context(impl_->context, [&] { gpu::ensure_cuda_ok(cudaEventSynchronize(impl_->slots.at(slot)->transfer), "compiled image transfer wait"); });
}
void CompiledImageStream::handoff(const std::size_t slot, void* stream) {
 if (!impl_->config.loading.h2d_dataloader) {
  (void)wait_read(slot);
  return;
 }
 on_context(impl_->context, [&] { gpu::ensure_cuda_ok(cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(stream), impl_->slots.at(slot)->transfer, 0), "compiled image consumer handoff"); });
}
void CompiledImageStream::release(const std::size_t slot, void* stream, CompletionObserver observer) {
 on_context(impl_->context, [&] {
  impl_->devices.at(slot)->consumed(stream);
  impl_->enqueue(slot, true, reinterpret_cast<cudaStream_t>(stream), observer);
 });
}
void CompiledImageStream::fence(const std::size_t slot, void* stream, CompletionObserver observer) {
 on_context(impl_->context, [&] { impl_->enqueue(slot, true, reinterpret_cast<cudaStream_t>(stream), observer); });
}
void CompiledImageStream::wait_consumers() {
 std::unique_lock lock(impl_->mutex);
 impl_->changed.wait(lock, [&] { return impl_->completions[1].idle(); });
 if (impl_->failure) std::rethrow_exception(impl_->failure);
}
void CompiledImageStream::synchronize(const std::size_t index) {
 auto& slot = *impl_->slots.at(index);
 {
  std::unique_lock lock(impl_->mutex);
  impl_->changed.wait(lock, [&] { return !slot.reading && slot.read_callbacks == 0U && slot.callbacks == 0U; });
  if (impl_->failure) std::rethrow_exception(impl_->failure);
 }
 if (impl_->context) on_context(impl_->context, [&] { impl_->settle_unfenced(slot); });
}
void CompiledImageStream::synchronize() {
 wait_reads();
 if (impl_->context) on_context(impl_->context, [&] { impl_->settle_unfenced(); });
 std::unique_lock lock(impl_->mutex);
 impl_->changed.wait(lock, [&] { return std::ranges::all_of(impl_->completions, &Impl::CompletionQueue::idle); });
 if (impl_->failure) std::rethrow_exception(impl_->failure);
}
std::weak_ptr<const void> CompiledImageStream::storage_custody() const noexcept { return impl_; }
int CompiledImageStream::reset_storage() noexcept {
 if (impl_.use_count() != 1) return cudaErrorNotReady;
 {
  std::lock_guard lock(impl_->mutex);
  if (impl_->completion_failed) return cudaErrorUnknown;
  if (!std::ranges::all_of(impl_->completions, &Impl::CompletionQueue::idle) ||
      std::ranges::any_of(impl_->slots, [](const auto& slot) { return slot->reading || slot->transfer_pending || slot->consumer_pending || slot->unfenced; }))
   return cudaErrorNotReady;
 }
 for (auto& slot : impl_->slots) {
  const auto labels_status = slot->labels.reset();
  if (labels_status != cudaSuccess) return labels_status;
  const auto metadata_status = slot->metadata.reset();
  if (metadata_status != cudaSuccess) return metadata_status;
  const auto status = slot->host.reset();
  if (status != cudaSuccess) return status;
 }
 for (auto& device : impl_->devices) {
  const auto status = device->reset();
  if (status != cudaSuccess) return status;
 }
 return cudaSuccess;
}
bool CompiledImageStream::owns_allocation() const noexcept {
 return std::ranges::any_of(impl_->slots, [](const auto& slot) { return slot->host.owns_allocation() || slot->metadata.owns_allocation() || slot->labels.owns_allocation(); }) ||
        std::ranges::any_of(impl_->devices, [](const auto& device) { return device->owns_allocation(); });
}
bool CompiledImageStream::owns_resources() const noexcept {
 return owns_allocation() || impl_->copy != nullptr || std::ranges::any_of(impl_->slots, [](const auto& slot) { return slot->transfer != nullptr || slot->consumer != nullptr; });
}
}  // namespace mmltk::backend::data
