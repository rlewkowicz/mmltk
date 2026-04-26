#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <cuda.h>
#include <cuda_runtime_api.h>

namespace frameshow {

struct CaptureRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct CaptureConfig {
    std::string device_path = "/dev/video0";
    int cuda_device_index = 0;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t fps = 120;
    std::uint32_t v4l2_buffer_count = 4;
    std::uint32_t preview_buffer_count = 3;
    CaptureRegion initial_region{};
};

struct CaptureFormatInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes_per_line = 0;
};

struct GpuFrameBuffer {
    CUdeviceptr data = 0;
    std::size_t pitch_bytes = 0;
    std::uint32_t x_px = 0;
    std::uint32_t y_px = 0;
    std::uint32_t width_px = 0;
    std::uint32_t height_px = 0;
    cudaEvent_t ready_event = nullptr;
};

struct GpuFrameView {
    std::uint32_t buffer_index = 0;
    std::uint64_t frame_id = 0;
    GpuFrameBuffer buffer{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
};

using InferenceBuffer = GpuFrameBuffer;
using PreviewBuffer = GpuFrameBuffer;
using InferenceFrameView = GpuFrameView;
using PreviewFrameView = GpuFrameView;

struct CaptureStats {
    std::uint64_t queued_v4l2_buffers = 0;
    std::uint64_t dequeued_v4l2_buffers = 0;
    std::uint64_t bytes_captured = 0;
    std::uint64_t inference_frames_published = 0;
    std::uint64_t preview_frames_published = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t empty_frames_dropped = 0;
    std::uint64_t inference_backpressure_drops = 0;
    std::uint64_t preview_backpressure_drops = 0;
    std::uint64_t short_frames = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t requeue_failures = 0;
    bool running = false;
};

}  // namespace frameshow
