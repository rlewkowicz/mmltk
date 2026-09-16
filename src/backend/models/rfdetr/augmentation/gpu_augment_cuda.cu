#include "support_sampling.h"
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cmath>
#include <cstdint>
#include "detail/gpu_augment_cuda_launch.h"
#include "detail/gpu_augment_plan_math.h"
#include "src/frameworks/gpu/cuda_error.h"
namespace mmltk::backend::models::rfdetr {
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
constexpr int kThreads = 256;
using augment_math::clamp01;
using augment_math::image_key;
using augment_math::uniform01;
using enum augment_math::ParameterIndex;
enum CopyPasteParameterIndex : std::uint8_t {
    kPasteDonorSlot = 0,
    kPasteMode = 1,
    kPasteInverse00 = 2,
    kPasteInverse01 = 3,
    kPasteInverse02 = 4,
    kPasteInverse10 = 5,
    kPasteInverse11 = 6,
    kPasteInverse12 = 7,
};
__host__ __device__ constexpr std::int64_t ceil_div(const std::int64_t value, const std::int64_t divisor) { return (value + divisor - 1) / divisor; }
__device__ __forceinline__ float channel_mean(const int channel) { return channel == 0 ? 0.485F : (channel == 1 ? 0.456F : 0.406F); }
__device__ __forceinline__ float channel_std(const int channel) { return channel == 0 ? 0.229F : (channel == 1 ? 0.224F : 0.225F); }
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
__global__ void normalize_images_kernel(const float* input, Output* output, const std::int64_t active_batch_size, const std::int64_t output_batch_size,
                                        const int height, const int width) {
    const int groups_per_row = static_cast<int>(ceil_div(width, 4));
    const std::int64_t group_index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total_groups = output_batch_size * 3 * static_cast<std::int64_t>(height) * groups_per_row;
    if (group_index >= total_groups) { return; }
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
                if (x + lane < width) { lanes[lane] = input[offset + lane]; }
            }
        }
    }
    const float* input_values = reinterpret_cast<const float*>(&values);
#pragma unroll
    for (int lane = 0; lane < 4; ++lane) {
        if (x + lane >= width) { break; }
        output[offset + lane] = active ? normalized_output<Output>(input_values[lane], channel) : zero_output<Output>();
    }
}
__device__ __forceinline__ float approximate_gaussian(const std::uint64_t key, const std::uint64_t base) {
    return uniform01(key, base) + uniform01(key, base + 1) + uniform01(key, base + 2) + uniform01(key, base + 3) - 2.0F;
}
__device__ __forceinline__ void apply_effects(float& red, float& green, float& blue, const float* values, const std::uint64_t key,
                                              const std::int64_t pixel_index, const int output_x, const int output_y, const int width, const int height,
                                              const GpuAugmentationOutputDomain output_domain) {
    const float input_red = red;
    const float input_green = green;
    const float input_blue = blue;
    red = values[kColorMatrix + 0] * input_red + values[kColorMatrix + 1] * input_green + values[kColorMatrix + 2] * input_blue + values[kColorOffset + 0];
    green = values[kColorMatrix + 3] * input_red + values[kColorMatrix + 4] * input_green + values[kColorMatrix + 5] * input_blue + values[kColorOffset + 1];
    blue = values[kColorMatrix + 6] * input_red + values[kColorMatrix + 7] * input_green + values[kColorMatrix + 8] * input_blue + values[kColorOffset + 2];
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
    const auto erasure = augment_math::spatial_erasure(values, key);
    const bool erase_pixel = augment_math::erases_pixel(erasure, output_x, output_y, width, height);
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
    red = clamp01(red);
    green = clamp01(green);
    blue = clamp01(blue);
    if (output_domain == GpuAugmentationOutputDomain::ModelNormalized) {
        red = (red - channel_mean(0)) / channel_std(0);
        green = (green - channel_mean(1)) / channel_std(1);
        blue = (blue - channel_mean(2)) / channel_std(2);
    }
}
__device__ __forceinline__ float4 reverse_float4(const float4 value) { return make_float4(value.w, value.z, value.y, value.x); }
template <bool ExplicitKeys>
__global__ void pointwise_images_kernel(const float* input, float* output, const float* parameters, const std::uint64_t* image_keys,
                                        const std::int64_t batch_size, const int height, const int width, const std::uint64_t seed, const int epoch,
                                        const int rank, const std::uint64_t sequence, const GpuAugmentationOutputDomain output_domain,
                                        const float* const* input_slots) {
    const int groups_per_row = static_cast<int>(ceil_div(width, 4));
    const std::int64_t group_index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t total_groups = batch_size * static_cast<std::int64_t>(height) * groups_per_row;
    if (group_index >= total_groups) { return; }
    const int group_x = static_cast<int>(group_index % groups_per_row);
    const std::int64_t row_index = group_index / groups_per_row;
    const int output_y = static_cast<int>(row_index % height);
    const std::int64_t image = row_index / height;
    const auto source_image = input_slots ? 0 : image;
    if (input_slots) input = input_slots[image];
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
            const std::int64_t source_offset = ((source_image * 3 + channel) * height + source_y) * static_cast<std::int64_t>(width) + source_x;
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
                const std::int64_t source_offset = ((source_image * 3 + channel) * height + source_y) * static_cast<std::int64_t>(width) + source_x;
                channel_values[lane] = input[source_offset];
            }
        }
    }
    std::uint64_t key = 0U;
    if constexpr (ExplicitKeys) {
        key = image_keys[image];
    } else {
        key = image_key(seed, epoch, rank, sequence, image);
    }
