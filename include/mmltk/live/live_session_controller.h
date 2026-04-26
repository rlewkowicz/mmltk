#pragma once

#include "mmltk/live/crop_state.h"
#include "mmltk/live/live_analyzer_worker.h"
#include "mmltk/live/live_compositor.h"
#include "mmltk/live/live_frame_fanout.h"
#include "mmltk/live/live_manual_overlay_worker.h"
#include "mmltk/live/manual_overlay_document.h"
#include "mmltk/live/live_video_ingress.h"

#include <frameshow/capture_types.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {

struct LiveSessionConfig {
    frameshow::CaptureConfig capture{};
    std::uint32_t detect_slot_count = 2;
    std::uint32_t output_slot_count = 4;
    std::uint32_t analyzer_result_slot_count = 2;
    std::uint32_t analyzer_overlay_slot_count = 2;
    std::uint32_t manual_overlay_slot_count = 2;
    std::uint32_t raw_base_cache_slot_count = 3;
    int cuda_device_index = 0;
    bool persistent_analysis_overlay = false;
};

struct LiveSessionStatus {
    bool running = false;
    std::string last_error;
    LiveAnalyzerWorker::Status analyzer{};
    LiveManualOverlayWorker::Status manual{};
    LiveCompositor::Status compositor{};
};

class LiveSessionController {
   public:
    explicit LiveSessionController(LiveSessionConfig config);
    ~LiveSessionController();

    LiveSessionController(const LiveSessionController&) = delete;
    LiveSessionController& operator=(const LiveSessionController&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] LiveSessionStatus snapshot_status() const;

    [[nodiscard]] bool try_acquire_latest_workspace(WorkspaceOutputBundle* out);
    void release_workspace(std::uint32_t slot_index);
    void set_workspace_ready_callback(LiveCompositor::WorkspaceReadyCallback callback);

    [[nodiscard]] bool try_acquire_latest_detect(DetectBundle* out);
    void release_detect(std::uint32_t slot_index);

    [[nodiscard]] UiCropState& ui_crop_state() noexcept;
    [[nodiscard]] const UiCropState& ui_crop_state() const noexcept;
    [[nodiscard]] ManualOverlayDocument& manual_overlay_document() noexcept;
    [[nodiscard]] const ManualOverlayDocument& manual_overlay_document() const noexcept;

    void attach_analyzer(std::unique_ptr<FrameAnalyzer> analyzer);
    void clear_analyzer();
    void set_persistent_analysis_overlay(bool enabled);
    [[nodiscard]] bool persistent_analysis_overlay() const noexcept;
    [[nodiscard]] bool readback_raw_base(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                         std::uint32_t* width, std::uint32_t* height, LiveCaptureRegion* region,
                                         std::string* error_message);

    [[nodiscard]] LiveVideoIngress& ingress() noexcept;
    [[nodiscard]] const LiveVideoIngress& ingress() const noexcept;
    [[nodiscard]] LiveFrameFanout& fanout() noexcept;
    [[nodiscard]] const LiveFrameFanout& fanout() const noexcept;
    [[nodiscard]] LiveAnalyzerWorker* analyzer_worker() noexcept;
    [[nodiscard]] const LiveAnalyzerWorker* analyzer_worker() const noexcept;
    [[nodiscard]] LiveManualOverlayWorker* manual_overlay_worker() noexcept;
    [[nodiscard]] const LiveManualOverlayWorker* manual_overlay_worker() const noexcept;
    [[nodiscard]] LiveCompositor& compositor() noexcept;
    [[nodiscard]] const LiveCompositor& compositor() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::live
