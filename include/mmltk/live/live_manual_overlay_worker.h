#pragma once

#include "mmltk/live/live_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {

class ManualOverlayDocument;

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

    LiveManualOverlayWorker(ManualOverlayDocument& document, int cuda_device_index, std::uint32_t max_capture_width,
                            std::uint32_t max_capture_height, std::uint32_t overlay_slot_count = 2);
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::live
