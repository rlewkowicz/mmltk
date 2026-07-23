#pragma once

#include "mmltk/live/live_frame_id.h"
#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/manual_overlay_document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {

class WorkspaceSurfacePool;

class StaticWorkspaceCompositor {
   public:
    StaticWorkspaceCompositor(std::shared_ptr<WorkspaceSurfacePool> workspace_pool, int cuda_device_index,
                              std::uint32_t max_width, std::uint32_t max_height, std::uint32_t overlay_slot_count = 2,
                              std::uint32_t overlay_canvas_width = 0, std::uint32_t overlay_canvas_height = 0);
    ~StaticWorkspaceCompositor();

    StaticWorkspaceCompositor(const StaticWorkspaceCompositor&) = delete;
    StaticWorkspaceCompositor& operator=(const StaticWorkspaceCompositor&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept;

    void set_source_bgr(const std::uint8_t* pixels_bgr, std::size_t byte_count, std::uint32_t width,
                        std::uint32_t height, LiveFrameId frame_id, std::string source_name = {},
                        LiveCaptureRegion source_region = {});
    void clear_source() noexcept;
    [[nodiscard]] bool refresh();

    [[nodiscard]] ManualOverlayDocument& overlay_document() noexcept;
    [[nodiscard]] const ManualOverlayDocument& overlay_document() const noexcept;
    [[nodiscard]] std::string last_error() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  
