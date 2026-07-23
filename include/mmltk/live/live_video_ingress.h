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

class LivePipelineTrace;

class LiveVideoIngress {
   public:
    explicit LiveVideoIngress(frameshow::CaptureConfig config, std::shared_ptr<LivePipelineTrace> pipeline_trace = {});
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
    void release_source(std::uint32_t buffer_index, const LiveFrameId& frame_id, std::uint64_t capture_ns);

    void set_capture_region(const LiveCaptureRegion& region);
    [[nodiscard]] LiveCaptureRegion snapshot_capture_region() const;
    [[nodiscard]] frameshow::CaptureFormatInfo snapshot_format() const;
    [[nodiscard]] frameshow::CaptureStats snapshot_stats() const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] std::string last_error_without_trace() const;

    [[nodiscard]] const frameshow::CaptureConfig& config() const noexcept;
    [[nodiscard]] std::uint32_t max_inflight_sources() const noexcept;
    [[nodiscard]] std::uint64_t session_nonce() const noexcept;

   private:
    [[nodiscard]] bool publish_acquired_source(const frameshow::Status& status,
                                               const frameshow::InferenceFrameView& frame, SourceFrameView* out);

    frameshow::CaptureConfig config_{};
    std::unique_ptr<frameshow::CaptureSession> session_;
    std::shared_ptr<LivePipelineTrace> pipeline_trace_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> session_nonce_{0};
    mutable std::mutex lifecycle_mutex_;

    [[nodiscard]] std::string last_error_impl(bool record_status_probe) const;
};

}  
