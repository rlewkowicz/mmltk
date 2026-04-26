#pragma once

#include "mmltk/live/live_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mmltk::live {

struct AnalyzerResult;
class FrameAnalyzer;
class LiveFrameFanout;

class LiveAnalyzerWorker {
   public:
    struct AnalyzerResultView {
        std::uint32_t slot_index = 0;
        const AnalyzerResult* result = nullptr;
    };

    struct AnalysisOverlayView {
        std::uint32_t slot_index = 0;
        LiveFrameId frame_id{};
        CUdeviceptr data = 0;
        std::size_t pitch_bytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        cudaEvent_t ready_event = nullptr;
        bool has_content = false;
    };

    struct Status {
        bool running = false;
        bool analyzer_attached = false;
        bool model_hot = false;
        std::uint64_t frames_analyzed = 0;
        std::uint64_t frames_skipped = 0;
        LiveFrameId last_completed_frame_id{};
        double last_latency_ms = 0.0;
        std::string backend_name;
        std::string last_error;
    };

    LiveAnalyzerWorker(LiveFrameFanout& fanout, int cuda_device_index, std::uint32_t max_capture_width,
                       std::uint32_t max_capture_height, std::uint32_t result_slot_count = 2,
                       std::uint32_t overlay_slot_count = 2);
    ~LiveAnalyzerWorker();

    LiveAnalyzerWorker(const LiveAnalyzerWorker&) = delete;
    LiveAnalyzerWorker& operator=(const LiveAnalyzerWorker&) = delete;

    void start();
    void stop();
    void set_analyzer(std::unique_ptr<FrameAnalyzer> analyzer);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool try_acquire_latest_result(AnalyzerResultView* out);
    void release_result(std::uint32_t slot_index);
    [[nodiscard]] bool try_acquire_latest_overlay(AnalysisOverlayView* out);
    void release_overlay(std::uint32_t slot_index);
    [[nodiscard]] Status snapshot_status() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::live
