#include "mmltk/live/live_video_ingress.h"

#include <frameshow/status.hpp>

#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "mmltk_logging.h"

namespace mmltk::live {

namespace {

void log_live_ingress_message(const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    auto logger = mmltk::logging::logger("live.ingress");
    if (std::string_view(level) == "error") {
        logger->error("{}", message);
    } else if (std::string_view(level) == "warn") {
        logger->warn("{}", message);
    } else {
        logger->info("{}", message);
    }
}

std::uint64_t generate_session_nonce() {
    std::random_device random_device;
    const std::uint64_t random_component =
        (static_cast<std::uint64_t>(random_device()) << 32U) ^ static_cast<std::uint64_t>(random_device());
    const std::uint64_t clock_component =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t nonce = random_component ^ clock_component;
    if (nonce == 0U) {
        nonce = 1U;
    }
    return nonce;
}

LiveCaptureRegion to_live_region(const frameshow::CaptureRegion& region) {
    return LiveCaptureRegion{region.x, region.y, region.width, region.height};
}

frameshow::CaptureRegion to_frameshow_region(const LiveCaptureRegion& region) {
    return frameshow::CaptureRegion{region.x, region.y, region.width, region.height};
}

std::runtime_error status_error(const char* action, const frameshow::Status& status) {
    std::string message = action;
    if (!status.message.empty()) {
        message += ": ";
        message += status.message;
    }
    return std::runtime_error(message);
}

SourceFrameView source_view_from_inference_frame(const frameshow::InferenceFrameView& frame,
                                                 const std::uint64_t session_nonce) {
    return SourceFrameView{
        frame.buffer_index,
        LiveFrameId{session_nonce, frame.frame_id},
        frame.buffer.data,
        frame.buffer.pitch_bytes,
        frame.buffer.width_px,
        frame.buffer.height_px,
        frame.buffer.ready_event,
        to_live_region(frameshow::CaptureRegion{frame.buffer.x_px, frame.buffer.y_px, frame.buffer.width_px,
                                                frame.buffer.height_px}),
        frame.capture_ns,
        frame.ready_ns,
        frame.short_frame,
    };
}

}  // namespace

LiveVideoIngress::LiveVideoIngress(frameshow::CaptureConfig config) : config_(std::move(config)) {}

LiveVideoIngress::~LiveVideoIngress() {
    try {
        stop();
    } catch (const std::exception& error) {
        log_live_ingress_message("error", error.what());
    }
}

void LiveVideoIngress::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("live ingress is already running");
    }

    auto session = std::make_unique<frameshow::CaptureSession>(config_);
    const frameshow::Status status = session->start();
    if (!status.ok()) {
        throw status_error("failed to start live ingress", status);
    }

    session_nonce_.store(generate_session_nonce(), std::memory_order_release);
    session_ = std::move(session);
    running_.store(true, std::memory_order_release);
    log_live_ingress_message(
        "info", "started capture on " + config_.device_path + " region=" + std::to_string(config_.initial_region.x) +
                    "," + std::to_string(config_.initial_region.y) + "," +
                    std::to_string(config_.initial_region.width) + "x" + std::to_string(config_.initial_region.height));
}

void LiveVideoIngress::stop() {
    std::unique_ptr<frameshow::CaptureSession> session;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!session_) {
            running_.store(false, std::memory_order_release);
            return;
        }
        session = std::move(session_);
        running_.store(false, std::memory_order_release);
        session->notify_inference_waiters();
    }

    const frameshow::Status status = session->stop();
    if (!status.ok()) {
        throw status_error("failed to stop live ingress", status);
    }
    log_live_ingress_message("info", "stopped capture on " + config_.device_path);
}

bool LiveVideoIngress::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool LiveVideoIngress::try_acquire_latest_source(SourceFrameView* out) {
    if (out == nullptr) {
        throw std::runtime_error("live ingress requires a non-null source frame output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!session_ || !running_.load(std::memory_order_acquire)) {
        return false;
    }

    frameshow::InferenceFrameView frame{};
    const frameshow::Status status = session_->try_acquire_latest_inference_frame(&frame);
    if (status.code == frameshow::StatusCode::kNotReady) {
        return false;
    }
    if (!status.ok()) {
        throw status_error("failed to acquire latest live source frame", status);
    }

    *out = source_view_from_inference_frame(frame, session_nonce_.load(std::memory_order_acquire));
    return true;
}

bool LiveVideoIngress::wait_acquire_latest_source(SourceFrameView* out, const std::atomic<bool>& stop_requested) {
    if (out == nullptr) {
        throw std::runtime_error("live ingress requires a non-null source frame output");
    }
    *out = {};
    if (!running()) {
        return false;
    }
    frameshow::CaptureSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!session_ || !running_.load(std::memory_order_acquire)) {
            return false;
        }
        session = session_.get();
    }

    frameshow::InferenceFrameView frame{};
    const frameshow::Status status = session->wait_acquire_latest_inference_frame(&frame, stop_requested);
    if (status.code == frameshow::StatusCode::kNotReady) {
        return false;
    }
    if (!status.ok()) {
        throw status_error("failed to acquire latest live source frame", status);
    }

    *out = source_view_from_inference_frame(frame, session_nonce_.load(std::memory_order_acquire));
    return true;
}

void LiveVideoIngress::notify_source_waiters() noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (session_ != nullptr) {
        session_->notify_inference_waiters();
    }
}

void LiveVideoIngress::release_source(std::uint32_t buffer_index) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!session_) {
        throw std::runtime_error("live ingress cannot release a source buffer without an active session");
    }
    const frameshow::Status status = session_->release_inference_frame(buffer_index);
    if (!status.ok()) {
        throw status_error("failed to release live source frame", status);
    }
}

void LiveVideoIngress::set_capture_region(const LiveCaptureRegion& region) {
    const frameshow::CaptureRegion next_region = to_frameshow_region(region);
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    config_.initial_region = next_region;
    if (!session_) {
        return;
    }
    const frameshow::Status status = session_->set_capture_region(config_.initial_region);
    if (!status.ok()) {
        throw status_error("failed to update live ingress capture region", status);
    }
    log_live_ingress_message("info", "updated capture region to " + std::to_string(region.x) + "," +
                                         std::to_string(region.y) + "," + std::to_string(region.width) + "x" +
                                         std::to_string(region.height));
}

LiveCaptureRegion LiveVideoIngress::snapshot_capture_region() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (session_) {
        return to_live_region(session_->snapshot_capture_region());
    }
    return to_live_region(config_.initial_region);
}

frameshow::CaptureFormatInfo LiveVideoIngress::snapshot_format() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return session_ ? session_->snapshot_format() : frameshow::CaptureFormatInfo{};
}

std::string LiveVideoIngress::last_error() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return session_ ? session_->last_error() : std::string{};
}

const frameshow::CaptureConfig& LiveVideoIngress::config() const noexcept {
    return config_;
}

std::uint32_t LiveVideoIngress::max_inflight_sources() const noexcept {
    return config_.v4l2_buffer_count == 0U ? 1U : config_.v4l2_buffer_count;
}

std::uint64_t LiveVideoIngress::session_nonce() const noexcept {
    return session_nonce_.load(std::memory_order_acquire);
}

}  // namespace mmltk::live
