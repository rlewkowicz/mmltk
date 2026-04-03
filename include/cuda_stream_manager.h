#pragma once
#include <cstddef>
#include <cstdint>

namespace mmltk {

struct CudaStreamManagerConfig {
    size_t batch_capacity = 0;
    size_t image_stride = 0;
    size_t num_buffers = 0;
    bool enable_gather_buffers = false;
    int device_id = 0;
};

// Manages reusable host/device batch buffers plus the loader H2D copy stream.
class CudaStreamManager {
public:
    explicit CudaStreamManager(CudaStreamManagerConfig config);
    ~CudaStreamManager();

    CudaStreamManager(const CudaStreamManager&) = delete;
    CudaStreamManager& operator=(const CudaStreamManager&) = delete;

    // Reusable device memory for batch DMA targets.
    // The loader keeps one device buffer per batch slot.
    [[nodiscard]] float* device_buffer(int buf_idx) const;

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
    [[nodiscard]] void* copy_stream() const;

    // Pinned gather buffer for shuffled batches. Throws if gather buffers are disabled.
    [[nodiscard]] float* gather_buffer(int buf_idx) const;

private:
    [[nodiscard]] size_t slot_index(int buf_idx) const;

    size_t slot_count_;
    struct Impl;
    Impl* impl_;
};

} // namespace mmltk
