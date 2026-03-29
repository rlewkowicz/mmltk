#pragma once

#include "annotation_core.h"
#include "source_selection.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fastloader::gui {

struct AnnotationLiveCaptureSnapshot {
    bool running = false;
    bool has_frame = false;
    std::uint64_t frames_captured = 0;
    AnnotationFrame frame;
    std::string last_error;
};

class AnnotationLiveCaptureSession {
public:
    AnnotationLiveCaptureSession();
    ~AnnotationLiveCaptureSession();

    AnnotationLiveCaptureSession(const AnnotationLiveCaptureSession&) = delete;
    AnnotationLiveCaptureSession& operator=(const AnnotationLiveCaptureSession&) = delete;

    void start(const SourceSelectionState& source, int cuda_device_index, bool full_frame);
    void stop();
    void update_preview_region(const SourceSelectionState& source, bool full_frame);
    AnnotationLiveCaptureSnapshot snapshot() const;

private:
    void worker_main(SourceSelectionState source, int cuda_device_index, bool full_frame);

    mutable std::mutex mutex_;
    std::thread worker_thread_;
    bool stop_requested_ = false;
    bool preview_region_pending_ = false;
    std::uint32_t pending_region_x_ = 0;
    std::uint32_t pending_region_y_ = 0;
    std::uint32_t pending_region_width_ = 0;
    std::uint32_t pending_region_height_ = 0;
    AnnotationLiveCaptureSnapshot snapshot_{};
};

} // namespace fastloader::gui
