#pragma once

#include "live/live_helpers.h"
#include "mmltk/live/crop_state.h"
#include "mmltk/live/live_types.h"
#include "mmltk/live/live_video_ingress.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mmltk::live {

class LiveFrameFanout {
public:
    LiveFrameFanout(LiveVideoIngress& ingress,
                    const UiCropState& ui_crop_state,
                    std::uint32_t detect_slot_count,
                    std::uint32_t output_slot_count);
    ~LiveFrameFanout();

    LiveFrameFanout(const LiveFrameFanout&) = delete;
    LiveFrameFanout& operator=(const LiveFrameFanout&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] bool try_acquire_detect(DetectBundle* out);
    void release_detect(std::uint32_t slot_index);
    [[nodiscard]] bool try_acquire_output(OutputBundle* out);
    void release_output(std::uint32_t slot_index);

private:
    struct DetectSlot {
        std::uint32_t slot_index = 0;
        PitchedDeviceBuffer<> device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        std::uint64_t capture_ns = 0;
        std::uint64_t ready_ns = 0;
        bool short_frame = false;
    };

    struct OutputSlot {
        std::uint32_t slot_index = 0;
        PitchedDeviceBuffer<> device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        std::uint64_t capture_ns = 0;
        std::uint64_t ready_ns = 0;
        bool short_frame = false;
    };

    struct ReleaseEvent {
        CudaEventHandle event;
        bool in_use = false;
    };

    struct PendingSourceRelease {
        std::uint32_t buffer_index = 0;
        std::size_t event_index = 0;
    };

    void allocate_resources();
    void destroy_resources() noexcept;
    void fanout_thread_main();
    void drain_pending_source_releases(bool wait);
    void publish_error(std::string error_message);

    [[nodiscard]] DetectSlot* reserve_detect_slot();
    [[nodiscard]] OutputSlot* reserve_output_slot();
    [[nodiscard]] std::size_t acquire_release_event_index();
    [[nodiscard]] LiveCaptureRegion resolve_detect_region(const SourceFrameView& source) const;

    [[nodiscard]] bool try_acquire_detect_slot(DetectSlot& slot, DetectBundle* out);
    [[nodiscard]] bool try_acquire_output_slot(OutputSlot& slot, OutputBundle* out);

    LiveVideoIngress& ingress_;
    const UiCropState& ui_crop_state_;
    RuntimeCropState runtime_crop_{};
    std::thread thread_;
    std::uint32_t detect_slot_count_ = 0;
    std::uint32_t output_slot_count_ = 0;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::shared_ptr<const std::string> last_error_{std::make_shared<std::string>()};

    std::vector<std::unique_ptr<DetectSlot>> detect_slots_;
    std::vector<std::unique_ptr<OutputSlot>> output_slots_;
    std::atomic<int> latest_detect_index_{-1};
    std::atomic<int> latest_output_index_{-1};

    CudaStreamHandle release_stream_;
    std::vector<ReleaseEvent> release_events_;
    std::vector<PendingSourceRelease> pending_source_releases_;

    std::uint32_t max_capture_width_ = 0;
    std::uint32_t max_capture_height_ = 0;
};

} // namespace mmltk::live
