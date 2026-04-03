#pragma once

#include "live/live_helpers.h"
#include "mmltk/live/frame_analyzer.h"
#include "mmltk/live/live_frame_fanout.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mmltk::live {

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

    LiveAnalyzerWorker(LiveFrameFanout& fanout,
                       int cuda_device_index,
                       std::uint32_t max_capture_width,
                       std::uint32_t max_capture_height,
                       std::uint32_t result_slot_count = 2,
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
    struct ResultSlot {
        std::uint32_t slot_index = 0;
        AnalyzerResult result{};
        std::vector<torch::Tensor> retained_tensors;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
    };

    struct OverlaySlot {
        std::uint32_t slot_index = 0;
        PitchedDeviceBuffer<> device_buffer;
        CudaStreamHandle stream;
        CudaEventHandle ready_event;
        std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
        LiveFrameId frame_id{};
        bool has_content = false;
    };

    void allocate_resources();
    void destroy_resources() noexcept;
    void worker_thread_main();
    [[nodiscard]] ResultSlot* reserve_result_slot();
    [[nodiscard]] OverlaySlot* reserve_overlay_slot();
    [[nodiscard]] bool try_acquire_result_slot(ResultSlot& slot, AnalyzerResultView* out);
    [[nodiscard]] bool try_acquire_overlay_slot(OverlaySlot& slot, AnalysisOverlayView* out);
    void publish_error(std::string error_message);

    LiveFrameFanout& fanout_;
    int cuda_device_index_ = 0;
    std::uint32_t max_capture_width_ = 0;
    std::uint32_t max_capture_height_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::shared_ptr<FrameAnalyzer> analyzer_{};
    std::shared_ptr<const Status> status_{std::make_shared<Status>()};
    std::shared_ptr<const std::string> last_error_{std::make_shared<std::string>()};
    std::vector<std::unique_ptr<ResultSlot>> result_slots_;
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots_;
    std::atomic<int> latest_result_index_{-1};
    std::atomic<int> latest_overlay_index_{-1};
};

} // namespace mmltk::live
