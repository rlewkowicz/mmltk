#include "rfdetr/gpu_augment_cuda_launch.h"

#include "rfdetr/torch_cuda_utils.h"

#include <cuda_runtime.h>
#include <cuda_bf16.h>
#include <cuda_fp16.h>

#include <cmath>
#include <cstdint>

namespace mmltk::rfdetr {

namespace {

constexpr int kThreads = 256;
constexpr float kPi = 3.14159265358979323846F;

enum ParameterIndex : int {
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

enum CopyPasteParameterIndex : int {
    kPasteDonorSlot = 0,
    kPasteMode = 1,
    kPasteInverse00 = 2,
    kPasteInverse01 = 3,
    kPasteInverse02 = 4,
    kPasteInverse10 = 5,
    kPasteInverse11 = 6,
    kPasteInverse12 = 7,
};

__host__ __device__ constexpr std::int64_t ceil_div(const std::int64_t value, const std::int64_t divisor) {
    return (value + divisor - 1) / divisor;
}

__device__ __forceinline__ std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

__device__ __forceinline__ float uniform01(const std::uint64_t key, const std::uint64_t counter) {
    return static_cast<float>((mix64(key + counter * 0x9e3779b97f4a7c15ULL) >> 40U) & 0xFFFFFFULL) *
           (1.0F / 16777216.0F);
}

__device__ __forceinline__ std::uint64_t image_key(const std::uint64_t seed, const int epoch, const int rank,
                                                   const std::uint64_t sequence, const std::int64_t image) {
    return mix64(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(epoch)) << 32U) ^
                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rank)) * 0xd2b74407b1ce6e93ULL) ^
                 (sequence * 0xca5a826395121157ULL) ^ (static_cast<std::uint64_t>(image) * 0x9e3779b97f4a7c15ULL));
}

__device__ __forceinline__ float sample_strength(const GpuAugmentationGroupLaunchConfig group, const std::uint64_t key,
                                                 std::uint64_t& counter) {
    const float value = uniform01(key, counter++);
    return fmaf(group.max_strength - group.min_strength, value, group.min_strength);
}

__device__ __forceinline__ float clamp01(const float value) {
    return fminf(1.0F, fmaxf(0.0F, value));
}

__device__ __forceinline__ float channel_mean(const int channel) {
    return channel == 0 ? 0.485F : (channel == 1 ? 0.456F : 0.406F);
}

__device__ __forceinline__ float channel_std(const int channel) {
    return channel == 0 ? 0.229F : (channel == 1 ? 0.224F : 0.225F);
}

template <typename Output>
__device__ __forceinline__ Output normalized_output(const float value, const int channel);

template <typename Output>
__device__ __forceinline__ Output zero_output();

template <>
__device__ __forceinline__ float normalized_output<float>(const float value, const int channel) {
    return (value - channel_mean(channel)) / channel_std(channel);
}

template <>
__device__ __forceinline__ float zero_output<float>() {
    return 0.0F;
}

template <>
__device__ __forceinline__ __half normalized_output<__half>(const float value, const int channel) {
    return __float2half_rn((value - channel_mean(channel)) / channel_std(channel));
}

template <>
__device__ __forceinline__ __half zero_output<__half>() {
    return __float2half_rn(0.0F);
}

template <>
__device__ __forceinline__ __nv_bfloat16 normalized_output<__nv_bfloat16>(const float value, const int channel) {
    return __float2bfloat16_rn((value - channel_mean(channel)) / channel_std(channel));
}

template <>
__device__ __forceinline__ __nv_bfloat16 zero_output<__nv_bfloat16>() {
    return __float2bfloat16_rn(0.0F);
}

