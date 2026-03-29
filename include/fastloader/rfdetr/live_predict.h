#pragma once

#include "fastloader/rfdetr/evaluation.h"
#include "fastloader/rfdetr/model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastloader::rfdetr {

struct LiveCaptureRegion {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct LiveVideoSourceOptions {
    std::string device_path = "/dev/video0";
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 120;
    uint32_t v4l2_buffer_count = 1;
    uint32_t preview_buffer_count = 1;
    LiveCaptureRegion initial_region{};
};

struct LivePredictOptions : ModelArtifactRequest {
    LiveVideoSourceOptions source;
    std::string backend = "auto";
    size_t max_dets_per_image = 500;
    uint32_t split_count = 1;
    int device_id = 0;
    float threshold = 0.0f;
    bool include_masks = false;
    bool include_status_detections = false;
    bool allow_fp16 = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

struct LiveCaptureStats {
    uint64_t queued_v4l2_buffers = 0;
    uint64_t dequeued_v4l2_buffers = 0;
    uint64_t bytes_captured = 0;
    uint64_t inference_frames_published = 0;
    uint64_t frames_dropped = 0;
    uint64_t empty_frames_dropped = 0;
    uint64_t inference_backpressure_drops = 0;
    uint64_t short_frames = 0;
    uint64_t sequence_gaps = 0;
    uint64_t requeue_failures = 0;
    bool running = false;
};

struct LivePreviewFormatInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes_per_line = 0;
};

struct LivePreviewBuffer {
    std::uintptr_t data = 0;
    std::size_t pitch_bytes = 0;
    uint32_t x_px = 0;
    uint32_t y_px = 0;
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    void* ready_event = nullptr;
};

struct LivePreviewFrame {
    uint32_t buffer_index = 0;
    uint64_t frame_id = 0;
    LivePreviewBuffer buffer{};
    uint64_t capture_ns = 0;
    uint64_t ready_ns = 0;
    bool short_frame = false;
};

struct LiveSplitPrediction {
    uint32_t split_index = 0;
    LiveCaptureRegion source_region{};
    std::vector<Prediction> detections;
};

struct LivePredictionFrame {
    uint64_t frame_id = 0;
    uint64_t capture_ns = 0;
    uint64_t ready_ns = 0;
    bool short_frame = false;
    std::vector<LiveSplitPrediction> splits;
};

struct LivePredictStatus {
    bool worker_running = false;
    bool capture_running = false;
    bool model_loading = false;
    bool model_hot = false;
    bool busy = false;
    uint64_t frames_started = 0;
    uint64_t frames_completed = 0;
    uint64_t frames_skipped = 0;
    uint64_t splits_started = 0;
    uint64_t splits_completed = 0;
    uint64_t last_started_frame_id = 0;
    uint64_t last_completed_frame_id = 0;
    double last_latency_ms = 0.0;
    uint32_t active_split_count = 1;
    uint32_t active_input_width = 0;
    uint32_t active_input_height = 0;
    std::string backend_name;
    std::string active_model_path;
    std::string last_error;
    LiveCaptureStats capture;
    LivePredictionFrame last_prediction;
};

bool live_capture_supported();

class LivePredictSession {
public:
    explicit LivePredictSession(const LivePredictOptions& options);
    ~LivePredictSession();

    LivePredictSession(const LivePredictSession&) = delete;
    LivePredictSession& operator=(const LivePredictSession&) = delete;
    LivePredictSession(LivePredictSession&&) noexcept;
    LivePredictSession& operator=(LivePredictSession&&) noexcept;

    void start();
    void stop();
    void configure(uint32_t split_count);
    void set_capture_region(const LiveCaptureRegion& region);
    void set_inference_region(const LiveCaptureRegion& region);
    void clear_inference_region();
    LiveCaptureRegion snapshot_capture_region() const;
    LivePredictStatus snapshot_status() const;
    LivePreviewFormatInfo snapshot_preview_format() const;
    bool try_acquire_latest_preview(LivePreviewFrame* out_frame, std::string* error_message = nullptr);
    bool release_preview(uint32_t buffer_index, std::string* error_message = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fastloader::rfdetr
