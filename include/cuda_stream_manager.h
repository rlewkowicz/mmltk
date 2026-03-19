#pragma once
#include <cstddef>
#include <cstdint>

namespace fastloader {

// Manages reusable host/device batch buffers plus the loader H2D copy stream.
class CudaStreamManager {
public:
    // batch_capacity: max images per batch
    // image_stride: bytes per image (C*H*W*sizeof(float))
    // num_buffers: number of reusable batch slots kept in flight
    // enable_gather_buffers: allocate pinned host gather buffers for shuffled batches
    // device_id: CUDA device
    CudaStreamManager(size_t batch_capacity, size_t image_stride,
                      size_t num_buffers, bool enable_gather_buffers, int device_id = 0);
    ~CudaStreamManager();

    CudaStreamManager(const CudaStreamManager&) = delete;
    CudaStreamManager& operator=(const CudaStreamManager&) = delete;

    // Reusable device memory for batch DMA targets.
    // The loader keeps one device buffer per batch slot.
    float* device_buffer(int buf_idx) const;

    // Async H2D copy from the supplied host span into device_buffer[buf_idx].
    void async_h2d(int buf_idx, const void* host_src, size_t bytes);

    // Wait until the most recent transfer using the slot has completed.
    void wait_for_transfer(int buf_idx);
    void handoff_to_stream(int buf_idx, void* consumer_stream);
    void record_consumer_done(int buf_idx, void* consumer_stream);
    bool slot_reusable(int buf_idx);

    // Synchronize the loader copy stream and all slot consumer completions.
    void sync();

    // Raw CUDA copy stream handle for diagnostics or explicit external integration.
    void* copy_stream() const;

    // Pinned gather buffer for shuffled batches. Throws if gather buffers are disabled.
    float* gather_buffer(int buf_idx) const;

private:
    size_t slot_index(int buf_idx) const;

    size_t slot_count_;
    struct Impl;
    Impl* impl_;
};

} // namespace fastloader