template <typename Output>
__global__ void normalize_images_kernel(const float* input, Output* output, const std::int64_t active_batch_size,
                                        const std::int64_t output_batch_size, const int height, const int width) {
    const int groups_per_row = static_cast<int>(ceil_div(width, 4));
    const std::int64_t group_index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total_groups = output_batch_size * 3 * static_cast<std::int64_t>(height) * groups_per_row;
    if (group_index >= total_groups) {
        return;
    }

    const int group_x = static_cast<int>(group_index % groups_per_row);
    const std::int64_t row_index = group_index / groups_per_row;
    const int row = static_cast<int>(row_index % height);
    const std::int64_t plane_index = row_index / height;
    const int channel = static_cast<int>(plane_index % 3);
    const std::int64_t image = plane_index / 3;
    const int x = group_x * 4;
    const std::int64_t offset = ((image * 3 + channel) * height + row) * static_cast<std::int64_t>(width) + x;
    const bool active = image < active_batch_size;
    float4 values{};
    if (active) {
        if ((width & 3) == 0 && x + 3 < width) {
            values = *reinterpret_cast<const float4*>(input + offset);
        } else {
            float* lanes = reinterpret_cast<float*>(&values);
#pragma unroll
            for (int lane = 0; lane < 4; ++lane) {
                if (x + lane < width) {
                    lanes[lane] = input[offset + lane];
                }
            }
        }
    }
    const float* input_values = reinterpret_cast<const float*>(&values);

#pragma unroll
    for (int lane = 0; lane < 4; ++lane) {
        if (x + lane >= width) {
            break;
        }
        output[offset + lane] = active ? normalized_output<Output>(input_values[lane], channel) : zero_output<Output>();
    }
}

__global__ void generate_parameters_kernel(float* parameters, const std::int64_t batch_size,
                                           const GpuAugmentationLaunchConfig config, const std::uint64_t seed,
                                           const int epoch, const int rank, const std::uint64_t sequence) {
    const std::int64_t image = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (image >= batch_size) {
        return;
    }

    float* values = parameters + image * kGpuAugmentationParameterCount;
#pragma unroll
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
        return;
    }

    const std::uint64_t key = image_key(seed, epoch, rank, sequence, image);
    std::uint64_t counter = 0;
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
    if (uniform01(key, counter++) < config.resize.probability) {
        const float strength = sample_strength(config.resize, key, counter);
        const float resize_scale = 1.0F + (uniform01(key, counter++) < 0.5F ? -0.5F : 0.5F) * strength;
        const float offset_extent = fabsf(1.0F - resize_scale);
        const float offset_x = uniform01(key, counter++) * offset_extent * (resize_scale < 1.0F ? 1.0F : -1.0F);
        const float offset_y = uniform01(key, counter++) * offset_extent * (resize_scale < 1.0F ? 1.0F : -1.0F);
        forward00 *= resize_scale;
        forward01 *= resize_scale;
        forward02 = fmaf(resize_scale, forward02, offset_x);
        forward10 *= resize_scale;
        forward11 *= resize_scale;
        forward12 = fmaf(resize_scale, forward12, offset_y);
    }
    const float determinant = forward00 * forward11 - forward01 * forward10;
    const float inverse_determinant = 1.0F / determinant;
    const float inverse00 = forward11 * inverse_determinant;
    const float inverse01 = -forward01 * inverse_determinant;
    const float inverse10 = -forward10 * inverse_determinant;
    const float inverse11 = forward00 * inverse_determinant;
    values[kForward00] = forward00;
    values[kForward01] = forward01;
    values[kForward02] = forward02;
    values[kForward10] = forward10;
    values[kForward11] = forward11;
    values[kForward12] = forward12;
    values[kInverse00] = inverse00;
    values[kInverse01] = inverse01;
    values[kInverse02] = -(inverse00 * forward02 + inverse01 * forward12);
    values[kInverse10] = inverse10;
    values[kInverse11] = inverse11;
    values[kInverse12] = -(inverse10 * forward02 + inverse11 * forward12);
    values[kAreaScale] = fabsf(determinant);
    values[kFlipX] = flip_x ? 1.0F : 0.0F;
    values[kFlipY] = flip_y ? 1.0F : 0.0F;

    if (uniform01(key, counter++) < config.color.probability) {
        const float strength = sample_strength(config.color, key, counter);
        const float brightness = (uniform01(key, counter++) * 2.0F - 1.0F) * 0.15F * strength;
        const float contrast = 1.0F + (uniform01(key, counter++) * 2.0F - 1.0F) * 0.30F * strength;
        float saturation = 1.0F + (uniform01(key, counter++) * 2.0F - 1.0F) * 0.30F * strength;
        if (uniform01(key, counter++) < 0.10F * strength) {
            saturation = 0.0F;
        }
        constexpr float luma[3] = {0.299F, 0.587F, 0.114F};
#pragma unroll
        for (int output_channel = 0; output_channel < 3; ++output_channel) {
#pragma unroll
            for (int input_channel = 0; input_channel < 3; ++input_channel) {
                values[kColorMatrix + output_channel * 3 + input_channel] =
                    contrast *
                    ((output_channel == input_channel ? saturation : 0.0F) + (1.0F - saturation) * luma[input_channel]);
            }
            values[kColorOffset + output_channel] = 0.5F * (1.0F - contrast) + brightness;
        }
    }

    if (uniform01(key, counter++) < config.noise.probability) {
        values[kNoiseMode] = uniform01(key, counter++) < 0.5F ? 1.0F : 2.0F;
        values[kNoiseStrength] = sample_strength(config.noise, key, counter);
    }
    if (uniform01(key, counter++) < config.blur.probability) {
        values[kBlurStrength] = sample_strength(config.blur, key, counter);
    }
    if (uniform01(key, counter++) < config.occlusion.probability) {
        const float strength = sample_strength(config.occlusion, key, counter);
        const int mode = min(2, static_cast<int>(uniform01(key, counter++) * 3.0F)) + 1;
        values[kOcclusionMode] = static_cast<float>(mode);
        values[kOcclusionStrength] = strength;
        values[kOcclusionChannel] = static_cast<float>(min(2, static_cast<int>(uniform01(key, counter++) * 3.0F)));
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
}

