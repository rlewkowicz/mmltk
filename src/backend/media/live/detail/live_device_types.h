#pragma once
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include "live_slot_state.h"
namespace mmltk::backend::media::live {
namespace capture = mmltk::backend::media::capture;
namespace gpu = mmltk::frameworks::gpu;
namespace detail {
struct ActiveLiveCudaContext final {
    std::uintptr_t worker = 0U;
    void* owner = nullptr;
    int device = -1;
};
extern thread_local ActiveLiveCudaContext active_live_cuda_context;
}  // namespace detail
// Immutable projection of the one physical Live owner's command identity and
// CUDA device. Satellites borrow this value; they cannot select another
// device or become an independent failure authority.
class LivePhysicalCudaContext;
class LiveCudaCommandScope final {
   public:
    LiveCudaCommandScope(const LiveCudaCommandScope&) = delete;
    LiveCudaCommandScope& operator=(const LiveCudaCommandScope&) = delete;
    LiveCudaCommandScope(LiveCudaCommandScope&&) = delete;
    LiveCudaCommandScope& operator=(LiveCudaCommandScope&&) = delete;
    ~LiveCudaCommandScope() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] cudaError_t Record(cudaError_t status) noexcept;
    [[nodiscard]] cudaError_t Record(std::int32_t status) noexcept;

   private:
    explicit LiveCudaCommandScope(const LivePhysicalCudaContext& context) noexcept;
    const LivePhysicalCudaContext* context_ = nullptr;
    gpu::ResourceOwnerCommandScope command_{};
    gpu::CudaDeviceScope device_{gpu::CudaDeviceOwner{}};
    cudaError_t first_status_ = cudaSuccess;
    bool nested_ = false;
    bool owns_active_context_ = false;
    friend class LivePhysicalCudaContext;
};
class LivePhysicalCudaContext final {
   public:
    LivePhysicalCudaContext() noexcept = default;
    LivePhysicalCudaContext(gpu::ResourceOwnerCommandBinding command, gpu::CudaDeviceOwner device) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int device() const noexcept;
    [[nodiscard]] LiveCudaCommandScope scope() const noexcept;
    void Record(cudaError_t status) const noexcept;

   private:
    [[nodiscard]] gpu::ResourceOwnerCommandScope EnterCommand() const noexcept;
    gpu::ResourceOwnerCommandBinding command_{};
    gpu::CudaDeviceOwner device_{};
    friend class LiveCudaCommandScope;
};
[[nodiscard]] inline std::optional<SlotState> retire_live_slot(LiveCudaCommandScope& scope, std::atomic<std::uint32_t>& state,
                                                               const cudaStream_t stream) noexcept {
    bool synchronized = static_cast<bool>(scope);
    if (scope && stream != nullptr) synchronized = scope.Record(cudaStreamSynchronize(stream)) == cudaSuccess;
    const auto current = static_cast<SlotState>(state.load(std::memory_order_acquire));
    bool claimed = current == SlotState::Completing;
    if (!claimed && current != SlotState::Free && current != SlotState::Terminal) claimed = claim_live_slot(state, current);
    if (!claimed) return std::nullopt;
    return synchronized ? SlotState::Free : SlotState::Terminal;
}
template <class Slot, class Publish>
void retire_live_slots(LiveCudaCommandScope& scope, Slot* const slots, const std::uint32_t count, Publish&& publish) noexcept {
    for (std::uint32_t index = 0U; index < count; ++index) {
        Slot& slot = slots[index];
        if (const auto state = retire_live_slot(scope, slot.state, slot.stream)) std::forward<Publish>(publish)(slot, *state);
    }
}
[[nodiscard]] inline bool claim_acquired_live_slot(std::atomic<std::uint32_t>* const state, const LivePhysicalCudaContext& cuda) noexcept {
    if (state != nullptr && claim_live_slot(*state, SlotState::Acquired)) return true;
    cuda.Record(cudaErrorUnknown);
    return false;
}
void synchronize_live_cuda_stream(LiveCudaCommandScope& scope, cudaStream_t stream) noexcept;
void destroy_live_cuda_event(LiveCudaCommandScope& scope, cudaEvent_t& event) noexcept;
void destroy_live_cuda_stream(LiveCudaCommandScope& scope, cudaStream_t& stream) noexcept;
void free_live_cuda_allocation(LiveCudaCommandScope& scope, CUdeviceptr& allocation) noexcept;
struct DeviceFrameView;
struct DeviceFrameMetadata {
    LiveFrameId frame{};
    capture::CaptureSessionIdentity capture_identity{};
    std::uint32_t capture_slot = 0U;
    std::uint32_t pixel_format = 0U;
    LiveCaptureRegion region{};
    std::uint64_t captured_ns = 0U;
    std::uint64_t ready_ns = 0U;
    bool short_frame = false;
    [[nodiscard]] static DeviceFrameMetadata Captured(const capture::FilledCaptureSlotLease& lease) noexcept;
    [[nodiscard]] static DeviceFrameMetadata From(const DeviceFrameView& view) noexcept;
    [[nodiscard]] DeviceFrameView View(std::uint32_t slot, CUdeviceptr pixels, std::size_t pitch_bytes, cudaEvent_t ready, cudaStream_t stream) const noexcept;
};
struct DeviceFrameView final : DeviceFrameMetadata {
    std::uint32_t slot = 0U;
    CUdeviceptr pixels = 0U;
    std::size_t pitch_bytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    cudaEvent_t ready = nullptr;
    cudaStream_t stream = nullptr;
    [[nodiscard]] inline bool valid() const noexcept {
        return frame.valid() && capture_identity.valid() && pixels != 0U && pitch_bytes != 0U && width != 0U && height != 0U && ready != nullptr;
    }
};
struct DeviceFrameCopyTarget final {
    CUdeviceptr pixels = 0U;
    std::size_t pitch_bytes = 0U;
    cudaEvent_t ready = nullptr;
    cudaStream_t stream = nullptr;
};
[[nodiscard]] cudaError_t copy_device_frame(LiveCudaCommandScope&, const DeviceFrameView&, DeviceFrameCopyTarget) noexcept;
inline DeviceFrameMetadata DeviceFrameMetadata::Captured(const capture::FilledCaptureSlotLease& lease) noexcept {
    return {.frame = {lease.identity().session, lease.sequence()},
            .capture_identity = lease.identity(),
            .capture_slot = lease.slot(),
            .pixel_format = lease.pixel_format(),
            .region = lease.region(),
            .captured_ns = lease.capture_ns(),
            .short_frame = lease.short_frame()};
}
inline DeviceFrameMetadata DeviceFrameMetadata::From(const DeviceFrameView& view) noexcept { return static_cast<const DeviceFrameMetadata&>(view); }
inline DeviceFrameView DeviceFrameMetadata::View(const std::uint32_t slot, const CUdeviceptr pixels, const std::size_t pitch_bytes, const cudaEvent_t ready,
                                                 const cudaStream_t stream) const noexcept {
    DeviceFrameView view;
    static_cast<DeviceFrameMetadata&>(view) = *this;
    view.slot = slot;
    view.pixels = pixels;
    view.pitch_bytes = pitch_bytes;
    view.width = region.width;
    view.height = region.height;
    view.ready = ready;
    view.stream = stream;
    return view;
}
struct OverlayView final {
    std::uint32_t slot = 0U;
    LiveFrameId frame{};
    CUdeviceptr rgba = 0U;
    std::size_t pitch_bytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    cudaEvent_t ready = nullptr;
    cudaStream_t stream = nullptr;
    bool has_content = false;
};
}  // namespace mmltk::backend::media::live
