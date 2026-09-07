#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gpu_augment_cuda_launch.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"

// The host solves each image's geometry and effects once. CUDA image kernels consume that
// parameter block, while annotation consumers retain the same compact spatial-erasure plan.
// Sampling preserves the original counter stream; these functions allocate no storage.
namespace mmltk::backend::models::rfdetr::augment_math {

inline constexpr float kPi = 3.14159265358979323846F;
[[nodiscard]] __host__ __device__ __forceinline__ std::uint64_t image_key(const std::uint64_t seed, const int epoch, const int rank,
                                                                          const std::uint64_t sequence, const std::int64_t image) {
    return mix64(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(epoch)) << 32U) ^
                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rank)) * 0xd2b74407b1ce6e93ULL) ^
                 (sequence * 0xca5a826395121157ULL) ^ (static_cast<std::uint64_t>(image) * kGoldenRatio));
}

[[nodiscard]] __host__ __device__ __forceinline__ float sample_strength(const GpuAugmentationGroupLaunchConfig& group,
                                                                        const std::uint64_t key, std::uint64_t& counter) {
    const float value = uniform01(key, counter++);
    return fmaf(group.max_strength - group.min_strength, value, group.min_strength);
}

[[nodiscard]] __host__ __device__ __forceinline__ float clamp01(const float value) { return fminf(1.0F, fmaxf(0.0F, value)); }

// Affine transform sampled for one image, plus the resize factors the host planner records.
struct GeometryPlan {
    float forward[6];
    float inverse[6];
    float area_scale;
    float resize_scale;
    float resize_offset_x;
    float resize_offset_y;
    bool flip_x;
    bool flip_y;
};

// Draws flip/rotation/resize from `key`, advancing `counter` so callers stay on the same key stream,
// and solves the forward transform together with its inverse.
[[nodiscard]] __host__ __device__ __forceinline__ GeometryPlan solve_geometry_plan(const GpuAugmentationLaunchConfig& config,
                                                                                   const std::uint64_t key, std::uint64_t& counter) {
    const bool flip_x = uniform01(key, counter++) < config.geometry.probability * 0.5F;
    const bool flip_y = uniform01(key, counter++) < config.geometry.probability * 0.5F;
    float angle = 0.0F;
    if (uniform01(key, counter++) < config.geometry.probability * 0.35F) {
        const float strength = sample_strength(config.geometry, key, counter);
        angle = (uniform01(key, counter++) * 2.0F - 1.0F) * (10.0F * kPi / 180.0F) * strength;
    }

    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    const float rotation_scale = 1.0F / (fabsf(cosine) + fabsf(sine));
    const float flip_scale_x = flip_x ? -1.0F : 1.0F;
    const float flip_scale_y = flip_y ? -1.0F : 1.0F;
    float forward00 = rotation_scale * cosine * flip_scale_x;
    float forward01 = -rotation_scale * sine * flip_scale_y;
    float forward10 = rotation_scale * sine * flip_scale_x;
    float forward11 = rotation_scale * cosine * flip_scale_y;
    float forward02 = 0.5F - 0.5F * (forward00 + forward01);
    float forward12 = 0.5F - 0.5F * (forward10 + forward11);

    float resize_scale = 1.0F;
    float resize_offset_x = 0.0F;
    float resize_offset_y = 0.0F;
    if (uniform01(key, counter++) < config.resize.probability) {
        const float strength = sample_strength(config.resize, key, counter);
        resize_scale = 1.0F + (uniform01(key, counter++) < 0.5F ? -0.5F : 0.5F) * strength;
        const float extent = fabsf(1.0F - resize_scale);
        const float direction = resize_scale < 1.0F ? 1.0F : -1.0F;
        resize_offset_x = uniform01(key, counter++) * extent * direction;
        resize_offset_y = uniform01(key, counter++) * extent * direction;
        forward00 *= resize_scale;
        forward01 *= resize_scale;
        forward02 = fmaf(resize_scale, forward02, resize_offset_x);
        forward10 *= resize_scale;
        forward11 *= resize_scale;
        forward12 = fmaf(resize_scale, forward12, resize_offset_y);
    }

    const float determinant = forward00 * forward11 - forward01 * forward10;
    const float inverse_determinant = 1.0F / determinant;
    const float inverse00 = forward11 * inverse_determinant;
    const float inverse01 = -forward01 * inverse_determinant;
    const float inverse10 = -forward10 * inverse_determinant;
    const float inverse11 = forward00 * inverse_determinant;
    return GeometryPlan{
        {forward00, forward01, forward02, forward10, forward11, forward12},
        {inverse00, inverse01, -(inverse00 * forward02 + inverse01 * forward12), inverse10, inverse11,
         -(inverse10 * forward02 + inverse11 * forward12)},
        fabsf(determinant),
        resize_scale,
        resize_offset_x,
        resize_offset_y,
        flip_x,
        flip_y,
    };
}