__device__ __forceinline__ float approximate_gaussian(const std::uint64_t key, const std::uint64_t base) {
    return uniform01(key, base) + uniform01(key, base + 1) + uniform01(key, base + 2) + uniform01(key, base + 3) - 2.0F;
}

__device__ __forceinline__ void apply_effects(float& red, float& green, float& blue, const float* values,
                                              const std::uint64_t key, const std::int64_t pixel_index,
                                              const float normalized_x, const float normalized_y) {
    const float input_red = red;
    const float input_green = green;
    const float input_blue = blue;
    red = values[kColorMatrix + 0] * input_red + values[kColorMatrix + 1] * input_green +
          values[kColorMatrix + 2] * input_blue + values[kColorOffset + 0];
    green = values[kColorMatrix + 3] * input_red + values[kColorMatrix + 4] * input_green +
            values[kColorMatrix + 5] * input_blue + values[kColorOffset + 1];
    blue = values[kColorMatrix + 6] * input_red + values[kColorMatrix + 7] * input_green +
           values[kColorMatrix + 8] * input_blue + values[kColorOffset + 2];

    const int noise_mode = static_cast<int>(values[kNoiseMode]);
    const float noise_strength = values[kNoiseStrength];
    const std::uint64_t pixel_counter = static_cast<std::uint64_t>(pixel_index) * 32ULL + 0x1000ULL;
    if (noise_mode == 1) {
        const float sigma = 0.06F * noise_strength;
        red += sigma * approximate_gaussian(key, pixel_counter + 0);
        green += sigma * approximate_gaussian(key, pixel_counter + 4);
        blue += sigma * approximate_gaussian(key, pixel_counter + 8);
    } else if (noise_mode == 2 && uniform01(key, pixel_counter + 12) < 0.02F * noise_strength) {
        red = 1.0F;
        green = 1.0F;
        blue = 1.0F;
    }

    const int occlusion_mode = static_cast<int>(values[kOcclusionMode]);
    const float occlusion_strength = values[kOcclusionStrength];
    bool erase_pixel = false;
    if (occlusion_mode == 2) {
        erase_pixel = uniform01(key, pixel_counter + 13) < 0.05F * occlusion_strength;
    } else if (occlusion_mode == 3) {
        erase_pixel = normalized_x >= values[kEraseX0] && normalized_x < values[kEraseX1] &&
                      normalized_y >= values[kEraseY0] && normalized_y < values[kEraseY1];
    }
    if (erase_pixel) {
        red = channel_mean(0);
        green = channel_mean(1);
        blue = channel_mean(2);
    } else if (occlusion_mode == 1) {
        const int channel = static_cast<int>(values[kOcclusionChannel]);
        if (channel == 0) {
            red = fmaf(occlusion_strength, channel_mean(0) - red, red);
        } else if (channel == 1) {
            green = fmaf(occlusion_strength, channel_mean(1) - green, green);
        } else {
            blue = fmaf(occlusion_strength, channel_mean(2) - blue, blue);
        }
    }

    red = (clamp01(red) - channel_mean(0)) / channel_std(0);
    green = (clamp01(green) - channel_mean(1)) / channel_std(1);
    blue = (clamp01(blue) - channel_mean(2)) / channel_std(2);
}