#pragma unroll
    for (int lane = 0; lane < 4; ++lane) {
        const int x = output_x + lane;
        if (x >= width) { continue; }
        float* red_values = reinterpret_cast<float*>(&channels[0]);
        float* green_values = reinterpret_cast<float*>(&channels[1]);
        float* blue_values = reinterpret_cast<float*>(&channels[2]);
        const std::int64_t pixel_index = static_cast<std::int64_t>(output_y) * width + x;
        apply_effects(red_values[lane], green_values[lane], blue_values[lane], values, key, pixel_index, x, output_y, width, height, output_domain);
    }
    if (vector_aligned) {
#pragma unroll
        for (int channel = 0; channel < 3; ++channel) {
            const std::int64_t output_offset = ((image * 3 + channel) * height + output_y) * static_cast<std::int64_t>(width) + output_x;
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
                    const std::int64_t output_offset = ((image * 3 + channel) * height + output_y) * static_cast<std::int64_t>(width) + x;
                    output[output_offset] = channel_values[lane];
                }
            }
        }
    }
}
__device__ __forceinline__ float remap_channel(const float* input, const std::int64_t plane_offset, const int height, const int width, const float source_x,
                                               const float source_y, const float blur_strength, const int channel, const std::int64_t row_stride) {
    if (source_x < 0.0F || source_x > 1.0F || source_y < 0.0F || source_y > 1.0F) { return channel_mean(channel); }
    const float pixel_x = fminf(fmaxf(source_x * static_cast<float>(width) - 0.5F, 0.0F), static_cast<float>(width - 1));
    const float pixel_y = fminf(fmaxf(source_y * static_cast<float>(height) - 0.5F, 0.0F), static_cast<float>(height - 1));
    const int x0 = static_cast<int>(floorf(pixel_x));
    const int y0 = static_cast<int>(floorf(pixel_y));
    const int x1 = min(width - 1, x0 + 1);
    const int y1 = min(height - 1, y0 + 1);
    const float fraction_x = pixel_x - static_cast<float>(x0);
    const float fraction_y = pixel_y - static_cast<float>(y0);
    const float value00 = input[plane_offset + static_cast<std::int64_t>(y0) * row_stride + x0];
    const float value01 = input[plane_offset + static_cast<std::int64_t>(y0) * row_stride + x1];
    const float value10 = input[plane_offset + static_cast<std::int64_t>(y1) * row_stride + x0];
    const float value11 = input[plane_offset + static_cast<std::int64_t>(y1) * row_stride + x1];
    const float top = fmaf(fraction_x, value01 - value00, value00);
    const float bottom = fmaf(fraction_x, value11 - value10, value10);
    const float bilinear = fmaf(fraction_y, bottom - top, top);
    const float box = (value00 + value01 + value10 + value11) * 0.25F;
    return fmaf(blur_strength, box - bilinear, bilinear);
}
__device__ __forceinline__ bool packed_mask_contains(const std::int64_t* packed_masks, const std::int64_t words_per_mask, const int slot, const int height,
                                                     const int width, const float source_x, const float source_y) {
    if (packed_masks == nullptr || source_x < 0.0F || source_x > 1.0F || source_y < 0.0F || source_y > 1.0F) { return false; }
    const int x = static_cast<int>(augment_math::support_pixel_index(source_x, width));
    const int y = static_cast<int>(augment_math::support_pixel_index(source_y, height));
    const std::int64_t pixel = static_cast<std::int64_t>(y) * width + x;
    const auto* words = reinterpret_cast<const unsigned long long*>(packed_masks + slot * words_per_mask);
    return ((words[pixel >> 6] >> (pixel & 63)) & 1ULL) != 0ULL;
}
template <bool ExplicitKeys>
__global__ void remap_images_kernel(const float* input, float* output, const float* parameters, const float* copy_paste_parameters, const float* donor_images,
                                    const std::int64_t* donor_masks, const float* donor_boxes, const std::int64_t donor_mask_words,
                                    const std::uint64_t* image_keys, const std::int64_t batch_size, const int height, const int width, const std::uint64_t seed,
                                    const int epoch, const int rank, const std::uint64_t sequence, const GpuAugmentationOutputDomain output_domain,
                                    const float* const* input_slots, const float* const* donor_slots, const GpuAugmentationPreparedView* prepared) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t pixels_per_image = static_cast<std::int64_t>(height) * width;
    const std::int64_t total = batch_size * pixels_per_image;
    if (index >= total) { return; }
    const std::int64_t image = index / pixels_per_image;
    const auto source_image = input_slots ? 0 : image;
    if (input_slots) input = input_slots[image];
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
        const std::int64_t plane_offset = (source_image * 3 + channel) * pixels_per_image;
        const auto view = prepared ? prepared[image * 2] : GpuAugmentationPreparedView{input, width, height, width, pixels_per_image};
        channels[channel] = remap_channel(view.pixels, prepared ? channel * view.plane_stride : plane_offset, view.height, view.width, source_x, source_y,
                                          blur_strength, channel, view.row_stride);
    }
    if (copy_paste_parameters != nullptr && (donor_images != nullptr || donor_slots != nullptr)) {
        const float* paste = copy_paste_parameters + image * kGpuCopyPasteParameterCount;
        const int donor_slot = static_cast<int>(paste[kPasteDonorSlot]);
        if (donor_slot >= 0) {
            const float donor_x = paste[kPasteInverse00] * normalized_x + paste[kPasteInverse01] * normalized_y + paste[kPasteInverse02];
            const float donor_y = paste[kPasteInverse10] * normalized_x + paste[kPasteInverse11] * normalized_y + paste[kPasteInverse12];
            const int mode = static_cast<int>(paste[kPasteMode]);
            bool paste_pixel = false;
            if (mode == 1) {
                paste_pixel = packed_mask_contains(donor_masks, donor_mask_words, donor_slot, height, width, donor_x, donor_y);
            } else if (mode == 2 && donor_boxes != nullptr) {
                const float* box = donor_boxes + static_cast<std::size_t>(donor_slot) * 4U;
                paste_pixel = donor_x >= box[0] && donor_x <= box[2] && donor_y >= box[1] && donor_y <= box[3];
            }
            if (paste_pixel) {
#pragma unroll
                for (int channel = 0; channel < 3; ++channel) {
                    const std::int64_t donor_plane = ((donor_slots ? 0 : static_cast<std::int64_t>(donor_slot)) * 3 + channel) * pixels_per_image;
                    const auto view =
                        prepared ? prepared[image * 2 + 1]
                                 : GpuAugmentationPreparedView{donor_slots ? donor_slots[donor_slot] : donor_images, width, height, width, pixels_per_image};
                    channels[channel] = remap_channel(view.pixels, prepared ? channel * view.plane_stride : donor_plane, view.height, view.width, donor_x,
                                                      donor_y, blur_strength, channel, view.row_stride);
                }
            }
        }
    }
    std::uint64_t key = 0U;
    if constexpr (ExplicitKeys) {
        key = image_keys[image];
    } else {
        key = image_key(seed, epoch, rank, sequence, image);
    }
    apply_effects(channels[0], channels[1], channels[2], values, key, pixel_index, output_x, output_y, width, height, output_domain);