enum ParameterIndex : std::uint8_t {
    kInverse00 = 0,
    kInverse01 = 1,
    kInverse02 = 2,
    kInverse10 = 3,
    kInverse11 = 4,
    kInverse12 = 5,
    kForward00 = 6,
    kForward01 = 7,
    kForward02 = 8,
    kForward10 = 9,
    kForward11 = 10,
    kForward12 = 11,
    kColorMatrix = 12,
    kColorOffset = 21,
    kNoiseMode = 24,
    kNoiseStrength = 25,
    kBlurStrength = 26,
    kOcclusionMode = 27,
    kOcclusionStrength = 28,
    kOcclusionChannel = 29,
    kEraseX0 = 30,
    kEraseY0 = 31,
    kEraseX1 = 32,
    kEraseY1 = 33,
    kAreaScale = 34,
    kFlipX = 36,
    kFlipY = 37,
};

// Plan all image effects once on the host; the image kernel consumes the staged parameters.
[[nodiscard]] inline GeometryPlan solve_image_plan(float* values, const GpuAugmentationLaunchConfig& config, const std::uint64_t key) {
    for (int index = 0; index < kGpuAugmentationParameterCount; ++index) {
        values[index] = 0.0F;
    }
    values[kInverse00] = 1.0F;
    values[kInverse11] = 1.0F;
    values[kForward00] = 1.0F;
    values[kForward11] = 1.0F;
    values[kColorMatrix + 0] = 1.0F;
    values[kColorMatrix + 4] = 1.0F;
    values[kColorMatrix + 8] = 1.0F;
    values[kAreaScale] = 1.0F;
    if (config.enabled == 0) {
        return GeometryPlan{
            {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}, 1.0F, 1.0F, 0.0F, 0.0F, false, false};
    }

    std::uint64_t counter = 0;
    const augment_math::GeometryPlan geometry = augment_math::solve_geometry_plan(config, key, counter);
    for (int index = 0; index < 6; ++index) {
        values[kForward00 + index] = geometry.forward[index];
        values[kInverse00 + index] = geometry.inverse[index];
    }
    values[kAreaScale] = geometry.area_scale;
    values[kFlipX] = geometry.flip_x ? 1.0F : 0.0F;
    values[kFlipY] = geometry.flip_y ? 1.0F : 0.0F;

    // Preserve conditional draws in order: geometry/resize, color, noise, blur, then occlusion.
    // Donor and cache choices use their independent 0x4000/0x5000 counter ranges.
    if (uniform01(key, counter++) < config.color.probability) {
        const float strength = sample_strength(config.color, key, counter);
        const float brightness = (uniform01(key, counter++) * 2.0F - 1.0F) * 0.15F * strength;
        const float contrast = 1.0F + (uniform01(key, counter++) * 2.0F - 1.0F) * 0.30F * strength;
        float saturation = 1.0F + (uniform01(key, counter++) * 2.0F - 1.0F) * 0.30F * strength;
        if (uniform01(key, counter++) < 0.10F * strength) { saturation = 0.0F; }
        constexpr float luma[3] = {0.299F, 0.587F, 0.114F};
        for (int output_channel = 0; output_channel < 3; ++output_channel) {
            for (int input_channel = 0; input_channel < 3; ++input_channel) {
                values[kColorMatrix + output_channel * 3 + input_channel] =
                    contrast * ((output_channel == input_channel ? saturation : 0.0F) + (1.0F - saturation) * luma[input_channel]);
            }
            values[kColorOffset + output_channel] = 0.5F * (1.0F - contrast) + brightness;
        }
    }

    if (uniform01(key, counter++) < config.noise.probability) {
        values[kNoiseMode] = uniform01(key, counter++) < 0.5F ? 1.0F : 2.0F;
        values[kNoiseStrength] = sample_strength(config.noise, key, counter);
    }
    if (uniform01(key, counter++) < config.blur.probability) { values[kBlurStrength] = sample_strength(config.blur, key, counter); }
    if (uniform01(key, counter++) < config.occlusion.probability) {
        const float strength = sample_strength(config.occlusion, key, counter);
        const int mode = std::min(2, static_cast<int>(uniform01(key, counter++) * 3.0F)) + 1;
        values[kOcclusionMode] = static_cast<float>(mode);
        values[kOcclusionStrength] = strength;
        values[kOcclusionChannel] = static_cast<float>(std::min(2, static_cast<int>(uniform01(key, counter++) * 3.0F)));
        if (mode == 3) {
            const float area = 0.15F * strength;
            const float aspect = 0.5F + 1.5F * uniform01(key, counter++);
            const float erase_width = fminf(1.0F, sqrtf(area * aspect));
            const float erase_height = fminf(1.0F, area / fmaxf(erase_width, 1.0e-6F));
            const float x0 = uniform01(key, counter++) * (1.0F - erase_width);
            const float y0 = uniform01(key, counter++) * (1.0F - erase_height);
            values[kEraseX0] = x0;
            values[kEraseY0] = y0;
            values[kEraseX1] = x0 + erase_width;
            values[kEraseY1] = y0 + erase_height;
        }
    }
    return geometry;
}

[[nodiscard]] __host__ __device__ __forceinline__ AugmentationSpatialErasure spatial_erasure(const float* values, const std::uint64_t key) {
    return {key,
            values[kOcclusionMode] == 2.0F ? 0.05F * values[kOcclusionStrength] : 0.0F,
            values[kEraseX0],
            values[kEraseY0],
            values[kEraseX1],
            values[kEraseY1],
            values[kOcclusionMode] == 3.0F ? 1U : 0U};
}

}  // namespace mmltk::backend::models::rfdetr::augment_math
