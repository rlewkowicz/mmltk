#pragma once

#include "mmltk/live/live_compositor_status.h"
#include "mmltk/live/live_pipeline_status.h"
#include "mmltk/live/live_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmltk::live {

class LiveAnalyzerWorker;
class LiveFrameFanout;
class LiveManualOverlayWorker;
class WorkspaceSurfacePool;

class LiveCompositor {
   public:
    struct Status : LivePipelineStatus, LiveCompositorTelemetry {
        LiveFrameId last_frame_id{};
        bool manual_overlay_active = false;
        bool analysis_overlay_active = false;
        std::string last_error;
    };

    LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer_worker, LiveManualOverlayWorker* manual_worker,
                   int cuda_device_index, std::uint32_t max_capture_width, std::uint32_t max_capture_height,
                   std::shared_ptr<WorkspaceSurfacePool> workspace_pool,
                   std::shared_ptr<LivePipelineTrace> pipeline_trace = {});
    ~LiveCompositor();

    LiveCompositor(const LiveCompositor&) = delete;
    LiveCompositor& operator=(const LiveCompositor&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] WorkspaceSwapchainDescriptor workspace_swapchain_descriptor() const;
    [[nodiscard]] WorkspacePresentSnapshot latest_workspace_present() const noexcept;
    [[nodiscard]] bool request_next_workspace_source_frame(std::uint64_t after_revision,
                                                           const std::atomic<bool>& stop_requested,
                                                           WorkspaceSourceFrameLease* out);
    void release_workspace_source_frame(const WorkspaceSourceFrameLease& lease) noexcept;
    [[nodiscard]] bool try_acquire_latest_workspace(WorkspaceOutputBundle* out);
    void release_workspace(std::uint32_t slot_index);
    [[nodiscard]] bool acquire_workspace_slot_for_import(std::uint32_t slot_index);
    [[nodiscard]] bool mark_workspace_slot_in_flight(std::uint32_t slot_index, bool in_flight);
    [[nodiscard]] bool import_workspace_timeline_semaphore(std::uint64_t generation, std::uint32_t slot_index,
                                                           int semaphore_fd, std::string* error_message);
    [[nodiscard]] bool workspace_timeline_sync_ready(std::uint64_t generation) const noexcept;
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

}  
