#pragma once

#include "live/live_helpers.h"
#include "mmltk/live/live_analyzer_worker.h"
#include "mmltk/live/live_frame_fanout.h"
#include "mmltk/live/live_manual_overlay_worker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mmltk::live {

class LiveCompositor {
public:
    struct Status {
        bool running = false;
        std::uint64_t frames_composited = 0;
        std::uint64_t frames_dropped = 0;
        LiveFrameId last_frame_id{};
        bool manual_overlay_active = false;
        bool analysis_overlay_active = false;
        std::string last_error;
    };

    LiveCompositor(LiveFrameFanout& fanout,
                   LiveAnalyzerWorker* analyzer_worker,
                   LiveManualOverlayWorker* manual_worker,
                   int cuda_device_index,
                   std::uint32_t max_capture_width,
                   std::uint32_t max_capture_height,
                   std::uint32_t output_slot_count = 3,
                   std::uint32_t raw_base_cache_slot_count = 3);
    ~LiveCompositor();

    LiveCompositor(const LiveCompositor&) = delete;
    LiveCompositor& operator=(const LiveCompositor&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool try_acquire_latest_output(OutputBundle* out);
    void release_output(std::uint32_t slot_index);
    void set_persistent_analysis_overlay(bool enabled);
    [[nodiscard]] bool persistent_analysis_overlay() const noexcept;
    [[nodiscard]] Status snapshot_status() const;
    [[nodiscard]] bool readback_raw_base(const LiveFrameId& frame_id,
                                         std::vector<std::uint8_t>* pixels_bgr,
                                         std::uint32_t* width,
                                         std::uint32_t* height,
                                         LiveCaptureRegion* region,
                                         std::string* error_message);

private:
    struct CompositorSlot {
        std::uint32_t slot_index = 0;
        PitchedDeviceBuffer<> device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        std::uint64_t capture_ns = 0;
        std::uint64_t ready_ns = 0;
        bool short_frame = false;
    };

    struct RawBaseCacheSlot {
        PitchedDeviceBuffer<> device_buffer;
        CudaEventHandle ready_event;
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        bool occupied = false;
    };

    void allocate_resources();
    void destroy_resources() noexcept;
    void compositor_thread_main();
    [[nodiscard]] CompositorSlot* reserve_output_slot();
    [[nodiscard]] bool try_acquire_output_slot(CompositorSlot& slot, OutputBundle* out);
    void cache_raw_base(cudaStream_t source_stream,
                        const OutputBundle& base,
                        std::size_t copy_width_bytes,
                        std::size_t cache_index);
    void publish_error(std::string error_message);

    LiveFrameFanout& fanout_;
    LiveAnalyzerWorker* analyzer_worker_ = nullptr;
    LiveManualOverlayWorker* manual_worker_ = nullptr;
    int cuda_device_index_ = 0;
    std::uint32_t max_capture_width_ = 0;
    std::uint32_t max_capture_height_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> persistent_analysis_overlay_{false};
    std::shared_ptr<const Status> status_{std::make_shared<Status>()};
    std::shared_ptr<const std::string> last_error_{std::make_shared<std::string>()};
    std::vector<std::unique_ptr<CompositorSlot>> output_slots_;
    std::vector<RawBaseCacheSlot> raw_base_cache_slots_;
    std::atomic<int> latest_output_index_{-1};
    std::size_t next_raw_base_cache_index_ = 0;

    PitchedDeviceBuffer<> retained_analysis_overlay_;
    CudaEventHandle retained_analysis_ready_event_;
    CudaStreamHandle retained_analysis_stream_;
    bool retained_analysis_has_content_ = false;

    std::mutex readback_mutex_;
    CudaStreamHandle readback_stream_;
    CudaEventHandle readback_ready_event_;
    PinnedUploadBuffer<std::uint8_t> readback_host_buffer_;
    int current_cuda_device_index_ = -1;
};

} // namespace mmltk::live
