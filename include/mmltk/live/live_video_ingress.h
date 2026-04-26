#pragma once

#include "mmltk/live/live_types.h"

#include <frameshow/capture_session.hpp>
#include <frameshow/capture_types.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace mmltk::live {

class LiveVideoIngress {
   public:
    explicit LiveVideoIngress(frameshow::CaptureConfig config);
    ~LiveVideoIngress();

    LiveVideoIngress(const LiveVideoIngress&) = delete;
    LiveVideoIngress& operator=(const LiveVideoIngress&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool try_acquire_latest_source(SourceFrameView* out);
    [[nodiscard]] bool wait_acquire_latest_source(SourceFrameView* out, const std::atomic<bool>& stop_requested);
    void notify_source_waiters() noexcept;
    void release_source(std::uint32_t buffer_index);

    void set_capture_region(const LiveCaptureRegion& region);
    [[nodiscard]] LiveCaptureRegion snapshot_capture_region() const;
    [[nodiscard]] frameshow::CaptureFormatInfo snapshot_format() const;
    [[nodiscard]] std::string last_error() const;

    [[nodiscard]] const frameshow::CaptureConfig& config() const noexcept;
    [[nodiscard]] std::uint32_t max_inflight_sources() const noexcept;
    [[nodiscard]] std::uint64_t session_nonce() const noexcept;

   private:
    frameshow::CaptureConfig config_{};
    std::unique_ptr<frameshow::CaptureSession> session_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> session_nonce_{0};
    mutable std::mutex lifecycle_mutex_;
};

}  // namespace mmltk::live
