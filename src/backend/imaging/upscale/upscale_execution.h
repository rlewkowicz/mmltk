#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <exception>
#include "src/frameworks/gpu/image_failure.h"

namespace mmltk::backend::imaging::upscale {

enum class ImageUpscalerOutcome : std::uint8_t { Completed, Cancelled };
// Invocation-scoped view: neither the runtime nor a queued request owns it.
using ImageUpscalerCurrent = std::function_ref<bool()>;
inline bool image_upscaler_current() noexcept { return true; }
enum class ImageUpscalerKind : std::uint8_t { ShiftLUT, RealPLKSR, Count };
enum class ImageUpscalerBackend : std::uint8_t { NisFallback, OnnxRuntime, TensorRt, Count };
static_assert(static_cast<std::size_t>(ImageUpscalerKind::Count) == 2U);
struct ImageUpscalerRequest {
    const float* device_pixels = nullptr;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t crop_x = 0U;
    std::uint32_t crop_y = 0U;
    std::uint32_t crop_width = 0U;
    std::uint32_t crop_height = 0U;
    ImageUpscalerCurrent current{image_upscaler_current};
};
struct ImageUpscalerUnsettledFailure final {
    std::exception_ptr failure{};
    std::exception_ptr settlement{};
};
class ImageUpscalerInitializationFailure final : public mmltk::frameworks::gpu::ImageFailure {
   public:
    using ImageFailure::ImageFailure;
};

enum class ImageUpscalerExecutionStage : std::uint8_t {
    StreamCreated, EventCreated, ContextCreated, BuffersAllocated,
    WarmInputSubmitted, WarmSubmitted, WarmSettled, CaptureBegan, CaptureSubmitted, CaptureEnded,
    GraphInstantiated, ReplaySubmitted, ReplaySettled, GraphExecutableDestroyed,
    GraphDestroyed, EventDestroyed, StreamDestroyed, BufferReleased, ContextReleased,
    InitializationAdmitted, ChecksumAdmitted, CacheLockAdmitted, CacheLockWaiting,
    BuildAdmitted, BasicAllocationAdmitted, BasicLaunchAdmitted, PreprocessAdmitted,
    RestoredAllocationAdmitted, TilePrepared, BindingsReady, RuntimeEnqueued, Count,
};
// Effect-only, per-owner instrumentation. Empty in ordinary execution; failures
// enter the same checked settlement path as failures of the preceding operation.
using ImageUpscalerExecutionCheckpoint = std::function<void(ImageUpscalerExecutionStage)>;
[[nodiscard]] inline bool image_upscaler_admitted(const ImageUpscalerExecutionCheckpoint& checkpoint,
    ImageUpscalerExecutionStage stage, ImageUpscalerCurrent current) {
    if (checkpoint) checkpoint(stage);
    return current();
}

}  // namespace mmltk::backend::imaging::upscale
