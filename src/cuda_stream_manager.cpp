#include "cuda_stream_manager.h"
#include "cuda_priority.h"
#include "profile_utils.h"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#define CUDA_CHECK(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        char msg[256]; \
        snprintf(msg, sizeof(msg), "CUDA error at %s:%d: %s", __FILE__, __LINE__, cudaGetErrorString(err)); \
        throw std::runtime_error(msg); \
    } \
} while(0)

namespace fastloader {

namespace {

struct PoolKey {
    size_t batch_capacity = 0;
    size_t image_stride = 0;
    size_t num_buffers = 0;
    bool enable_gather_buffers = false;
    int device_id = 0;

    bool operator==(const PoolKey& other) const {
        return batch_capacity == other.batch_capacity &&
               image_stride == other.image_stride &&
               num_buffers == other.num_buffers &&
               enable_gather_buffers == other.enable_gather_buffers &&
               device_id == other.device_id;
    }
};

struct DeviceSlot {
    float* device = nullptr;
#if FASTLOADER_ENABLE_PROFILING
    cudaEvent_t transfer_start = nullptr;
#endif
    cudaEvent_t transfer_done = nullptr;
    cudaEvent_t consumer_done = nullptr;
    bool transfer_pending = false;
    bool consumer_pending = false;
};

struct HostSlot {
    float* gather = nullptr;
};

std::uint64_t elapsed_ms_to_ns(float elapsed_ms) {
    return static_cast<std::uint64_t>(
        std::llround(static_cast<double>(elapsed_ms) * 1.0e6));
}

[[noreturn]] void throw_cuda_query_error(cudaError_t err) {
    char msg[256];
    std::snprintf(msg,
                  sizeof(msg),
                  "CUDA error at %s:%d: %s",
                  __FILE__,
                  __LINE__,
                  cudaGetErrorString(err));
    throw std::runtime_error(msg);
}

bool query_event_complete(cudaEvent_t event) {
    cudaError_t err = cudaEventQuery(event);
    if (err == cudaSuccess) {
        return true;
    }
    if (err == cudaErrorNotReady) {
        return false;
    }
    throw_cuda_query_error(err);
    return false;
}

struct SlotPool {
    PoolKey key{};
    size_t buf_bytes = 0;
    cudaStream_t copy_stream = nullptr;
    std::vector<DeviceSlot> device_slots;
    std::vector<HostSlot> host_slots;
    bool leased = false;

    void reap_transfer(DeviceSlot& slot, bool stream_already_synchronized) {
        if (!slot.transfer_pending) {
            return;
        }
        if (!stream_already_synchronized) {
            CUDA_CHECK(cudaEventSynchronize(slot.transfer_done));
        }
#if FASTLOADER_ENABLE_PROFILING
        float elapsed_ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, slot.transfer_start, slot.transfer_done));
        FASTLOADER_PROFILE_RECORD_DURATION_NS("cuda.async_h2d.gpu_active",
                                              elapsed_ms_to_ns(elapsed_ms));
#endif
        slot.transfer_pending = false;
    }

    bool slot_reusable(DeviceSlot& slot) {
        if (slot.transfer_pending && !query_event_complete(slot.transfer_done)) {
            return false;
        }
        reap_transfer(slot, true);
        if (slot.consumer_pending && !query_event_complete(slot.consumer_done)) {
            return false;
        }
        slot.consumer_pending = false;
        return true;
    }
};

void destroy_pool(SlotPool& pool) noexcept {
    cudaSetDevice(pool.key.device_id);
    if (pool.copy_stream) {
        cudaStreamSynchronize(pool.copy_stream);
    }
    for (auto& slot : pool.device_slots) {
#if FASTLOADER_ENABLE_PROFILING
        if (slot.transfer_start) {
            cudaEventDestroy(slot.transfer_start);
        }
#endif
        if (slot.transfer_done) {
            cudaEventDestroy(slot.transfer_done);
        }
        if (slot.consumer_done) {
            cudaEventDestroy(slot.consumer_done);
        }
        if (slot.device) {
            cudaFree(slot.device);
        }
    }
    for (auto& slot : pool.host_slots) {
        if (slot.gather) {
            cudaFreeHost(slot.gather);
        }
    }
    if (pool.copy_stream) {
        cudaStreamDestroy(pool.copy_stream);
    }
}

