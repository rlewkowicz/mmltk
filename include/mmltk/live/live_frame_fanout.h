#pragma once

#include "live/live_helpers.h"
#include "mmltk/live/live_worker_runtime.h"
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

template <typename Dimensions, typename Bundle>
struct LiveFrameFanoutSlot {
    using BundleType = Bundle;

    std::uint32_t slot_index = 0;
    BgrPitchedDeviceBuffer device_buffer;
    CudaStreamHandle stream;
    CudaEventHandle ready_event;
    std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
    LiveFrameId frame_id{};
    LiveCaptureRegion region{};
    std::uint64_t capture_ns = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;

    [[nodiscard]] BundleType make_view() const noexcept {
        return BundleType{
            slot_index,
            frame_id,
            device_buffer.data(),
            Dimensions{region.width, region.height, device_buffer.pitch_bytes()},
            ready_event.get(),
            stream.get(),
            region,
            capture_ns,
            ready_ns,
            short_frame,
        };
    }

    void reset_for_reuse() noexcept {}

    [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
        return ready_event.get();
    }

    void publish_from_source(const SourceFrameView& source,
                             const LiveCaptureRegion& published_region,
                             const std::uint64_t published_ready_ns) noexcept {
        frame_id = source.frame_id;
        region = published_region;
        capture_ns = source.capture_ns;
        ready_ns = published_ready_ns;
        short_frame = source.short_frame;
        state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    }
};

template <typename Slot>
void publish_live_frame_slot(Slot& slot,
                             std::atomic<int>& latest_index,
                             const SourceFrameView& source,
                             const LiveCaptureRegion& published_region,
                             const std::uint64_t published_ready_ns) noexcept {
    slot.publish_from_source(source, published_region, published_ready_ns);
    latest_index.store(static_cast<int>(slot.slot_index), std::memory_order_release);
}

template <typename Slot>
[[nodiscard]] Slot* reserve_live_frame_slot(std::vector<std::unique_ptr<Slot>>& slots,
                                            std::atomic<int>& latest_index) {
    return worker_runtime::reserve_writable_slot_view(slots,
                                                      latest_index,
                                                      "cudaEventQuery for live fanout slot reuse");
}

template <typename Slot>
[[nodiscard]] bool try_acquire_live_frame_slot(std::vector<std::unique_ptr<Slot>>& slots,
                                               const std::atomic<int>& latest_index,
                                               typename Slot::BundleType* out) {
    return worker_runtime::try_acquire_latest_published_slot_view(slots, latest_index, out);
}

template <typename Slot>
void release_live_frame_slot(Slot& slot, const char* context) {
    release_acquired_slot(slot, context);
}

template <typename Slot>
void allocate_live_frame_slots(std::vector<std::unique_ptr<Slot>>& slots,
                               const std::uint32_t slot_count,
                               const std::uint32_t max_capture_width,
                               const std::uint32_t max_capture_height,
                               const char* buffer_context,
                               const char* stream_context,
                               const char* event_context) {
    worker_runtime::allocate_pitched_device_slots(
        slots,
        slot_count,
        max_capture_width,
        max_capture_height,
        buffer_context,
        stream_context,
        event_context);
}

class LiveFrameFanout {
public:
    LiveFrameFanout(LiveVideoIngress& ingress,
                    const UiCropState& ui_crop_state,
                    std::uint32_t detect_slot_count,
                    std::uint32_t output_slot_count);
    ~LiveFrameFanout();

    LiveFrameFanout(const LiveFrameFanout&) = delete;
    LiveFrameFanout& operator=(const LiveFrameFanout&) = delete;

    struct Status {
        bool running = false;
        std::string last_error;
    };

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] Status snapshot_status() const;
    [[nodiscard]] bool try_acquire_detect(DetectBundle* out);
    void release_detect(std::uint32_t slot_index);
    [[nodiscard]] bool try_acquire_output(OutputBundle* out);
    void release_output(std::uint32_t slot_index);

private:
    using DetectSlot = LiveFrameFanoutSlot<DetectDimensions, DetectBundle>;
    using OutputSlot = LiveFrameFanoutSlot<OutputDimensions, OutputBundle>;

    struct ReleaseEvent {
        CudaEventHandle event;
        bool in_use = false;
    };

    struct PendingSourceRelease {
        std::uint32_t buffer_index = 0;
        std::size_t event_index = 0;
    };

    void allocate_resources(std::uint32_t detect_slot_count,
                            std::uint32_t output_slot_count);
    void destroy_resources() noexcept;
    void fanout_thread_main();
    void drain_pending_source_releases(bool wait);
    void publish_error(std::string error_message);

    [[nodiscard]] DetectSlot* reserve_detect_slot();
    [[nodiscard]] OutputSlot* reserve_output_slot();
    [[nodiscard]] std::size_t acquire_release_event_index();
    [[nodiscard]] LiveCaptureRegion resolve_detect_region(const SourceFrameView& source) const;

    LiveVideoIngress& ingress_;
    const UiCropState& ui_crop_state_;
    RuntimeCropState runtime_crop_{};
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::shared_ptr<const Status> status_{std::make_shared<Status>()};

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
