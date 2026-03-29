#include "annotation_live_capture.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cuda_runtime_api.h>

#if FASTLOADER_RFDETR_LIVE_CAPTURE
#include <frameshow/capture_session.hpp>
#include <frameshow/capture_types.hpp>
#include <frameshow/status.hpp>
#endif

namespace fastloader::gui {

namespace {

constexpr std::chrono::milliseconds kCapturePollSleep{4};

std::string cuda_status_message(cudaError_t status, const char* label) {
    return std::string(label) + " failed: " + cudaGetErrorString(status);
}

#if FASTLOADER_RFDETR_LIVE_CAPTURE
frameshow::CaptureRegion preview_region(const SourceSelectionState& source, bool full_frame) {
    const ResolvedVideoCrop crop = resolve_video_crop(source);
    return frameshow::CaptureRegion{
        static_cast<std::uint32_t>(full_frame ? 0 : std::max(0, crop.x)),
        static_cast<std::uint32_t>(full_frame ? 0 : std::max(0, crop.y)),
        static_cast<std::uint32_t>(full_frame ? std::max(1, source.capture_width) : std::max(1, crop.width)),
        static_cast<std::uint32_t>(full_frame ? std::max(1, source.capture_height) : std::max(1, crop.height)),
    };
}

frameshow::CaptureConfig make_capture_config(const SourceSelectionState& source, int cuda_device_index, bool full_frame) {
    frameshow::CaptureConfig config;
    config.device_path = "/dev/video" + std::to_string(std::max(0, source.device_index));
    config.cuda_device_index = cuda_device_index;
    config.width = static_cast<std::uint32_t>(std::max(1, source.capture_width));
    config.height = static_cast<std::uint32_t>(std::max(1, source.capture_height));
    config.fps = static_cast<std::uint32_t>(std::max(1, source.capture_fps));
    config.v4l2_buffer_count = static_cast<std::uint32_t>(std::max(1, source.v4l2_buffer_count));
    config.preview_buffer_count = 3U;
    config.initial_region = preview_region(source, full_frame);
    return config;
}
#endif

} // namespace

AnnotationLiveCaptureSession::AnnotationLiveCaptureSession() = default;

AnnotationLiveCaptureSession::~AnnotationLiveCaptureSession() {
    stop();
}

void AnnotationLiveCaptureSession::start(const SourceSelectionState& source, int cuda_device_index, bool full_frame) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        preview_region_pending_ = false;
        snapshot_ = {};
        snapshot_.running = true;
    }
    worker_thread_ = std::thread(&AnnotationLiveCaptureSession::worker_main, this, source, cuda_device_index, full_frame);
}

void AnnotationLiveCaptureSession::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        preview_region_pending_ = false;
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.running = false;
}

AnnotationLiveCaptureSnapshot AnnotationLiveCaptureSession::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void AnnotationLiveCaptureSession::update_preview_region(const SourceSelectionState& source, bool full_frame) {
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
    (void)source;
    (void)full_frame;
#else
    const frameshow::CaptureRegion region = preview_region(source, full_frame);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_region_x_ = region.x;
    pending_region_y_ = region.y;
    pending_region_width_ = region.width;
    pending_region_height_ = region.height;
    preview_region_pending_ = true;
#endif
}

void AnnotationLiveCaptureSession::worker_main(SourceSelectionState source, int cuda_device_index, bool full_frame) {
#if !FASTLOADER_RFDETR_LIVE_CAPTURE
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.running = false;
    snapshot_.last_error =
        "Annotation live capture is unavailable because live capture support was not built into this binary.";
    return;
#else
    try {
        frameshow::CaptureSession session(make_capture_config(source, cuda_device_index, full_frame));
        const frameshow::Status start_status = session.start();
        if (!start_status.ok()) {
            throw std::runtime_error(start_status.message.empty() ? "failed to start annotation live capture"
                                                                  : start_status.message);
        }

        while (true) {
            frameshow::CaptureRegion requested_region{};
            bool apply_region = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) {
                    break;
                }
                if (preview_region_pending_) {
                    requested_region = frameshow::CaptureRegion{
                        pending_region_x_,
                        pending_region_y_,
                        pending_region_width_,
                        pending_region_height_,
                    };
                    preview_region_pending_ = false;
                    apply_region = true;
                }
            }

            if (apply_region) {
                const frameshow::Status region_status = session.set_capture_region(requested_region);
                if (!region_status.ok()) {
                    throw std::runtime_error(region_status.message.empty() ? "failed to update annotation preview region"
                                                                          : region_status.message);
                }
            }

            frameshow::PreviewFrameView view{};
            const frameshow::Status acquire_status = session.try_acquire_latest_preview(&view);
            if (acquire_status.code == frameshow::StatusCode::kNotReady) {
                std::this_thread::sleep_for(kCapturePollSleep);
                continue;
            }
            if (!acquire_status.ok()) {
                throw std::runtime_error(acquire_status.message.empty() ? "failed to acquire annotation live preview"
                                                                        : acquire_status.message);
            }

            const std::uint32_t width = view.buffer.width_px;
            const std::uint32_t height = view.buffer.height_px;
            std::vector<std::uint8_t> pixels_bgr(static_cast<std::size_t>(width) *
                                                     static_cast<std::size_t>(height) * 3U,
                                                 0U);
            cudaError_t cuda_status = cudaSuccess;
            if (view.buffer.ready_event != nullptr) {
                cuda_status = cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(view.buffer.ready_event));
                if (cuda_status != cudaSuccess) {
                    (void)session.release_preview(view.buffer_index);
                    throw std::runtime_error(cuda_status_message(cuda_status, "cudaEventSynchronize"));
                }
            }
            cuda_status = cudaMemcpy2D(pixels_bgr.data(),
                                       static_cast<std::size_t>(width) * 3U,
                                       reinterpret_cast<const void*>(view.buffer.data),
                                       view.buffer.pitch_bytes,
                                       static_cast<std::size_t>(width) * 3U,
                                       height,
                                       cudaMemcpyDeviceToHost);
            const frameshow::Status release_status = session.release_preview(view.buffer_index);
            if (cuda_status != cudaSuccess) {
                throw std::runtime_error(cuda_status_message(cuda_status, "cudaMemcpy2D"));
            }
            if (!release_status.ok()) {
                throw std::runtime_error(release_status.message.empty() ? "failed to release annotation preview buffer"
                                                                        : release_status.message);
            }

            AnnotationFrame frame;
            frame.source_name = "live";
            frame.source_path = "/dev/video" + std::to_string(std::max(0, source.device_index));
            frame.frame_id = view.frame_id;
            frame.width = width;
            frame.height = height;
            frame.view_x = view.buffer.x_px;
            frame.view_y = view.buffer.y_px;
            frame.capture_width = static_cast<std::uint32_t>(std::max(1, source.capture_width));
            frame.capture_height = static_cast<std::uint32_t>(std::max(1, source.capture_height));
            frame.pixels_bgr = std::move(pixels_bgr);

            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.has_frame = true;
            snapshot_.frames_captured += 1U;
            snapshot_.frame = std::move(frame);
            snapshot_.last_error.clear();
        }

        const frameshow::Status stop_status = session.stop();
        if (!stop_status.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.last_error = stop_status.message.empty() ? "failed to stop annotation live capture"
                                                               : stop_status.message;
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.last_error = error.what();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.running = false;
#endif
}

} // namespace fastloader::gui
