#pragma once

#include "mmltk/live/live_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mmltk::live {

class LiveAnalyzerWorker;
class LiveFrameFanout;
class LiveManualOverlayWorker;

class LiveCompositor {
   public:
    using WorkspaceReadyCallback = std::function<void()>;

    struct Status {
        bool running = false;
        std::uint64_t frames_composited = 0;
        std::uint64_t frames_dropped = 0;
        LiveFrameId last_frame_id{};
        bool manual_overlay_active = false;
        bool analysis_overlay_active = false;
        std::string last_error;
    };

    LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer_worker, LiveManualOverlayWorker* manual_worker,
                   int cuda_device_index, std::uint32_t max_capture_width, std::uint32_t max_capture_height,
                   std::uint32_t output_slot_count = 3, std::uint32_t raw_base_cache_slot_count = 3);
    ~LiveCompositor();

    LiveCompositor(const LiveCompositor&) = delete;
    LiveCompositor& operator=(const LiveCompositor&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool try_acquire_latest_workspace(WorkspaceOutputBundle* out);
    void release_workspace(std::uint32_t slot_index);
    void set_workspace_ready_callback(WorkspaceReadyCallback callback);
    void set_persistent_analysis_overlay(bool enabled);
    [[nodiscard]] bool persistent_analysis_overlay() const noexcept;
    [[nodiscard]] Status snapshot_status() const;
    [[nodiscard]] bool readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                         std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                         std::string* error_message);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::live
