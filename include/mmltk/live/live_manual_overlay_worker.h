#pragma once

#include "live/live_helpers.h"
#include "mmltk/live/live_types.h"
#include "mmltk/live/manual_overlay_document.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mmltk::live {

class LiveManualOverlayWorker {
public:
    struct OverlayView {
        std::uint32_t slot_index = 0;
        CUdeviceptr data = 0;
        std::size_t pitch_bytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        cudaEvent_t ready_event = nullptr;
        bool has_content = false;
        std::uint64_t generation = 0;
    };

    struct Status {
        bool running = false;
        std::uint64_t generations_rendered = 0;
        std::uint64_t last_generation = 0;
        std::string last_error;
    };

    LiveManualOverlayWorker(ManualOverlayDocument& document,
                            int cuda_device_index,
                            std::uint32_t max_capture_width,
                            std::uint32_t max_capture_height,
                            std::uint32_t overlay_slot_count = 2);
    ~LiveManualOverlayWorker();

    LiveManualOverlayWorker(const LiveManualOverlayWorker&) = delete;
    LiveManualOverlayWorker& operator=(const LiveManualOverlayWorker&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool try_acquire_latest_overlay(OverlayView* out);
    void release_overlay(std::uint32_t slot_index);
    [[nodiscard]] Status snapshot_status() const;

private:
    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        RgbaPitchedDeviceBuffer device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        std::uint64_t generation = 0;
        bool has_content = false;

        [[nodiscard]] OverlayView make_view() const noexcept {
            return OverlayView{
                slot_index,
                device_buffer.data(),
                device_buffer.pitch_bytes(),
                device_buffer.width(),
                device_buffer.height(),
                ready_event.get(),
                has_content,
                generation,
            };
        }

        void reset_for_reuse() noexcept {
            generation = 0;
            has_content = false;
        }

        [[nodiscard]] cudaEvent_t ready_event_handle() const noexcept {
            return ready_event.get();
        }
    };

    void allocate_resources();
    void destroy_resources() noexcept;
    void worker_thread_main();
    void render_snapshot(const ManualOverlayDocumentSnapshot& snapshot);
    [[nodiscard]] OverlaySlot* reserve_overlay_slot();
    void publish_error(std::string error_message);

    ManualOverlayDocument& document_;
    int cuda_device_index_ = 0;
    std::uint32_t max_capture_width_ = 0;
    std::uint32_t max_capture_height_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::shared_ptr<const Status> status_{std::make_shared<Status>()};
    std::shared_ptr<const std::string> last_error_{std::make_shared<std::string>()};
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots_;
    std::atomic<int> latest_overlay_index_{-1};
    PinnedUploadBuffer<std::uint8_t> host_mask_upload_;
    DeviceUploadBuffer<std::uint8_t> device_mask_upload_;
    PinnedUploadBuffer<int> host_points_upload_;
    DeviceUploadBuffer<int> device_points_upload_;
    PinnedUploadBuffer<std::uint32_t> host_edges_upload_;
    DeviceUploadBuffer<std::uint32_t> device_edges_upload_;
};

} // namespace mmltk::live
