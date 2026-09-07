#pragma once

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "detail/image_upscaler_nis.h"
#include "detail/image_upscaler_nis_coefficients.h"

namespace mmltk::backend::imaging::upscale::image_upscaler_nis::device {

inline constexpr std::uint32_t kPhaseCount = static_cast<std::uint32_t>(coefficients::kPhaseCount);
inline constexpr std::uint32_t kFilterTaps = static_cast<std::uint32_t>(coefficients::kFilterTaps);

using DeviceCoefficientTable = coefficients::CoefficientTable;

extern __device__ __constant__ const DeviceCoefficientTable kScaleCoefficients;
extern __device__ __constant__ const DeviceCoefficientTable kUsmCoefficients;

struct alignas(8) HalfRgba final {
    __half red;
    __half green;
    __half blue;
    __half alpha;
};

__device__ __forceinline__ float channel_mean(const std::uint32_t channel) {
    return channel == 0U ? 0.485F : (channel == 1U ? 0.456F : 0.406F);
}

__device__ __forceinline__ float channel_std(const std::uint32_t channel) {
    return channel == 0U ? 0.229F : (channel == 1U ? 0.224F : 0.225F);
}

__device__ __forceinline__ std::uint32_t phase(const float source_coordinate) {
    const float fraction = source_coordinate - floorf(source_coordinate);
    return min(kPhaseCount - 1U, static_cast<std::uint32_t>(fraction * static_cast<float>(kPhaseCount)));
}

__device__ __forceinline__ float normalized_channel(const float* pixels, const std::uint32_t width, const std::uint32_t height,
                                                    const std::uint32_t x, const std::uint32_t y, const std::uint32_t channel) {
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    return pixels[static_cast<std::size_t>(channel) * plane + static_cast<std::size_t>(y) * width + x] * channel_std(channel) +
           channel_mean(channel);
}

// Workflow-neutral direct sampler used by atlas-style consumers that cannot
// materialize a single rectangular separable intermediate.
__device__ __forceinline__ float3 sample_normalized_nchw(const float* pixels, const std::uint32_t width, const std::uint32_t height,
                                                         const float source_x, const float source_y) {
    const int base_x = static_cast<int>(floorf(source_x)) - 2;
    const int base_y = static_cast<int>(floorf(source_y)) - 2;
    const std::uint32_t phase_x = phase(source_x);
    const std::uint32_t phase_y = phase(source_y);
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    float3 color = make_float3(0.0F, 0.0F, 0.0F);
    float3 horizontal_detail = make_float3(0.0F, 0.0F, 0.0F);
    float3 vertical_detail = make_float3(0.0F, 0.0F, 0.0F);
#pragma unroll
    for (std::uint32_t tap_y = 0U; tap_y < kFilterTaps; ++tap_y) {
        const std::uint32_t y = static_cast<std::uint32_t>(max(0, min(static_cast<int>(height) - 1, base_y + static_cast<int>(tap_y))));
        const float vertical = kScaleCoefficients.values[phase_y][tap_y];
        const float vertical_usm = kUsmCoefficients.values[phase_y][tap_y];
#pragma unroll
        for (std::uint32_t tap_x = 0U; tap_x < kFilterTaps; ++tap_x) {
            const std::uint32_t x = static_cast<std::uint32_t>(max(0, min(static_cast<int>(width) - 1, base_x + static_cast<int>(tap_x))));
            const float coefficient = vertical * kScaleCoefficients.values[phase_x][tap_x];
            const float horizontal_coefficient = vertical * kUsmCoefficients.values[phase_x][tap_x];
            const float vertical_coefficient = vertical_usm * kScaleCoefficients.values[phase_x][tap_x];
            const std::size_t offset = static_cast<std::size_t>(y) * width + x;
            const float red = pixels[offset] * channel_std(0U) + channel_mean(0U);
            const float green = pixels[plane + offset] * channel_std(1U) + channel_mean(1U);
            const float blue = pixels[plane * 2U + offset] * channel_std(2U) + channel_mean(2U);
            color.x = fmaf(coefficient, red, color.x);
            color.y = fmaf(coefficient, green, color.y);
            color.z = fmaf(coefficient, blue, color.z);
            horizontal_detail.x = fmaf(horizontal_coefficient, red, horizontal_detail.x);
            horizontal_detail.y = fmaf(horizontal_coefficient, green, horizontal_detail.y);
            horizontal_detail.z = fmaf(horizontal_coefficient, blue, horizontal_detail.z);
            vertical_detail.x = fmaf(vertical_coefficient, red, vertical_detail.x);
            vertical_detail.y = fmaf(vertical_coefficient, green, vertical_detail.y);
            vertical_detail.z = fmaf(vertical_coefficient, blue, vertical_detail.z);
        }
    }
    const float horizontal_luma =
        fabsf(fmaf(0.2126F, horizontal_detail.x, fmaf(0.7152F, horizontal_detail.y, 0.0722F * horizontal_detail.z)));
    const float vertical_luma = fabsf(fmaf(0.2126F, vertical_detail.x, fmaf(0.7152F, vertical_detail.y, 0.0722F * vertical_detail.z)));
    const float3 detail = horizontal_luma <= vertical_luma ? horizontal_detail : vertical_detail;
    constexpr float kSharpness = 0.18F;
    color.x = fmaf(kSharpness, detail.x, color.x);
    color.y = fmaf(kSharpness, detail.y, color.y);
    color.z = fmaf(kSharpness, detail.z, color.z);
    return color;
}

}  // namespace mmltk::backend::imaging::upscale::image_upscaler_nis::device
