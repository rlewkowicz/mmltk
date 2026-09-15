#include "src/backend/data/compiled_image_stream.h"

#include <cuda_runtime_api.h>
#include <cuda.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
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
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/cuda_priority.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

namespace mmltk::backend::data {
namespace gpu = mmltk::frameworks::gpu;
namespace {
void driver_check(const CUresult result, const char* operation) {
    if (result != CUDA_SUCCESS) {
        const char* message = nullptr;
        (void)cuGetErrorString(result, &message);
        throw std::runtime_error(std::string(operation) + ": " + (message ? message : "CUDA driver failure"));
    }
}
template <class Operation>
void on_context(const CUcontext context, Operation&& operation) {
    if (context == nullptr) throw std::logic_error("compiled image stream has no owning CUDA context");
    driver_check(cuCtxPushCurrent(context), "compiled image stream context binding");
    CUcontext popped = nullptr;
    try {
        operation();
    } catch (...) {
        const auto failure = std::current_exception();
        driver_check(cuCtxPopCurrent(&popped), "compiled image stream context restoration");
        std::rethrow_exception(failure);
    }
    driver_check(cuCtxPopCurrent(&popped), "compiled image stream context restoration");
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
CompiledImageStream::Buffer::Buffer(const bool pinned, const mmltk::common::system::ExecutionPlacement* placement, const bool mapped)
    : impl_(std::make_unique<Impl>(pinned, placement)) {
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
            driver_check(cuCtxGetCurrent(&context), "mapped image owner context");
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
            driver_check(cuCtxGetCurrent(&context), "local pinned host owner context");
            state.host = std::make_unique<gpu::PinnedHostBuffer>(context, state.placement, true);
        }
        state.host->ensure_bytes(bytes);
        state.capacity = state.host->capacity_bytes();
        return;
    }
    gpu::ensure_cuda_ok(state.allocation.RetryPending([&](void* value) { return state.free(value); }).failure,
                        "compiled image staging pending release");
    if (bytes <= state.capacity) return;
    gpu::ensure_cuda_ok(state.allocation.AllocateCandidate([&](void*& value) { return cudaMalloc(&value, bytes); }).failure,
                        "compiled image staging allocation");
    gpu::ensure_cuda_ok(state.allocation.PromoteCandidate([&](void* value) { return state.free(value); }).failure,
                        "compiled image staging replacement");
    state.capacity = bytes;
}
void* CompiledImageStream::Buffer::data() const noexcept {
    return impl_->gdr    ? reinterpret_cast<void*>(impl_->lease.device_data())
           : impl_->host ? impl_->host->data()
                         : impl_->allocation.active();
}
std::size_t CompiledImageStream::Buffer::capacity_bytes() const noexcept { return impl_->capacity; }
bool CompiledImageStream::Buffer::owns_allocation() const noexcept {
    return (impl_->gdr && impl_->capacity != 0) || (impl_->host && impl_->host->data()) || !impl_->allocation.empty();
}
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
    struct Slot {
        explicit Slot(const mmltk::common::system::ExecutionPlacement& placement) : host(true, &placement), metadata(true, &placement) {}
        Buffer host;
        Buffer metadata;
        std::vector<CompiledImageRead> reads;
        const CompiledDataset* source = nullptr;
        ReadObserver observer;
        CompletionObserver transfer_observer;
        std::atomic<bool> cancelled{false};
        bool reading = false;
        std::size_t read_callbacks = 0U;
        std::size_t callbacks = 0U;
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
        bool consumer = false;
        CompletionObserver observer;
    };
    explicit Impl(Config selected)
        : config(std::move(selected)),
          execution(config.execution ? *config.execution
                                     : gpu::resolve_device_execution(config.device, mmltk::common::system::NumaTopology::Capture(),
                                                                     config.loading.numa_node, config.cpu_affinity)),
          cpus(execution.placement.cpus),
          pool(std::make_unique<mmltk::common::concurrency::WorkerPool>(config.workers, cpus, "compiled-read", config.slots,
                                                                        &execution.placement, true)),
          slots(config.slots),
          devices(config.slots),
          completions(config.slots * 2) {
        if (execution.device != config.device ||
            (config.loading.numa_node >= 0 && config.loading.numa_node != execution.placement.numa_node))
            throw std::invalid_argument("compiled stream placement contradicts selected GPU/NUMA node");
        for (std::size_t worker = 0; worker < pool->size(); ++worker)
            mmltk::common::logging::debug([&](auto& log) {
                const auto& policy = pool->policy(worker);
                log.debug(
                    "event=execution.placement owner=compiled-read device={} pci={} node={} eligible={} worker={} "
                    "cpu={} nice={} scheduler={} io_class={} io_priority={}",
                    execution.device, execution.pci_identity, execution.placement.numa_node, mmltk::common::system::format_cpu_list(cpus),
                    worker, policy.affinity.front(), policy.nice_value, policy.scheduler_policy, policy.io_class, policy.io_priority_data);
            });
        for (auto& slot : slots)
            slot = std::make_unique<Slot>(execution.placement);
        for (auto& device : devices)
            device = std::make_unique<Buffer>(false, nullptr, !config.loading.h2d_dataloader);
    }
    Config config;
    gpu::DeviceExecution execution;
    std::vector<int> cpus;
    std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool;
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::unique_ptr<Buffer>> devices;
    CUcontext context = nullptr;
    cudaStream_t copy = nullptr;
    bool initialized = false;
    std::mutex mutex;
    std::mutex submission;
    std::condition_variable changed;
    std::vector<Completion> completions;
    std::size_t head = 0, queued = 0, active = 0;
    std::size_t consumers = 0U;
    bool stopping = false;
    std::exception_ptr failure;
    bool completion_failed = false;
    bool completion_started = false;
    std::thread completion_worker;

