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

class CudaStreamManager {
   public:
    explicit CudaStreamManager(CudaStreamManagerConfig config);
    ~CudaStreamManager();

    CudaStreamManager(const CudaStreamManager&) = delete;
    CudaStreamManager& operator=(const CudaStreamManager&) = delete;

    [[nodiscard]] float* device_buffer(int buf_idx) const;

    void async_h2d(int buf_idx, const void* host_src, size_t bytes);

    void wait_for_transfer(int buf_idx);
    void handoff_to_stream(int buf_idx, void* consumer_stream);
    void record_consumer_done(int buf_idx, void* consumer_stream);
    bool slot_reusable(int buf_idx);

    void sync();

    [[nodiscard]] void* copy_stream() const;

    [[nodiscard]] float* gather_buffer(int buf_idx) const;

   private:
    [[nodiscard]] size_t slot_index(int buf_idx) const;

    size_t slot_count_;
    struct Impl;
    Impl* impl_;
};

}  // namespace mmltk