std::unique_ptr<SlotPool> create_pool(const PoolKey& key) {
    CUDA_CHECK(cudaSetDevice(key.device_id));

    auto pool = std::make_unique<SlotPool>();
    pool->key = key;
    pool->buf_bytes = key.batch_capacity * key.image_stride;
    pool->device_slots.resize(key.num_buffers);
    pool->host_slots.resize(key.num_buffers);

    CUDA_CHECK(fastloader::cuda_stream_create_with_highest_priority(&pool->copy_stream, cudaStreamNonBlocking));
    for (auto& slot : pool->device_slots) {
        CUDA_CHECK(cudaMalloc(&slot.device, pool->buf_bytes));
#if FASTLOADER_ENABLE_PROFILING
        CUDA_CHECK(cudaEventCreate(&slot.transfer_start));
        CUDA_CHECK(cudaEventCreate(&slot.transfer_done));
#else
        CUDA_CHECK(cudaEventCreateWithFlags(&slot.transfer_done, cudaEventDisableTiming));
#endif
        CUDA_CHECK(cudaEventCreateWithFlags(&slot.consumer_done, cudaEventDisableTiming));
    }

    if (key.enable_gather_buffers) {
        for (auto& slot : pool->host_slots) {
            CUDA_CHECK(cudaHostAlloc(&slot.gather,
                                     pool->buf_bytes,
                                     cudaHostAllocDefault));
        }
    }

    return pool;
}

class SlotPoolCache {
public:
    ~SlotPoolCache() {
        for (auto& pool : pools_) {
            destroy_pool(*pool);
        }
    }

    SlotPool* acquire(const PoolKey& key, bool& hit) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pool : pools_) {
            if (pool->key == key && !pool->leased) {
                pool->leased = true;
                hit = true;
                return pool.get();
            }
        }

        auto pool = create_pool(key);
        pool->leased = true;
        SlotPool* acquired = pool.get();
        pools_.push_back(std::move(pool));
        hit = false;
        return acquired;
    }

    void release(SlotPool* pool) {
        std::lock_guard<std::mutex> lock(mutex_);
        pool->leased = false;
    }

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<SlotPool>> pools_;
};

SlotPoolCache& slot_pool_cache() {
    static SlotPoolCache cache;
    return cache;
}

} // namespace

struct CudaStreamManager::Impl {
    SlotPool* pool = nullptr;
};

CudaStreamManager::CudaStreamManager(size_t batch_capacity, size_t image_stride,
                                     size_t num_buffers, bool enable_gather_buffers, int device_id)
    : slot_count_(std::max<size_t>(1, num_buffers)) {
    FASTLOADER_PROFILE_SCOPE("cuda_stream_manager.construct");
    CUDA_CHECK(cudaSetDevice(device_id));

    impl_ = new Impl();
    const PoolKey key{
        batch_capacity,
        image_stride,
        slot_count_,
        enable_gather_buffers,
        device_id,
    };

    bool hit = false;
    impl_->pool = slot_pool_cache().acquire(key, hit);
    FASTLOADER_PROFILE_ADD(hit ? "cuda.slot_pool_hit" : "cuda.slot_pool_miss", 1);

    FASTLOADER_PROFILE_SET("cuda.buffer_bytes_per_slot", impl_->pool->buf_bytes);
    FASTLOADER_PROFILE_SET("cuda.num_buffers", slot_count_);
    FASTLOADER_PROFILE_SET("cuda.device_slots", slot_count_);
    FASTLOADER_PROFILE_SET("cuda.copy_streams", static_cast<size_t>(1));
    FASTLOADER_PROFILE_SET("cuda.copy_stream_count", static_cast<size_t>(1));
    for (size_t i = 0; i < slot_count_; ++i) {
        FASTLOADER_PROFILE_ADD("cuda.device_bytes", impl_->pool->buf_bytes);
        if (enable_gather_buffers) {
            FASTLOADER_PROFILE_ADD("cuda.gather_bytes", impl_->pool->buf_bytes);
        }
    }
}