    void enqueue(const std::size_t index, const bool consumer, cudaStream_t stream, CompletionObserver observer) {
        std::lock_guard lock(mutex);
        if (failure) std::rethrow_exception(failure);
        auto& slot = *slots.at(index);
        auto& pending = consumer ? slot.consumer_pending : slot.transfer_pending;
        if (stopping || pending || queued == completions.size()) throw std::logic_error("compiled image completion capacity unavailable");
        gpu::ensure_cuda_ok(cudaEventRecord(consumer ? slot.consumer : slot.transfer, stream), "compiled image completion event");
        completions[(head + queued) % completions.size()] = {index, consumer, observer};
        pending = true;
        ++slot.callbacks;
        if (consumer) ++consumers;
        if (slot.unfenced && slot.unfenced_stream == stream) slot.unfenced = false;
        ++queued;
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
        for (auto& slot : slots)
            settle_unfenced(*slot);
    }
    void complete_loop() noexcept {
        try {
            (void)mmltk::common::system::apply_worker_execution_policy(
                {cpus, "compiled-done", config.workers, execution.placement.numa_node, -10, false});
            driver_check(cuCtxSetCurrent(context), "compiled image completion worker context");
            {
                std::lock_guard lock(mutex);
                completion_started = true;
            }
            changed.notify_all();
            for (;;) {
                Completion job;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || queued != 0; });
                    if (queued == 0) return;
                    job = completions[head];
                    head = (head + 1) % completions.size();
                    --queued;
                    ++active;
                }
                std::exception_ptr error;
                try {
                    const auto& slot = *slots[job.slot];
                    gpu::ensure_cuda_ok(cudaEventSynchronize(job.consumer ? slot.consumer : slot.transfer),
                                        "compiled image GPU completion");
                } catch (...) { error = std::current_exception(); }
                {
                    std::lock_guard lock(mutex);
                    if (error) {
                        completion_failed = true;
                        if (!failure) failure = error;
                    }
                    auto& slot = *slots[job.slot];
                    (job.consumer ? slot.consumer_pending : slot.transfer_pending) = false;
                }
                // This is an ordinary system-owned worker, never a CUDA host
                // callback. Context errors still deliver a terminal notification.
                if (job.observer.complete) job.observer.complete(job.observer.context, job.slot, error);
                {
                    std::lock_guard lock(mutex);
                    --slots[job.slot]->callbacks;
                    if (job.consumer) --consumers;
                    --active;
                }
                changed.notify_all();
            }
        } catch (...) {
            std::unique_lock lock(mutex);
            failure = std::current_exception();
            completion_failed = queued != 0 || active != 0;
            while (queued != 0) {
                const auto job = completions[head];
                head = (head + 1) % completions.size();
                --queued;
                auto& slot = *slots[job.slot];
                (job.consumer ? slot.consumer_pending : slot.transfer_pending) = false;
                const auto error = failure;
                lock.unlock();
                if (job.observer.complete) job.observer.complete(job.observer.context, job.slot, error);
                lock.lock();
                --slot.callbacks;
                if (job.consumer) --consumers;
            }
            changed.notify_all();
        }
    }
};