__device__ __forceinline__ float4 reverse_float4(const float4 value) {
    return make_float4(value.w, value.z, value.y, value.x);
}

__global__ void pointwise_images_kernel(const float* input, float* output, const float* parameters,
                                        const std::int64_t batch_size, const int height, const int width,
                                        const std::uint64_t seed, const int epoch, const int rank,
                                        const std::uint64_t sequence) {
    const int groups_per_row = static_cast<int>(ceil_div(width, 4));
    const std::int64_t group_index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total_groups = batch_size * static_cast<std::int64_t>(height) * groups_per_row;
    if (group_index >= total_groups) {
        return;
    }
    const int group_x = static_cast<int>(group_index % groups_per_row);
    const std::int64_t row_index = group_index / groups_per_row;
    const int output_y = static_cast<int>(row_index % height);
    const std::int64_t image = row_index / height;
    const int output_x = group_x * 4;
    const float* values = parameters + image * kGpuAugmentationParameterCount;
    const bool flip_x = values[kFlipX] != 0.0F;
    const bool flip_y = values[kFlipY] != 0.0F;
    const int source_y = flip_y ? height - output_y - 1 : output_y;
    const bool vector_aligned = width % 4 == 0 && output_x + 3 < width;
    float4 channels[3];

    if (vector_aligned) {
        const int source_x = flip_x ? width - output_x - 4 : output_x;
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            const std::int64_t source_offset =
                ((image * 3 + channel) * height + source_y) * static_cast<std::int64_t>(width) + source_x;
            const float4 loaded = *reinterpret_cast<const float4*>(input + source_offset);
            channels[channel] = flip_x ? reverse_float4(loaded) : loaded;
        }
    } else {
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            float* channel_values = reinterpret_cast<float*>(&channels[channel]);
#pragma unroll
            for (int lane = 0; lane < 4; ++lane) {
                const int x = output_x + lane;
                if (x >= width) {
                    channel_values[lane] = 0.0F;
                    continue;
                }
                const int source_x = flip_x ? width - x - 1 : x;
                const std::int64_t source_offset =
                    ((image * 3 + channel) * height + source_y) * static_cast<std::int64_t>(width) + source_x;
                channel_values[lane] = input[source_offset];
            }
        }
    }

    const std::uint64_t key = image_key(seed, epoch, rank, sequence, image);
#pragma unroll
    for (int lane = 0; lane < 4; ++lane) {
        const int x = output_x + lane;
        if (x >= width) {
            continue;
        }
        float* red_values = reinterpret_cast<float*>(&channels[0]);
        float* green_values = reinterpret_cast<float*>(&channels[1]);
        float* blue_values = reinterpret_cast<float*>(&channels[2]);
        const std::int64_t pixel_index = static_cast<std::int64_t>(output_y) * width + x;
        apply_effects(red_values[lane], green_values[lane], blue_values[lane], values, key, pixel_index,
                      (static_cast<float>(x) + 0.5F) / static_cast<float>(width),
                      (static_cast<float>(output_y) + 0.5F) / static_cast<float>(height));
    }

    if (vector_aligned) {
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            const std::int64_t output_offset =
                ((image * 3 + channel) * height + output_y) * static_cast<std::int64_t>(width) + output_x;
            *reinterpret_cast<float4*>(output + output_offset) = channels[channel];
        }
    } else {
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            const float* channel_values = reinterpret_cast<const float*>(&channels[channel]);
#pragma unroll
            for (int lane = 0; lane < 4; ++lane) {
                const int x = output_x + lane;
                if (x < width) {
                    const std::int64_t output_offset =
                        ((image * 3 + channel) * height + output_y) * static_cast<std::int64_t>(width) + x;
                    output[output_offset] = channel_values[lane];
                }
            }
        }
    }
}

