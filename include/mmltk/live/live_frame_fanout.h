#pragma once

#include "mmltk/live/live_pipeline_status.h"
#include "mmltk/live/live_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {

class LiveVideoIngress;
class UiCropState;

class LiveFrameFanout {
   public:
    LiveFrameFanout(LiveVideoIngress& ingress, const UiCropState& ui_crop_state, std::uint32_t detect_slot_count,
                    std::uint32_t output_slot_count, std::shared_ptr<LivePipelineTrace> pipeline_trace = {});
    ~LiveFrameFanout();

    LiveFrameFanout(const LiveFrameFanout&) = delete;
    LiveFrameFanout& operator=(const LiveFrameFanout&) = delete;

    struct Status : LivePipelineStatus {
        bool running = false;
        std::uint64_t frames_fanned_out = 0;
        std::uint64_t skipped_detect_publishes = 0;
        std::uint64_t skipped_output_publishes = 0;
        std::string last_error;
    };

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] Status snapshot_status() const;
    [[nodiscard]] bool try_acquire_detect(DetectBundle* out);
    [[nodiscard]] bool wait_acquire_detect(DetectBundle* out, const std::atomic<bool>& stop_requested);
    void release_detect(std::uint32_t slot_index);
    [[nodiscard]] bool try_acquire_output(OutputBundle* out);
    [[nodiscard]] bool wait_acquire_output(OutputBundle* out, const std::atomic<bool>& stop_requested);
    void release_output(std::uint32_t slot_index);
    void notify_detect_waiters() noexcept;
    void notify_output_waiters() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::live
