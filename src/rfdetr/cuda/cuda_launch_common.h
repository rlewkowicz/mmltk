#pragma once

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mmltk::rfdetr::cuda_launch {

inline constexpr int kDefaultLinearThreads = 256;

struct RgbPixelFloat {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct RgbaPixelU8 {
    std::uint8_t r = 0U;
    std::uint8_t g = 0U;
    std::uint8_t b = 0U;
    std::uint8_t a = 0U;
};

inline int linear_blocks_for(const int item_count,
                             const int threads = kDefaultLinearThreads) {
    const int safe_threads = std::max(1, threads);
    const int safe_items = std::max(0, item_count);
    return (safe_items + safe_threads - 1) / safe_threads;
}

inline dim3 make_2d_grid(const int width,
                         const int height,
                         const dim3 block = dim3(16, 16, 1)) {
    return dim3((std::max(0, width) + static_cast<int>(block.x) - 1) / block.x,
                (std::max(0, height) + static_cast<int>(block.y) - 1) / block.y,
                1);
}

__device__ __forceinline__ std::uint8_t clamp_to_u8(const float value) {
    return static_cast<std::uint8_t>(fminf(255.0f, fmaxf(0.0f, value)));
}

__device__ __forceinline__ std::uint8_t* pitched_pixel_ptr(std::uint8_t* base,
                                                           const std::size_t pitch_bytes,
                                                           const int x,
                                                           const int y,
                                                           const std::size_t channel_count) {
    return base + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * channel_count;
}

__device__ __forceinline__ const std::uint8_t* pitched_pixel_ptr(const std::uint8_t* base,
                                                                 const std::size_t pitch_bytes,
                                                                 const int x,
                                                                 const int y,
                                                                 const std::size_t channel_count) {
    return base + static_cast<std::size_t>(y) * pitch_bytes + static_cast<std::size_t>(x) * channel_count;
}

__device__ __forceinline__ RgbPixelFloat load_bgr_pixel(const std::uint8_t* base,
                                                        const std::size_t pitch_bytes,
                                                        const int x,
                                                        const int y) {
    const std::uint8_t* pixel = pitched_pixel_ptr(base, pitch_bytes, x, y, 3U);
    return RgbPixelFloat{
        static_cast<float>(pixel[2]),
        static_cast<float>(pixel[1]),
        static_cast<float>(pixel[0]),
    };
}

__device__ __forceinline__ RgbPixelFloat load_rgb_pixel(const std::uint8_t* base,
                                                        const std::size_t pitch_bytes,
                                                        const int x,
                                                        const int y) {
    const std::uint8_t* pixel = pitched_pixel_ptr(base, pitch_bytes, x, y, 3U);
    return RgbPixelFloat{
        static_cast<float>(pixel[0]),
        static_cast<float>(pixel[1]),
        static_cast<float>(pixel[2]),
    };
}

__device__ __forceinline__ void store_bgr_pixel(std::uint8_t* base,
                                                const std::size_t pitch_bytes,
                                                const int x,
                                                const int y,
                                                const RgbPixelFloat& pixel) {
    std::uint8_t* dst = pitched_pixel_ptr(base, pitch_bytes, x, y, 3U);
    dst[0] = clamp_to_u8(pixel.b);
    dst[1] = clamp_to_u8(pixel.g);
    dst[2] = clamp_to_u8(pixel.r);
}

__device__ __forceinline__ void store_rgb_pixel(std::uint8_t* base,
                                                const std::size_t pitch_bytes,
                                                const int x,
                                                const int y,
                                                const RgbPixelFloat& pixel) {
    std::uint8_t* dst = pitched_pixel_ptr(base, pitch_bytes, x, y, 3U);
    dst[0] = clamp_to_u8(pixel.r);
    dst[1] = clamp_to_u8(pixel.g);
    dst[2] = clamp_to_u8(pixel.b);
}

__device__ __forceinline__ RgbaPixelU8 load_rgba_pixel(const std::uint8_t* base,
                                                       const std::size_t pitch_bytes,
                                                       const int x,
                                                       const int y) {
    const std::uint8_t* pixel = pitched_pixel_ptr(base, pitch_bytes, x, y, 4U);
    return RgbaPixelU8{pixel[0], pixel[1], pixel[2], pixel[3]};
}

__device__ __forceinline__ void store_rgba_pixel(std::uint8_t* base,
                                                 const std::size_t pitch_bytes,
                                                 const int x,
                                                 const int y,
                                                 const RgbaPixelU8& pixel) {
    std::uint8_t* dst = pitched_pixel_ptr(base, pitch_bytes, x, y, 4U);
    dst[0] = pixel.r;
    dst[1] = pixel.g;
    dst[2] = pixel.b;
    dst[3] = pixel.a;
}

__device__ __forceinline__ RgbPixelFloat blend_rgb(const RgbPixelFloat& base,
                                                   const std::uint8_t r,
                                                   const std::uint8_t g,
                                                   const std::uint8_t b,
                                                   const float alpha) {
    const float inv_alpha = 1.0f - alpha;
    return RgbPixelFloat{
        base.r * inv_alpha + static_cast<float>(r) * alpha,
        base.g * inv_alpha + static_cast<float>(g) * alpha,
        base.b * inv_alpha + static_cast<float>(b) * alpha,
    };
}

__device__ __forceinline__ void apply_rgb(std::uint8_t* r,
                                          std::uint8_t* g,
                                          std::uint8_t* b,
                                          const std::uint8_t* colors,
                                          const int color_offset) {
    *r = colors[color_offset + 0];
    *g = colors[color_offset + 1];
    *b = colors[color_offset + 2];
}

__device__ __forceinline__ void apply_rgb(RgbPixelFloat* pixel,
                                          const std::uint8_t* colors,
                                          const int color_offset) {
    pixel->r = static_cast<float>(colors[color_offset + 0]);
    pixel->g = static_cast<float>(colors[color_offset + 1]);
    pixel->b = static_cast<float>(colors[color_offset + 2]);
}

__device__ __forceinline__ RgbPixelFloat composite_rgba_over_bgr(const RgbPixelFloat& base,
                                                                 const RgbaPixelU8& overlay) {
    if (overlay.a == 0U) {
        return base;
    }
    if (overlay.a == 255U) {
        return RgbPixelFloat{
            static_cast<float>(overlay.r),
            static_cast<float>(overlay.g),
            static_cast<float>(overlay.b),
        };
    }
    return blend_rgb(base,
                     overlay.r,
                     overlay.g,
                     overlay.b,
                     static_cast<float>(overlay.a) / 255.0f);
}

__device__ __forceinline__ float point_distance_sq(const float px,
                                                   const float py,
                                                   const float qx,
                                                   const float qy) {
    const float dx = px - qx;
    const float dy = py - qy;
    return dx * dx + dy * dy;
}

__device__ __forceinline__ float point_to_segment_distance_sq(const float px,
                                                              const float py,
                                                              const float ax,
                                                              const float ay,
                                                              const float bx,
                                                              const float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = px - ax;
    const float apy = py - ay;
    const float ab_len_sq = abx * abx + aby * aby;
    if (ab_len_sq <= 0.0f) {
        return point_distance_sq(px, py, ax, ay);
    }
    const float t = fminf(1.0f, fmaxf(0.0f, (apx * abx + apy * aby) / ab_len_sq));
    return point_distance_sq(px, py, ax + abx * t, ay + aby * t);
}

} // namespace mmltk::rfdetr::cuda_launch