__device__ __forceinline__ float remap_channel(const float* input, const std::int64_t plane_offset, const int height,
                                               const int width, const float source_x, const float source_y,
                                               const float blur_strength, const int channel) {
    if (source_x < 0.0F || source_x > 1.0F || source_y < 0.0F || source_y > 1.0F) {
        return channel_mean(channel);
    }
    const float pixel_x =
        fminf(fmaxf(source_x * static_cast<float>(width) - 0.5F, 0.0F), static_cast<float>(width - 1));
    const float pixel_y =
        fminf(fmaxf(source_y * static_cast<float>(height) - 0.5F, 0.0F), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(floorf(pixel_x));
    const int y0 = static_cast<int>(floorf(pixel_y));
    const int x1 = min(width - 1, x0 + 1);
    const int y1 = min(height - 1, y0 + 1);
    const float fraction_x = pixel_x - static_cast<float>(x0);
    const float fraction_y = pixel_y - static_cast<float>(y0);
    const float value00 = input[plane_offset + static_cast<std::int64_t>(y0) * width + x0];
    const float value01 = input[plane_offset + static_cast<std::int64_t>(y0) * width + x1];
    const float value10 = input[plane_offset + static_cast<std::int64_t>(y1) * width + x0];
    const float value11 = input[plane_offset + static_cast<std::int64_t>(y1) * width + x1];
    const float top = fmaf(fraction_x, value01 - value00, value00);
    const float bottom = fmaf(fraction_x, value11 - value10, value10);
    const float bilinear = fmaf(fraction_y, bottom - top, top);
    const float box = (value00 + value01 + value10 + value11) * 0.25F;
    return fmaf(blur_strength, box - bilinear, bilinear);
}

__device__ __forceinline__ bool packed_mask_contains(const std::int64_t* packed_masks,
                                                     const std::int64_t words_per_mask, const int slot,
                                                     const int height, const int width, const float source_x,
                                                     const float source_y) {
    if (packed_masks == nullptr || source_x < 0.0F || source_x > 1.0F || source_y < 0.0F || source_y > 1.0F) {
        return false;
    }
    const int x = min(width - 1, max(0, static_cast<int>(nearbyintf(source_x * static_cast<float>(width) - 0.5F))));
    const int y = min(height - 1, max(0, static_cast<int>(nearbyintf(source_y * static_cast<float>(height) - 0.5F))));
    const std::int64_t pixel = static_cast<std::int64_t>(y) * width + x;
    const auto* words = reinterpret_cast<const unsigned long long*>(packed_masks + slot * words_per_mask);
    return ((words[pixel >> 6] >> (pixel & 63)) & 1ULL) != 0ULL;
}

__global__ void remap_images_kernel(const float* input, float* output, const float* parameters,
                                    const float* copy_paste_parameters, const float* donor_images,
                                    const std::int64_t* donor_masks, const float* donor_boxes,
                                    const std::int64_t donor_mask_words, const std::int64_t batch_size,
                                    const int height, const int width, const std::uint64_t seed, const int epoch,
                                    const int rank, const std::uint64_t sequence) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t pixels_per_image = static_cast<std::int64_t>(height) * width;
    const std::int64_t total = batch_size * pixels_per_image;
    if (index >= total) {
        return;
    }
    const std::int64_t image = index / pixels_per_image;
    const std::int64_t pixel_index = index - image * pixels_per_image;
    const int output_y = static_cast<int>(pixel_index / width);
    const int output_x = static_cast<int>(pixel_index - static_cast<std::int64_t>(output_y) * width);
    const float normalized_x = (static_cast<float>(output_x) + 0.5F) / static_cast<float>(width);
    const float normalized_y = (static_cast<float>(output_y) + 0.5F) / static_cast<float>(height);
    const float* values = parameters + image * kGpuAugmentationParameterCount;
    const float source_x = values[kInverse00] * normalized_x + values[kInverse01] * normalized_y + values[kInverse02];
    const float source_y = values[kInverse10] * normalized_x + values[kInverse11] * normalized_y + values[kInverse12];
    const float blur_strength = values[kBlurStrength];
    float channels[3];
#pragma unroll
    for (int channel = 0; channel < 3; ++channel) {
        const std::int64_t plane_offset = (image * 3 + channel) * pixels_per_image;
        channels[channel] =
            remap_channel(input, plane_offset, height, width, source_x, source_y, blur_strength, channel);
    }
    if (copy_paste_parameters != nullptr && donor_images != nullptr) {
        const float* paste = copy_paste_parameters + image * kGpuCopyPasteParameterCount;
        const int donor_slot = static_cast<int>(paste[kPasteDonorSlot]);
        if (donor_slot >= 0) {
            const float donor_x =
                paste[kPasteInverse00] * normalized_x + paste[kPasteInverse01] * normalized_y + paste[kPasteInverse02];
            const float donor_y =
                paste[kPasteInverse10] * normalized_x + paste[kPasteInverse11] * normalized_y + paste[kPasteInverse12];
            const int mode = static_cast<int>(paste[kPasteMode]);
            bool paste_pixel = false;
            if (mode == 1) {
                paste_pixel =
                    packed_mask_contains(donor_masks, donor_mask_words, donor_slot, height, width, donor_x, donor_y);
            } else if (mode == 2 && donor_boxes != nullptr) {
                const float* box = donor_boxes + donor_slot * 4;
                paste_pixel = donor_x >= box[0] && donor_x <= box[2] && donor_y >= box[1] && donor_y <= box[3];
            }
            if (paste_pixel) {
#pragma unroll
                for (int channel = 0; channel < 3; ++channel) {
                    const std::int64_t donor_plane =
                        (static_cast<std::int64_t>(donor_slot) * 3 + channel) * pixels_per_image;
                    channels[channel] = remap_channel(donor_images, donor_plane, height, width, donor_x, donor_y,
                                                      blur_strength, channel);
                }
            }
        }
    }
    apply_effects(channels[0], channels[1], channels[2], values, image_key(seed, epoch, rank, sequence, image),
                  pixel_index, normalized_x, normalized_y);
#pragma unroll
    for (int channel = 0; channel < 3; ++channel) {
        output[(image * 3 + channel) * pixels_per_image + pixel_index] = channels[channel];
    }
}

