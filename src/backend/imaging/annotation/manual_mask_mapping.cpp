#include "src/backend/imaging/annotation/manual_mask_mapping.h"
#include <cstddef>
#include <cstdint>
#include "detail/manual_mask_mapping_cuda_abi.h"
namespace {
using ManualMaskMapping = mmltk::backend::imaging::annotation::ManualMaskMapping;
}  // namespace
namespace mmltk::backend::imaging::annotation {
AnnotationLaunchStatus draw_deferred_manual_mask_runs_rgba_pitched(const ManualMaskMapping& mapping, std::uint8_t* overlay_region,
                                                                   const std::size_t pitch_bytes, const int width, const int height,
                                                                   const std::uint32_t* run_pairs, const std::uint32_t run_count,
                                                                   const std::uint32_t region_capture_x, const std::uint32_t region_capture_y,
                                                                   const std::uint8_t red, const std::uint8_t green, const std::uint8_t blue,
                                                                   const std::uint8_t alpha, const std::uintptr_t stream) noexcept {
    return static_cast<AnnotationLaunchStatus>(detail::launch_deferred_manual_mask_runs_cuda(detail::DeferredManualMaskRunsLaunchAbi{
        mapping,
        {overlay_region, pitch_bytes, width, height},
        run_pairs,
        run_count,
        region_capture_x,
        region_capture_y,
        {red, green, blue, alpha},
        reinterpret_cast<cudaStream_t>(stream),
    }));
}
}  // namespace mmltk::backend::imaging::annotation
