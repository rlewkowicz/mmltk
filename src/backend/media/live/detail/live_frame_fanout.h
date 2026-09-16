#pragma once
#include "live_device_types.h"
#include "live_slot_state.h"
#include "live_state_signal.h"
#include "src/backend/media/live/live_frame_id.h"
#include "src/backend/media/capture/capture_types.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include "live_video_ingress.h"
namespace mmltk::backend::media::live {
struct LiveRawFrameReadbackWork final {
    LiveFrameId frame{};
    std::size_t destination_bytes = 0U;
    [[nodiscard]] inline bool valid() const noexcept { return frame.valid() && destination_bytes != 0U; }
};
struct LiveRawFrameReadbackResult final {
    LiveFrameId frame{};
    const std::uint8_t* pixels = nullptr;
    std::size_t bytes = 0U;
    bool completed = false;
    [[nodiscard]] inline bool valid() const noexcept {
        return frame.valid() && ((completed && pixels != nullptr && bytes != 0U) || (!completed && pixels == nullptr && bytes == 0U));
    }
};
class LiveRawFrameCache final {
   public:
    LiveRawFrameCache(std::uint32_t slots, std::uint32_t width, std::uint32_t height, LivePhysicalCudaContext cuda);
    ~LiveRawFrameCache();
    LiveRawFrameCache(const LiveRawFrameCache&) = delete;
    LiveRawFrameCache& operator=(const LiveRawFrameCache&) = delete;
    [[nodiscard]] cudaEvent_t store(const DeviceFrameView& source);
    [[nodiscard]] bool begin_readback(LiveRawFrameReadbackWork work);
    [[nodiscard]] std::optional<LiveRawFrameReadbackResult> take_readback_result() noexcept;
    [[nodiscard]] std::optional<LiveRawFrameReadbackResult> settle_readback() noexcept;
    void set_ready_listener(std::function<void()> listener);
    void clear() noexcept;

   private:
    struct Slot final {
        std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
        CUdeviceptr pixels = 0U;
        std::size_t pitch = 0U;
        cudaEvent_t ready = nullptr;
        LiveFrameId frame{};
        capture::CaptureRegion region{};
        std::uint32_t index = 0U;
    };
    static void CUDART_CB ReadbackComplete(void* context) noexcept;
    static void ScrubProduct(Slot& slot) noexcept;
    void publish_slot(Slot& slot, SlotState published) noexcept;
    [[nodiscard]] Slot* reserve() noexcept;
    [[nodiscard]] std::optional<LiveRawFrameReadbackResult> finish_readback(bool completed) noexcept;
    void destroy() noexcept;
    LivePhysicalCudaContext cuda_{};
    std::unique_ptr<Slot[]> slots_;
    std::uint32_t slot_count_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> pinned_storage_;
    std::uint8_t* pinned_ = nullptr;
    std::size_t pinned_bytes_ = 0U;
    cudaStream_t store_stream_ = nullptr;
    cudaStream_t readback_stream_ = nullptr;
    LiveRawFrameReadbackWork readback_work_{};
    std::uint32_t readback_slot_ = 0U;
    std::size_t readback_bytes_ = 0U;
    std::atomic<bool> readback_complete_{false};
    LiveStateSignal ready_;
};
}  // namespace mmltk::backend::media::live
namespace mmltk::backend::media::live {
class LiveFrameFanout final {
   public:
    struct Status final {
        bool running = false;
        std::uint64_t frames_fanned_out = 0U;
        std::uint64_t arrivals_dropped = 0U;
        std::uint64_t unstarted_replaced = 0U;
    };
    // CLEANUP-IGNORE: LiveFrameFanout owns frame-distribution slots rather than compositor or overlay-worker resources.
    LiveFrameFanout(LiveVideoIngress& ingress, std::uint32_t slot_count, std::uint32_t width, std::uint32_t height, LivePhysicalCudaContext cuda);
    ~LiveFrameFanout();
    LiveFrameFanout(const LiveFrameFanout&) = delete;
    LiveFrameFanout& operator=(const LiveFrameFanout&) = delete;
    void start() noexcept;
    void close_admission() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool process_latest();
    [[nodiscard]] bool try_acquire_analysis(DeviceFrameView* output);
    [[nodiscard]] bool try_acquire_composite(DeviceFrameView* output);
    void release_analysis(std::uint32_t slot, cudaEvent_t completion, cudaStream_t stream);
    void release_composite(std::uint32_t slot, cudaEvent_t completion, cudaStream_t stream);
    [[nodiscard]] bool begin_raw_readback(LiveRawFrameReadbackWork work);
    [[nodiscard]] std::optional<LiveRawFrameReadbackResult> take_raw_readback_result() noexcept;
    [[nodiscard]] std::optional<LiveRawFrameReadbackResult> settle_raw_readback() noexcept;
    void set_raw_readback_listener(std::function<void()> listener);
    void set_ready_listener(std::function<void()> listener);
    [[nodiscard]] Status status() const noexcept;

   private:
    struct FanoutSlot final {
        std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
        CUdeviceptr pixels = 0U;
        std::size_t pitch = 0U;
        cudaStream_t stream = nullptr;
        cudaEvent_t ready = nullptr;
        DeviceFrameMetadata metadata{};
        std::uint32_t index = 0U;
    };
    struct SourceRelease final {
        LiveFrameFanout* owner = nullptr;
        std::uint32_t ingress_slot = 0U;
        cudaStream_t stream = nullptr;
    };
    static void CUDART_CB ReleaseSource(void* context) noexcept;
    static void ScrubProduct(FanoutSlot& slot) noexcept;
    void publish_slot(FanoutSlot* slots, FanoutSlot& slot, SlotState published) noexcept;
    void settle_slots(FanoutSlot* slots) noexcept;
    [[nodiscard]] FanoutSlot* reserve(FanoutSlot* slots, std::atomic<int>& latest) noexcept;
    [[nodiscard]] bool acquire(FanoutSlot* slots, std::atomic<int>& latest, DeviceFrameView* output) noexcept;
    void release(FanoutSlot* slots, std::uint32_t slot, cudaEvent_t completion, cudaStream_t stream);
    [[nodiscard]] bool settle_source_reads(FanoutSlot* analysis, FanoutSlot* composite, cudaEvent_t raw_ready) noexcept;
    void destroy() noexcept;
    LiveVideoIngress& ingress_;
    LivePhysicalCudaContext cuda_{};
    LiveRawFrameCache raw_cache_;
    std::unique_ptr<FanoutSlot[]> analysis_;
    std::unique_ptr<FanoutSlot[]> composite_;
    std::unique_ptr<SourceRelease[]> releases_;
    std::uint32_t slot_count_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::atomic<int> latest_analysis_{-1};
    std::atomic<int> latest_composite_{-1};
    cudaStream_t release_stream_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> fanned_{0U};
    std::atomic<std::uint64_t> dropped_{0U};
    std::atomic<std::uint64_t> replaced_{0U};
    LiveStateSignal ready_;
};
}  // namespace mmltk::backend::media::live