__global__ void update_donor_images_kernel(const float* source_images, float* donor_images,
                                           const std::int64_t* replacement_target_indices,
                                           const std::int64_t batch_size, const std::int64_t pixels_per_image) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t values_per_image = pixels_per_image * 3;
    const std::int64_t total = batch_size * values_per_image;
    if (index >= total) {
        return;
    }
    const std::int64_t image = index / values_per_image;
    if (replacement_target_indices[image] >= 0) {
        donor_images[index] = source_images[index];
    }
}

__global__ void update_donor_masks_kernel(const std::int64_t* source_masks, std::int64_t* donor_masks,
                                          const std::int64_t* replacement_target_indices, const std::int64_t batch_size,
                                          const std::int64_t mask_words) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total = batch_size * mask_words;
    if (index >= total) {
        return;
    }
    const std::int64_t slot = index / mask_words;
    const std::int64_t source_target = replacement_target_indices[slot];
    if (source_target >= 0) {
        donor_masks[index] = source_masks[source_target * mask_words + index % mask_words];
    }
}

__global__ void copy_cached_masks_kernel(const std::int64_t* donor_masks, std::int64_t* target_masks,
                                         const std::int64_t* donor_slots, const std::int64_t* target_indices,
                                         const std::int64_t batch_size, const std::int64_t mask_words) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total = batch_size * mask_words;
    if (index >= total) {
        return;
    }
    const std::int64_t image = index / mask_words;
    const std::int64_t donor_slot = donor_slots[image];
    const std::int64_t target = target_indices[image];
    if (donor_slot >= 0 && target >= 0) {
        const std::int64_t word = index % mask_words;
        target_masks[target * mask_words + word] = donor_masks[donor_slot * mask_words + word];
    }
}

}  // namespace

