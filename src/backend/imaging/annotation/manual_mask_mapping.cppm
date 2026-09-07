module;
#include <cstddef>
#include <cstdint>

#include "detail/manual_mask_mapping_abi.h"

export module mmltk.backend.imaging.annotation.manual_mask_mapping;

export namespace mmltk::backend::imaging::annotation {

// One integer mapping vocabulary shared by overlay persistence and CUDA launch
// boundaries. The derived launch record adds only execution-specific fields.
using ManualMaskMapping = detail::ManualMaskMappingAbi;
using AnnotationLaunchStatus = std::int32_t;
inline constexpr AnnotationLaunchStatus kAnnotationLaunchSuccess = 0;

[[nodiscard]] AnnotationLaunchStatus draw_deferred_manual_mask_runs_rgba_pitched(
    const ManualMaskMapping& mapping, std::uint8_t* overlay_region, std::size_t pitch_bytes, int width, int height,
    const std::uint32_t* run_pairs, std::uint32_t run_count, std::uint32_t region_capture_x, std::uint32_t region_capture_y,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha, std::uintptr_t stream) noexcept;

}  // namespace mmltk::backend::imaging::annotation