struct CompiledImageStream::Retention {
    Retention() : terminal(1U), lease(gpu::ReserveTerminalCudaLease(terminal)) {}
    ~Retention() noexcept {
        if (state) std::move(lease).Install(gpu::TerminalCudaCustody::Share(std::move(state)), cudaErrorUnknown);
    }
    gpu::TerminalCudaRetirementOwner terminal;
    gpu::TerminalCudaRetirementLease lease;
    std::shared_ptr<Impl> state;
};

CompiledImageStream::CompiledImageStream(Config config) {
    if (config.slots == 0 || config.slots > std::numeric_limits<std::size_t>::max() / 2 || config.workers == 0 ||
        config.workers > config.slots || config.workers > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("invalid compiled image stream capacity");
    retention_ = std::make_unique<Retention>();
    impl_ = std::make_shared<Impl>(std::move(config));
}
const gpu::DeviceExecution& CompiledImageStream::execution() const noexcept { return impl_->execution; }
void CompiledImageStream::bind_current_context() {
    CUcontext current = nullptr;
    driver_check(cuCtxGetCurrent(&current), "compiled image owner context");
    if (current == nullptr) throw std::logic_error("compiled image stream initialization requires the owning context to be current");
    if (impl_->initialized) {
        if (current != impl_->context) throw std::logic_error("compiled image stream cannot change owning CUDA context");
        return;
    }
    if (impl_->failure) std::rethrow_exception(impl_->failure);
    CUdevice device = -1;
    driver_check(cuCtxGetDevice(&device), "compiled image owner device");
    if (device != impl_->config.device) throw std::logic_error("compiled image stream owning device mismatch");
    impl_->context = current;
    try {
        for (auto& slot : impl_->slots) {
            for (auto* event : {&slot->transfer, &slot->consumer})
                gpu::ensure_cuda_ok(cudaEventCreateWithFlags(event, cudaEventDisableTiming | cudaEventBlockingSync),
                                    "compiled image stream event allocation");
        }
        if (impl_->config.loading.h2d_dataloader)
            gpu::ensure_cuda_ok(gpu::cuda_stream_create_with_highest_priority(&impl_->copy, cudaStreamNonBlocking),
                                "compiled image copy stream");
        impl_->completion_worker = std::thread([this] { impl_->complete_loop(); });
        {
            std::unique_lock lock(impl_->mutex);
            impl_->changed.wait(lock, [&] { return impl_->completion_started || impl_->failure; });
            if (impl_->failure) std::rethrow_exception(impl_->failure);
        }
        impl_->initialized = true;
    } catch (...) {
        impl_->failure = std::current_exception();
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
                    } catch (const std::exception& error) {
                        log.critical("compiled image stream retained unsafe CUDA resources on device {}: {}",
                                     retention_->state->config.device, error.what());
                    } catch (...) { log.critical("compiled image stream retained unsafe CUDA resources after an unknown failure"); }
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
    if (impl_->pool) {
        // All stream read tasks catch their failures. Detached failures in
        // unrelated catalog tasks still join through WorkerPool destruction.
        std::exception_ptr failure;
        try {
            impl_->pool->wait_idle();
        } catch (...) { failure = std::current_exception(); }
        impl_->pool.reset();
        if (failure) {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->failure) impl_->failure = failure;
        }
    }
    impl_->changed.notify_all();
    if (impl_->completion_worker.joinable()) impl_->completion_worker.join();
}
void CompiledImageStream::close() {
    stop_workers();
    if (impl_->context == nullptr) return;
    on_context(impl_->context, [&] {
        if (impl_->completion_failed) std::rethrow_exception(impl_->failure);
        impl_->settle_unfenced();
        if (impl_->copy) gpu::ensure_cuda_ok(cudaStreamSynchronize(impl_->copy), "compiled image stream destruction settlement");
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
    if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced)
        throw std::logic_error("compiled image host storage is still borrowed");
    on_context(impl_->context, [&] { slot.host.ensure_bytes(bytes); });
}
void CompiledImageStream::prepare_images(std::size_t index, std::size_t bytes) {
    if (impl_->config.loading.h2d_dataloader) prepare_host(index, bytes);
    prepare_device(index, bytes);
}
void CompiledImageStream::prepare_metadata(std::size_t index, std::size_t bytes) {
    auto& slot = *impl_->slots.at(index);
    std::lock_guard lock(impl_->mutex);
    if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced)
        throw std::logic_error("compiled image metadata is still borrowed");
    on_context(impl_->context, [&] { slot.metadata.ensure_bytes(bytes); });
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
        if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced)
            throw std::logic_error("compiled image device storage is still borrowed");
    }
    on_context(impl_->context, [&] {
        // Slots have independent storage. Growing this idle slot does not
        // settle unrelated reads or GPU consumers.
        try {
            buffer.ensure_bytes(bytes);
        } catch (const std::exception& error) {
            if (gpu::find_image_failure<gpu::GdrTransportUnavailable>(std::current_exception())) throw;
            throw std::runtime_error(
                "compiled image loading on CUDA device " + std::to_string(impl_->config.device) +
                (impl_->config.loading.h2d_dataloader ? " using H2D: " : " requires GDRCopy (omit --gdrcopy for H2D): ") + error.what());
        }
    });
}
std::span<const std::byte> CompiledImageStream::host_images(const std::size_t index) {
    if (!wait_read(index)) throw std::runtime_error("CPU image view requested for a cancelled read");
    auto& slot = *impl_->slots.at(index);
    std::size_t bytes = 0;
    for (const auto& read : slot.reads)
        bytes = std::max(bytes, read.destination_offset + slot.source->header().image_stride);
    if (!impl_->config.loading.h2d_dataloader && !slot.host_materialized) {
        on_context(impl_->context, [&] { slot.host.ensure_bytes(bytes); });
        mmltk::common::system::ScopedExecutionPolicy policy({impl_->cpus, {}, 0, impl_->execution.placement.numa_node, -10, true});
        if (!slot.source->read_images(slot.reads, {static_cast<std::byte*>(slot.host.data()), bytes}, slot.cancelled,
                                      impl_->config.prefault))
            throw std::runtime_error("CPU image view read cancelled");
        slot.host_materialized = true;
    }
    return {static_cast<const std::byte*>(slot.host.data()), bytes};
}
const CompiledImageStream::Buffer& CompiledImageStream::host_storage(const std::size_t slot) const { return impl_->slots.at(slot)->host; }
const CompiledImageStream::Buffer& CompiledImageStream::device_storage(const std::size_t index) const { return *impl_->devices.at(index); }
mmltk::common::concurrency::WorkerPool& CompiledImageStream::workers() noexcept { return *impl_->pool; }
void CompiledImageStream::submit(const std::size_t index, const CompiledDataset& source, const std::span<const CompiledImageRead> reads,
                                 const ReadObserver observer, const CompletionObserver transfer) {
    auto& slot = *impl_->slots.at(index);
    const auto stride = static_cast<std::size_t>(source.header().image_stride);
    const auto capacity = impl_->config.loading.h2d_dataloader ? slot.host.capacity_bytes() : device_storage(index).capacity_bytes();
    if (stride == 0 || reads.size() > capacity / stride) throw std::out_of_range("compiled image read list exceeds slot capacity");
    {
        if (reads.size() * stride > device_storage(index).capacity_bytes())
            throw std::out_of_range("compiled image batch exceeds device storage");
        for (std::size_t image = 0; image < reads.size(); ++image)
            if (reads[image].destination_offset != image * stride)
                throw std::invalid_argument("compiled image batch upload requires packed image destinations");
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->failure) std::rethrow_exception(impl_->failure);
        if (!impl_->initialized) throw std::logic_error("compiled image stream must bind its context before submitting reads");
        if (slot.reading || slot.transfer_pending || slot.consumer_pending || slot.unfenced || impl_->stopping)
            throw std::logic_error("compiled image slot is still in use");
        slot.reads.assign(reads.begin(), reads.end());
        slot.cancelled.store(false, std::memory_order_release);
        slot.reading = true;
        ++slot.read_callbacks;
        slot.read = false;
        slot.host_materialized = false;
        slot.read_failure = {};
        slot.source = &source;
        slot.observer = observer;
        slot.transfer_observer = transfer;
    }
    try {
        impl_->pool->enqueue_borrowed(
            this, index, [](void* owner, std::size_t queued_slot) { static_cast<CompiledImageStream*>(owner)->read_slot(queued_slot); });
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        slot.reading = false;
        --slot.read_callbacks;
        impl_->changed.notify_all();
        throw;
    }
}
void CompiledImageStream::read_slot(const std::size_t index) noexcept {
    auto& current = *impl_->slots[index];
    const auto observer = current.observer;
    const auto transfer = current.transfer_observer;
    const auto& source = *current.source;
    std::exception_ptr failure;
    bool read = false;
    bool transfer_submitted = false;
    mmltk::common::logging::ScopedProfile profile{"compiled.stream.read"};
    try {
        if (!current.cancelled.load(std::memory_order_acquire) && (!observer.before || observer.before(observer.context, index))) {
            if (impl_->config.loading.h2d_dataloader) {
                read = source.read_images(current.reads, {static_cast<std::byte*>(current.host.data()), current.host.capacity_bytes()},
                                          current.cancelled, impl_->config.prefault);
            } else {
                auto& destination = *impl_->devices.at(index);
                on_context(impl_->context, [&] {
                    destination.begin_write();
                    read = source.read_images_to(
                        current.reads,
                        CompiledDataset::ImageDestination{&destination, destination.capacity_bytes(),
                                                          [](void* buffer, std::size_t offset, std::span<const std::byte> bytes) {
                                                              static_cast<Buffer*>(buffer)->write(offset, bytes);
                                                          }},
                        current.cancelled, impl_->config.prefault);
                    if (read) destination.publish(nullptr);
                });
            }
        }
        if (read && impl_->config.loading.h2d_dataloader) {
            on_context(impl_->context, [&] {
                std::lock_guard submission(impl_->submission);
                const auto bytes = current.reads.size() * source.header().image_stride;
                upload(index, bytes, impl_->copy);
                impl_->enqueue(index, false, impl_->copy, transfer);
                transfer_submitted = true;
            });
        }
    } catch (...) { failure = std::current_exception(); }
    // The observer publishes host availability only after the gather
    // and optional upload submission have completed.
    {
        std::lock_guard lock(impl_->mutex);
        current.read = read;
        if (failure && !current.read_failure) current.read_failure = failure;
        current.reading = false;
    }
    if (!transfer_submitted && transfer.complete) transfer.complete(transfer.context, index, failure);
    if (observer.complete) observer.complete(observer.context, index, failure, read);
    {
        std::lock_guard lock(impl_->mutex);
        --current.read_callbacks;
    }
    impl_->changed.notify_all();
}
void CompiledImageStream::cancel_reads() noexcept {
    for (auto& slot : impl_->slots)
        slot->cancelled.store(true, std::memory_order_release);
    impl_->changed.notify_all();
}
void CompiledImageStream::cancel_read(const std::size_t index) noexcept {
    impl_->slots[index]->cancelled.store(true, std::memory_order_release);
    impl_->changed.notify_all();
}
void CompiledImageStream::wait_reads() {
    if (impl_->pool) impl_->pool->wait_idle();
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
    if (bytes > device.capacity_bytes() || bytes > host.capacity_bytes())
        throw std::out_of_range("compiled image upload exceeds slot storage");
    {
        std::lock_guard lock(impl_->mutex);
        auto& physical = *impl_->slots.at(slot);
        if (physical.unfenced && physical.unfenced_stream != reinterpret_cast<cudaStream_t>(stream))
            throw std::logic_error("compiled image uploads require one transfer boundary");
        physical.unfenced = true;
        physical.unfenced_stream = reinterpret_cast<cudaStream_t>(stream);
    }
    mmltk::common::logging::profile_add_value("compiled.stream.h2d.bytes", bytes);
    mmltk::common::logging::profile_add_value("compiled.stream.h2d.submissions", 1U);
    gpu::ensure_cuda_ok(cudaMemcpyAsync(device.data(), host.data(), bytes, cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)),
                        "compiled image upload");
}
void CompiledImageStream::wait_transfer(const std::size_t slot) {
    if (!impl_->config.loading.h2d_dataloader) {
        (void)wait_read(slot);
        return;
    }
    on_context(impl_->context,
               [&] { gpu::ensure_cuda_ok(cudaEventSynchronize(impl_->slots.at(slot)->transfer), "compiled image transfer wait"); });
}
void CompiledImageStream::handoff(const std::size_t slot, void* stream) {
    if (!impl_->config.loading.h2d_dataloader) {
        (void)wait_read(slot);
        return;
    }
    on_context(impl_->context, [&] {
        gpu::ensure_cuda_ok(cudaStreamWaitEvent(reinterpret_cast<cudaStream_t>(stream), impl_->slots.at(slot)->transfer, 0),
                            "compiled image consumer handoff");
    });
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
    impl_->changed.wait(lock, [&] { return impl_->consumers == 0U; });
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
    impl_->changed.wait(lock, [&] { return impl_->queued == 0 && impl_->active == 0; });
    if (impl_->failure) std::rethrow_exception(impl_->failure);
}
int CompiledImageStream::reset_storage() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->completion_failed) return cudaErrorUnknown;
        if (impl_->queued != 0 || impl_->active != 0 || std::ranges::any_of(impl_->slots, [](const auto& slot) {
                return slot->reading || slot->transfer_pending || slot->consumer_pending || slot->unfenced;
            }))
            return cudaErrorNotReady;
    }
    for (auto& slot : impl_->slots) {
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
    return std::ranges::any_of(impl_->slots,
                               [](const auto& slot) { return slot->host.owns_allocation() || slot->metadata.owns_allocation(); }) ||
           std::ranges::any_of(impl_->devices, [](const auto& device) { return device->owns_allocation(); });
}
bool CompiledImageStream::owns_resources() const noexcept {
    return owns_allocation() || impl_->copy != nullptr ||
           std::ranges::any_of(impl_->slots, [](const auto& slot) { return slot->transfer != nullptr || slot->consumer != nullptr; });
}

}  // namespace mmltk::backend::data