#pragma unroll
    for (int channel = 0; channel < 3; ++channel) { output[(image * 3 + channel) * pixels_per_image + pixel_index] = channels[channel]; }
}
__global__ void update_donor_images_kernel(const float* source_images, float* donor_images, const std::int64_t* source_ordinals, const std::int64_t batch_size,
                                           const std::int64_t pixels_per_image) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::int64_t values_per_image = pixels_per_image * 3;
    const std::int64_t total = batch_size * values_per_image;
    if (index >= total) { return; }
    const std::int64_t image = index / values_per_image;
    if (source_ordinals[image] >= 0) { donor_images[index] = source_images[index]; }
}
__global__ void rgba8_to_planar_float_kernel(const std::uint8_t* input, float* output, const std::int64_t image_pixels, const std::int64_t pixels) {
    const std::int64_t index = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= pixels) { return; }
    const std::int64_t image = index / image_pixels;
    const std::int64_t pixel = index - image * image_pixels;
    const std::int64_t input_offset = index * 4;
    const std::int64_t output_offset = image * 3 * image_pixels + pixel;
    constexpr float kInverse255 = 1.0F / 255.0F;
    output[output_offset] = static_cast<float>(input[input_offset]) * kInverse255;
    output[output_offset + image_pixels] = static_cast<float>(input[input_offset + 1]) * kInverse255;
    output[output_offset + image_pixels * 2] = static_cast<float>(input[input_offset + 2]) * kInverse255;
}
}  // namespace
void launch_gpu_rgba8_to_planar_float(const std::uint8_t* input, float* output, const std::int64_t batch_size, const int height, const int width,
                                      cudaStream_t stream) {
    if (batch_size == 0) { return; }
    const std::int64_t image_pixels = static_cast<std::int64_t>(height) * width;
    const std::int64_t pixels = batch_size * image_pixels;
    const unsigned int blocks = static_cast<unsigned int>(ceil_div(pixels, kThreads));
    rgba8_to_planar_float_kernel<<<blocks, kThreads, 0, stream>>>(input, output, image_pixels, pixels);
    ensure_cuda_ok(cudaGetLastError(), "RGBA8 augmentation input conversion launch");
}
void launch_gpu_batch_normalization(const float* input, void* output, const std::int64_t active_batch_size, const std::int64_t output_batch_size,
                                    const int height, const int width, const GpuPreprocessOutputType output_type, cudaStream_t stream) {
    if (active_batch_size == 0 || output_batch_size == 0) { return; }
    const std::int64_t groups = output_batch_size * 3 * static_cast<std::int64_t>(height) * ceil_div(width, 4);
    const unsigned int blocks = static_cast<unsigned int>(ceil_div(groups, kThreads));
    switch (output_type) {
        case GpuPreprocessOutputType::Float32:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(input, static_cast<float*>(output), active_batch_size, output_batch_size, height, width);
            break;
        case GpuPreprocessOutputType::Float16:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(input, static_cast<__half*>(output), active_batch_size, output_batch_size, height, width);
            break;
        case GpuPreprocessOutputType::BFloat16:
            normalize_images_kernel<<<blocks, kThreads, 0, stream>>>(input, static_cast<__nv_bfloat16*>(output), active_batch_size, output_batch_size, height,
                                                                     width);
            break;
    }
    ensure_cuda_ok(cudaGetLastError(), "GPU batch normalization launch");
}
void launch_gpu_augmentation_images(const float* input, float* output, const float* parameters, const float* copy_paste_parameters, const float* donor_images,
                                    const std::int64_t* donor_masks, const float* donor_boxes, const std::int64_t donor_mask_words,
                                    const std::int64_t batch_size, const int height, const int width, const GpuAugmentationLaunchConfig&,
                                    const std::uint64_t seed, const int epoch, const int rank, const std::uint64_t sequence, const bool remap,
                                    const GpuAugmentationOutputDomain output_domain, cudaStream_t stream, const float* const* input_slots,
                                    const float* const* donor_slots, const GpuAugmentationPreparedView* prepared) {
    if (batch_size == 0) { return; }
    if (remap) {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * width;
        remap_images_kernel<false><<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, copy_paste_parameters, donor_images, donor_masks, donor_boxes, donor_mask_words, nullptr, batch_size, height, width,
            seed, epoch, rank, sequence, output_domain, input_slots, donor_slots, prepared);
    } else {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * ceil_div(width, 4);
        pointwise_images_kernel<false><<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, nullptr, batch_size, height, width, seed, epoch, rank, sequence, output_domain, input_slots);
    }
    ensure_cuda_ok(cudaGetLastError(), "GPU augmentation image launch");
}
void launch_gpu_augmentation_images_explicit(const float* input, float* output, const float* parameters, const float* copy_paste_parameters,
                                             const float* donor_images, const std::int64_t* donor_masks, const float* donor_boxes,
                                             const std::int64_t donor_mask_words, const std::uint64_t* image_keys, const std::int64_t batch_size,
                                             const int height, const int width, const GpuAugmentationLaunchConfig&, const bool remap,
                                             const GpuAugmentationOutputDomain output_domain, cudaStream_t stream, const float* const* input_slots,
                                             const float* const* donor_slots, const GpuAugmentationPreparedView* prepared) {
    if (batch_size == 0) { return; }
    if (remap) {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * width;
        remap_images_kernel<true><<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, copy_paste_parameters, donor_images, donor_masks, donor_boxes, donor_mask_words, image_keys, batch_size, height, width,
            0U, 0, 0, 0U, output_domain, input_slots, donor_slots, prepared);
    } else {
        const std::int64_t total = batch_size * static_cast<std::int64_t>(height) * ceil_div(width, 4);
        pointwise_images_kernel<true><<<static_cast<unsigned int>(ceil_div(total, kThreads)), kThreads, 0, stream>>>(
            input, output, parameters, image_keys, batch_size, height, width, 0U, 0, 0, 0U, output_domain, input_slots);
    }
    ensure_cuda_ok(cudaGetLastError(), "explicit GPU augmentation image launch");
}
void update_gpu_augmentation_donor_cache(const float* source_images, float* donor_images, const std::int64_t* source_ordinals, const std::int64_t batch_size,
                                         const std::int64_t pixels_per_image, cudaStream_t stream) {
    if (batch_size == 0) { return; }
    const std::int64_t image_values = batch_size * pixels_per_image * 3;
    update_donor_images_kernel<<<static_cast<unsigned int>(ceil_div(image_values, kThreads)), kThreads, 0, stream>>>(
        source_images, donor_images, source_ordinals, batch_size, pixels_per_image);
    ensure_cuda_ok(cudaGetLastError(), "GPU augmentation donor image cache launch");
}
}  // namespace mmltk::backend::models::rfdetr