CudaStreamManager::~CudaStreamManager() {
    FASTLOADER_PROFILE_SCOPE("cuda_stream_manager.destruct");
    if (impl_) {
        slot_pool_cache().release(impl_->pool);
        delete impl_;
    }
}

size_t CudaStreamManager::slot_index(int buf_idx) const {
    return static_cast<size_t>(buf_idx) % slot_count_;
}

float* CudaStreamManager::device_buffer(int buf_idx) const {
    return impl_->pool->device_slots[slot_index(buf_idx)].device;
}

float* CudaStreamManager::gather_buffer(int buf_idx) const {
    float* buf = impl_->pool->host_slots[slot_index(buf_idx)].gather;
    if (!buf) {
        throw std::runtime_error("Gather buffers are disabled for this loader");
    }
    return buf;
}

void CudaStreamManager::async_h2d(int buf_idx, const void* host_src, size_t bytes) {
    FASTLOADER_PROFILE_SCOPE("cuda.async_h2d.submit");
    FASTLOADER_PROFILE_ADD("cuda.async_h2d.bytes", bytes);
    const size_t idx = slot_index(buf_idx);
    auto& slot = impl_->pool->device_slots[idx];
    if (slot.transfer_pending || slot.consumer_pending) {
        throw std::runtime_error("cuda async_h2d slot reused before prior work completed");
    }
#if FASTLOADER_ENABLE_PROFILING
    CUDA_CHECK(cudaEventRecord(slot.transfer_start, impl_->pool->copy_stream));
#endif
    CUDA_CHECK(cudaMemcpyAsync(
        slot.device, host_src, bytes,
        cudaMemcpyHostToDevice, impl_->pool->copy_stream));
    CUDA_CHECK(cudaEventRecord(slot.transfer_done, impl_->pool->copy_stream));
    slot.transfer_pending = true;
}

void CudaStreamManager::wait_for_transfer(int buf_idx) {
    FASTLOADER_PROFILE_SCOPE("cuda.wait_for_transfer");
    const size_t idx = slot_index(buf_idx);
    auto& slot = impl_->pool->device_slots[idx];
    impl_->pool->reap_transfer(slot, false);
}

void CudaStreamManager::handoff_to_stream(int buf_idx, void* consumer_stream) {
    FASTLOADER_PROFILE_SCOPE("cuda.handoff_to_stream");
    FASTLOADER_PROFILE_ADD("cuda.handoff.count", 1);
    const size_t idx = slot_index(buf_idx);
    auto& slot = impl_->pool->device_slots[idx];
    auto* stream = reinterpret_cast<cudaStream_t>(consumer_stream);
    CUDA_CHECK(cudaStreamWaitEvent(stream, slot.transfer_done, 0));
}

void CudaStreamManager::record_consumer_done(int buf_idx, void* consumer_stream) {
    FASTLOADER_PROFILE_SCOPE("cuda.record_consumer_done");
    FASTLOADER_PROFILE_ADD("cuda.consumer_release.count", 1);
    const size_t idx = slot_index(buf_idx);
    auto& slot = impl_->pool->device_slots[idx];
    auto* stream = reinterpret_cast<cudaStream_t>(consumer_stream);
    CUDA_CHECK(cudaEventRecord(slot.consumer_done, stream));
    slot.consumer_pending = true;
}

bool CudaStreamManager::slot_reusable(int buf_idx) {
    const size_t idx = slot_index(buf_idx);
    return impl_->pool->slot_reusable(impl_->pool->device_slots[idx]);
}

void CudaStreamManager::sync() {
    FASTLOADER_PROFILE_SCOPE("cuda.sync");
    CUDA_CHECK(cudaStreamSynchronize(impl_->pool->copy_stream));
    for (auto& slot : impl_->pool->device_slots) {
        impl_->pool->reap_transfer(slot, true);
        if (slot.consumer_pending) {
            CUDA_CHECK(cudaEventSynchronize(slot.consumer_done));
            slot.consumer_pending = false;
        }
    }
}

void* CudaStreamManager::copy_stream() const {
    return impl_->pool->copy_stream;
}

} // namespace fastloader