void launch_gpu_batch_normalization(const float* input, void* output, const std::int64_t active_batch_size,
                                    const std::int64_t output_batch_size, const int height, const int width,
                                    const GpuPreprocessOutputType output_type, cudaStream_t stream) {
    if (active_batch_size == 0 || output_batch_size == 0) {
        return;
    }
    const std::int64_t groups = output_batch_size * 3 * static_cast<std::int64_t>(height) * ceil_div(width, 4);
    const unsigned int blocks = static_cast<unsigned int>(ceil_div(groups, kThreads));
    switch (output_type) {
        case GpuPreprocessOutputType::Float32:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(
                input, static_cast<float*>(output), active_batch_size, output_batch_size, height, width);
            break;
        case GpuPreprocessOutputType::Float16:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(
                input, static_cast<__half*>(output), active_batch_size, output_batch_size, height, width);
            break;
        case GpuPreprocessOutputType::BFloat16:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(
                input, static_cast<__nv_bfloat16*>(output), active_batch_size, output_batch_size, height, width);
            break;
    }
    ensure_cuda_ok(cudaGetLastError(), "GPU batch normalization kernel launch");
}

void launch_gpu_augmentation_parameters(float* parameters, const std::int64_t batch_size,
                                        const GpuAugmentationLaunchConfig& config, const std::uint64_t seed,
                                        const int epoch, const int rank, const std::uint64_t sequence,
                                        cudaStream_t stream) {
    if (batch_size == 0) {
        return;
    }
    generate_parameters_kernel<<<static_cast<unsigned int>(ceil_div(batch_size, kThreads)), kThreads, 0, stream>>>(
        parameters, batch_size, config, seed, epoch, rank, sequence);
    ensure_cuda_ok(cudaGetLastError(), "GPU augmentation parameter kernel launch");
}

void launch_gpu_augmentation_images(const float* input, float* output, const float* parameters,
                                    const float* copy_paste_parameters, const float* donor_images,
                                    const std::int64_t* donor_masks, const float* donor_boxes,
                                    const std::int64_t donor_mask_words, const std::int64_t batch_size,
                                    const int height, const int width, const GpuAugmentationLaunchConfig&,
                                    const std::uint64_t seed, const int epoch, const int rank,
                                    const std::uint64_t sequence, const bool remap, cudaStream_t stream) {
    if (batch_size == 0) {
        return;
    }
    if (remap) {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * width;
        remap_images_kernel<<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, copy_paste_parameters, donor_images, donor_masks, donor_boxes, donor_mask_words,
            batch_size, height, width, seed, epoch, rank, sequence);
    } else {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * ceil_div(width, 4);
        pointwise_images_kernel<<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, batch_size, height, width, seed, epoch, rank, sequence);
    }
    ensure_cuda_ok(cudaGetLastError(), "GPU augmentation image kernel launch");
}

void launch_gpu_donor_cache_update(const float* source_images, float* donor_images, const std::int64_t* source_masks,
                                   std::int64_t* donor_masks, const std::int64_t* replacement_target_indices,
                                   const std::int64_t batch_size, const std::int64_t pixels_per_image,
                                   const std::int64_t mask_words, cudaStream_t stream) {
    if (batch_size == 0) {
        return;
    }
    const std::int64_t image_values = batch_size * pixels_per_image * 3;
    update_donor_images_kernel<<<static_cast<unsigned int>(ceil_div(image_values, kThreads)), kThreads, 0, stream>>>(
        source_images, donor_images, replacement_target_indices, batch_size, pixels_per_image);
    if (source_masks != nullptr && donor_masks != nullptr && mask_words > 0) {
        const std::int64_t mask_values = batch_size * mask_words;
        update_donor_masks_kernel<<<static_cast<unsigned int>(ceil_div(mask_values, kThreads)), kThreads, 0, stream>>>(
            source_masks, donor_masks, replacement_target_indices, batch_size, mask_words);
    }
    ensure_cuda_ok(cudaGetLastError(), "GPU donor cache update kernel launch");
}

void launch_gpu_copy_cached_masks(const std::int64_t* donor_masks, std::int64_t* target_masks,
                                  const std::int64_t* donor_slots, const std::int64_t* target_indices,
                                  const std::int64_t batch_size, const std::int64_t mask_words, cudaStream_t stream) {
    if (batch_size == 0 || mask_words == 0) {
        return;
    }
    const std::int64_t total = batch_size * mask_words;
    copy_cached_masks_kernel<<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
        donor_masks, target_masks, donor_slots, target_indices, batch_size, mask_words);
    ensure_cuda_ok(cudaGetLastError(), "GPU cached donor mask copy kernel launch");
}

}  // namespace mmltk::rfdetr
