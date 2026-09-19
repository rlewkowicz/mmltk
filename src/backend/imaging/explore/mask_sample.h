#pragma once
#include <cuda_runtime.h>
#include <cmath>
#include "detail/explore_render_cuda_abi.h"
#include "src/backend/imaging/sampling.h"
namespace mmltk::backend::imaging::explore::detail {
[[nodiscard]] __host__ __device__ inline bool sample_annotation_mask(const ExploreRenderRlePairAbi* pairs,
                                                                     const ExploreRenderAnnotationDescriptorAbi& annotation, const std::uint32_t capacity,
                                                                     const std::uint32_t width, const std::uint32_t height, const float x,
                                                                     const float y) noexcept {
    if (!annotation.mask_present || !pairs || width == 0U || height == 0U || annotation.rle_offset > capacity || annotation.rle_count > capacity - annotation.rle_offset ||
        annotation.rle_count == 0U || x < annotation.mask_bounds[0] || x > annotation.mask_bounds[2] || y < annotation.mask_bounds[1] || y > annotation.mask_bounds[3])
        return false;
    const float sx = annotation.inverse[0] * x + annotation.inverse[1] * y + annotation.inverse[2];
    const float sy = annotation.inverse[3] * x + annotation.inverse[4] * y + annotation.inverse[5];
    if (sx < 0.0F || sx > 1.0F || sy < 0.0F || sy > 1.0F) return false;
    namespace sampling = mmltk::backend::imaging::sampling;
    const auto pixel = sampling::support_pixel_index(sy, height) * width + sampling::support_pixel_index(sx, width);
    return sampling::rle_support_contains(pairs + annotation.rle_offset, annotation.rle_count, pixel);
}
[[nodiscard]] __host__ __device__ inline bool sample_annotation_support(const ExploreRenderRlePairAbi* pairs,
                                                                        const ExploreRenderAnnotationDescriptorAbi& annotation, std::uint32_t capacity,
                                                                        std::uint32_t width, std::uint32_t height, float x, float y) noexcept {
    if (annotation.mask_present) return sample_annotation_mask(pairs, annotation, capacity, width, height, x, y);
    return x >= annotation.box_xyxy[0] && x < annotation.box_xyxy[2] && y >= annotation.box_xyxy[1] && y < annotation.box_xyxy[3];
}
}  // namespace mmltk::backend::imaging::explore::detail
