#pragma once
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "analysis_frame.h"
#include "live_frame_fanout.h"
namespace mmltk::backend::media::live {
class LiveAnalyzerWorker final {
   public:
    struct Status final {
        bool running = false;
        bool provider_attached = false;
        std::uint64_t completed = 0U;
        std::uint64_t refused = 0U;
    };
    LiveAnalyzerWorker(LiveFrameFanout& fanout, std::uint32_t slot_count, std::uint32_t maximum_regions, std::uint32_t width, std::uint32_t height,
                       LivePhysicalCudaContext cuda);
    ~LiveAnalyzerWorker();
    LiveAnalyzerWorker(const LiveAnalyzerWorker&) = delete;
    LiveAnalyzerWorker& operator=(const LiveAnalyzerWorker&) = delete;
    void set_provider(std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> provider);
    void start();
    void close_admission() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool process_latest();
    [[nodiscard]] bool try_acquire(LiveFrameId frame, LiveAnalysisOverlayProjection* output);
    [[nodiscard]] bool discard(LiveFrameId frame);
    [[nodiscard]] bool drain_releases();
    void release(std::uint32_t slot) noexcept;
    void terminalize(std::uint32_t slot) noexcept;
    void set_ready_listener(std::function<void()> listener);
    [[nodiscard]] Status status() const;

   private:
    struct AnalysisSlot final {
        std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
        std::unique_ptr<mmltk::backend::ml::runtime::AnalysisAnnotationStorage[]> annotations;
        std::unique_ptr<CUdeviceptr[]> allocations;
        std::optional<LiveAnalysisFrame> frame;
        cudaStream_t settlement_stream = nullptr;
        std::atomic<bool> release_ready{false};
        LiveAnalyzerWorker* owner = nullptr;
        std::uint32_t index = 0U;
    };
    static void CUDART_CB ReadyToRelease(void* context) noexcept;
    static void ScrubProduct(AnalysisSlot& slot) noexcept;
    void publish_slot(AnalysisSlot& slot, SlotState published) noexcept;
    [[nodiscard]] AnalysisSlot* reserve() noexcept;
    [[nodiscard]] bool schedule_release(AnalysisSlot& slot) noexcept;
    void release_slot(AnalysisSlot& slot) noexcept;
    void release_storage() noexcept;
    LiveFrameFanout& fanout_;
    LivePhysicalCudaContext cuda_{};
    std::unique_ptr<AnalysisSlot[]> slots_;
    std::uint32_t slot_count_ = 0U;
    std::uint32_t maximum_regions_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider> provider_;
    std::atomic<int> latest_{-1};
    std::atomic<bool> running_{false};
    mutable std::mutex status_mutex_;
    Status status_{};
    LiveStateSignal ready_;
};
}  // namespace mmltk::backend::media::live
