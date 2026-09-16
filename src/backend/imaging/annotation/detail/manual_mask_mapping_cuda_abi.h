#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include "manual_mask_mapping_abi.h"
namespace mmltk::backend::imaging::annotation::detail {
[[nodiscard]] __host__ __device__ inline std::uint32_t project_crop_coordinate(const std::uint32_t output_coordinate, const std::uint32_t crop_extent,
                                                                               const std::uint32_t output_extent) noexcept {
    if (output_extent == 0U) { return 0U; }
    const std::uint32_t extent_quotient = crop_extent / output_extent;
    const std::uint32_t extent_remainder = crop_extent % output_extent;
    const std::uint64_t quotient_projection = static_cast<std::uint64_t>(output_coordinate) * extent_quotient;
    // The unreduced numerator is bounded by UINT32_MAX squared plus half
    // UINT32_MAX, which is still below UINT64_MAX. Splitting crop_extent into
    // its quotient and remainder preserves that exact center-sampled result.
    const std::uint64_t remainder_projection = static_cast<std::uint64_t>(output_coordinate) * extent_remainder + crop_extent / 2U;
    return static_cast<std::uint32_t>(quotient_projection + remainder_projection / output_extent);
}
struct MutableRgbaSurfaceAbi final {
    std::uint8_t* pixels = nullptr;
    std::size_t pitch_bytes = 0U;
    int width = 0;
    int height = 0;
};
struct RgbaColorAbi final {
    std::uint8_t red = 0U;
    std::uint8_t green = 0U;
    std::uint8_t blue = 0U;
    std::uint8_t alpha = 0U;
};
struct DeferredManualMaskRunsLaunchAbi final {
    ManualMaskMappingAbi mapping;
    MutableRgbaSurfaceAbi overlay_region;
    const std::uint32_t* run_pairs = nullptr;
    std::uint32_t run_count = 0U;
    std::uint32_t region_capture_x = 0U;
    std::uint32_t region_capture_y = 0U;
    RgbaColorAbi color;
    cudaStream_t stream = nullptr;
};
[[nodiscard]] cudaError_t launch_deferred_manual_mask_runs_cuda(const DeferredManualMaskRunsLaunchAbi& launch) noexcept;
}  // namespace mmltk::backend::imaging::annotation::detail
