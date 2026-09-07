#pragma once
#include <cuda.h>
#include <cuda_runtime_api.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "live_device_types.h"
#include "live_state_signal.h"
#include "overlay_palette.h"

namespace mmltk::backend::media::live {

class LiveManualOverlayWorker final {
   public:
    LiveManualOverlayWorker(ManualOverlayDocument& document, std::uint32_t slots, std::uint32_t width, std::uint32_t height,
                            std::uint32_t maximum_instances, LiveManualOverlayUploadLimits upload_limits, LivePhysicalCudaContext cuda);
    ~LiveManualOverlayWorker();
    LiveManualOverlayWorker(const LiveManualOverlayWorker&) = delete;
    LiveManualOverlayWorker& operator=(const LiveManualOverlayWorker&) = delete;

    void start() noexcept;
    void close_admission() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool render_pending();
    [[nodiscard]] bool try_acquire_latest(OverlayView* output);
    void release(std::uint32_t slot) noexcept;
    void terminalize(std::uint32_t slot) noexcept;
    void set_ready_listener(std::function<void()> listener);

   private:
    enum class PrepareResult : std::uint8_t {
        Ready,
        Refused,
        CudaFailure,
    };

    struct UploadStorage final {
        std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> storage;
        void* host = nullptr;
        CUdeviceptr device = 0U;
    };

    struct PackedInstance final {
        std::size_t mask_offset = 0U;
        std::size_t run_value_offset = 0U;
        std::size_t polyline_value_offset = 0U;
        std::size_t point_value_offset = 0U;
        std::size_t edge_value_offset = 0U;
        std::optional<ManualOverlayDeferredMaskProjection> deferred_mask_projection;
    };

    struct Slot final {
        std::atomic<std::uint32_t> state{slot_state_value(SlotState::Free)};
        CUdeviceptr rgba = 0U;
        std::size_t pitch = 0U;
        cudaStream_t stream = nullptr;
        cudaEvent_t ready = nullptr;
        std::uint64_t generation = 0U;
        std::uint32_t index = 0U;
        bool has_content = false;
        UploadStorage masks;
        UploadStorage runs;
        UploadStorage points;
        UploadStorage edges;
        UploadStorage brush;
        std::unique_ptr<PackedInstance[]> packed;
        LiveManualOverlayWorker* owner = nullptr;
    };

    struct SlotReservation final {
        Slot* slot = nullptr;

        [[nodiscard]] inline explicit operator bool() const noexcept { return slot != nullptr; }
    };

    static void CUDART_CB RenderComplete(void* context) noexcept;
    static void ScrubProduct(Slot& slot) noexcept;
    void publish_slot(Slot& slot, SlotState published) noexcept;
    [[nodiscard]] SlotReservation reserve() noexcept;
    void allocate_upload(UploadStorage& storage, std::size_t bytes, LiveCudaCommandScope& scope);
    [[nodiscard]] PrepareResult prepare_uploads(const ManualOverlayDocumentSnapshot& snapshot, Slot& slot, std::size_t* instance_count);
    [[nodiscard]] bool render_snapshot(const ManualOverlayDocumentSnapshot& snapshot, Slot& slot, std::size_t instance_count);
    void release_upload(UploadStorage& storage) noexcept;
    void destroy() noexcept;

    ManualOverlayDocument& document_;
    LivePhysicalCudaContext cuda_{};
    std::unique_ptr<Slot[]> slots_;
    std::uint32_t slot_count_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::uint32_t maximum_instances_ = 0U;
    const LiveManualOverlayUploadLimits upload_limits_{};
    std::atomic<int> latest_{-1};
    std::atomic<std::uint64_t> published_generation_{0U};
    std::atomic<bool> running_{false};
    std::uint64_t consumed_generation_ = 0U;
    LiveStateSignal ready_;
};
}  // namespace mmltk::backend::media::live
