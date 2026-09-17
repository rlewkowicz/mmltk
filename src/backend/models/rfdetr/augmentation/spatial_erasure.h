#pragma once
#include "src/common/math/deterministic_sampling.h"
#include <cuda_runtime.h>
#include <cmath>
#include <cstdint>
#include "gpu_augment_types.h"
#include "src/backend/imaging/sampling.h"
namespace mmltk::backend::models::rfdetr::augment_math {
inline constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
[[nodiscard]] __host__ __device__ __forceinline__ float uniform01(const std::uint64_t key, const std::uint64_t counter) {
    return static_cast<float>((mmltk::common::math::deterministic_mix64(key + counter * kGoldenRatio) >> 40U) & 0xFFFFFFULL) * (1.0F / 16777216.0F);
}
// Integer output coordinates identify the same pixel for image, preview and sparse mask samples.
[[nodiscard]] __host__ __device__ __forceinline__ bool erases_pixel(const AugmentationSpatialErasure& erasure, const std::int64_t x, const std::int64_t y,
                                                                    const std::int64_t width, const std::int64_t height) {
    if (erasure.dropout_probability > 0.0F &&
        uniform01(erasure.key, static_cast<std::uint64_t>(y * width + x) * 32ULL + 0x100DULL) < erasure.dropout_probability) {
        return true;
    }
    if (erasure.rectangular == 0U) { return false; }
    const float center_x = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
    const float center_y = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
    return center_x >= erasure.x0 && center_x < erasure.x1 && center_y >= erasure.y0 && center_y < erasure.y1;
}
[[nodiscard]] __host__ __device__ __forceinline__ bool erases_sample(const AugmentationSpatialErasure& erasure, const float x, const float y,
                                                                     const std::int64_t width, const std::int64_t height) {
    if (erasure.dropout_probability <= 0.0F && erasure.rectangular == 0U) { return false; }
    const auto pixel_x = mmltk::backend::imaging::sampling::support_pixel_index(x, width);
    const auto pixel_y = mmltk::backend::imaging::sampling::support_pixel_index(y, height);
    return erases_pixel(erasure, pixel_x, pixel_y, width, height);
}
}  // namespace mmltk::backend::models::rfdetr::augment_math
