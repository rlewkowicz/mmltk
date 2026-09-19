#include "detail/image_upscaler_nis.h"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "detail/image_upscaler_nis_coefficients.h"
#include "detail/image_upscaler_nis_device.cuh"
namespace mmltk::backend::imaging::upscale::image_upscaler_nis {
namespace device {
extern __device__ __constant__ const DeviceCoefficientTable kScaleCoefficients = coefficients::make_scale();
extern __device__ __constant__ const DeviceCoefficientTable kUsmCoefficients = coefficients::make_usm();
}  // namespace device
namespace {
using device::HalfRgba;
constexpr std::uint32_t kThreads = 256U;
[[nodiscard]] bool checked_elements(const std::uint32_t width, const std::uint32_t height, std::size_t& result) noexcept {
    if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / height) return false;
    result = static_cast<std::size_t>(width) * height;
    return result <= std::numeric_limits<std::size_t>::max() / sizeof(HalfRgba);
}
[[nodiscard]] bool valid_configuration(const Configuration& config) noexcept {
    if (config.source_width == 0U || config.source_height == 0U || config.crop_width == 0U || config.crop_height == 0U || config.output_width == 0U ||
        config.output_height == 0U || config.crop_x >= config.source_width || config.crop_y >= config.source_height ||
        config.crop_width > config.source_width - config.crop_x || config.crop_height > config.source_height - config.crop_y)
        return false;
    std::size_t source_elements = 0U;
    std::size_t horizontal_elements = 0U;
    std::size_t output_elements = 0U;
    if (!checked_elements(config.source_width, config.source_height, source_elements) ||
        !checked_elements(config.output_width, config.crop_height, horizontal_elements) ||
        !checked_elements(config.output_width, config.output_height, output_elements))
        return false;
    constexpr std::uint64_t kMaxLaunchElements = static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max()) * kThreads;
    return horizontal_elements <= kMaxLaunchElements && output_elements <= kMaxLaunchElements;
}
__device__ __forceinline__ float3 source_pixel(const void* source, const std::size_t pitch, const std::uint32_t x, const std::uint32_t y) {
    const auto* row = static_cast<const std::uint8_t*>(source) + static_cast<std::size_t>(y) * pitch;
    const auto* value = row + static_cast<std::size_t>(x) * 4U;
    constexpr float kByteScale = 1.0F / 255.0F;
    return make_float3(static_cast<float>(value[0]) * kByteScale, static_cast<float>(value[1]) * kByteScale, static_cast<float>(value[2]) * kByteScale);
}
__device__ __forceinline__ std::uint32_t nearest_source_coordinate(const std::uint32_t output_coordinate, const std::uint32_t crop_extent,
                                                                   const std::uint32_t output_extent) {
    const std::uint64_t centered = static_cast<std::uint64_t>(output_coordinate) * crop_extent + crop_extent / 2U;
    return min(crop_extent - 1U, static_cast<std::uint32_t>(centered / output_extent));
}
__device__ __forceinline__ std::uint8_t source_alpha_byte(const void* source, const std::size_t pitch, const Configuration config, const std::uint32_t x,
                                                          const std::uint32_t y) {
    const std::uint32_t source_x = config.crop_x + nearest_source_coordinate(x, config.crop_width, config.output_width);
    const std::uint32_t source_y = config.crop_y + nearest_source_coordinate(y, config.crop_height, config.output_height);
    const auto* row = static_cast<const std::uint8_t*>(source) + static_cast<std::size_t>(source_y) * pitch;
    return row[static_cast<std::size_t>(source_x) * 4U + 3U];
}
__global__ void horizontal_kernel(const void* source, const std::size_t source_pitch, HalfRgba* horizontal, const Configuration config) {
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(config.output_width) * config.crop_height;
    if (index >= total) return;
    const std::uint32_t output_x = static_cast<std::uint32_t>(index % config.output_width);
    const std::uint32_t crop_y = static_cast<std::uint32_t>(index / config.output_width);
    const float source_x = (static_cast<float>(output_x) + 0.5F) * static_cast<float>(config.crop_width) / static_cast<float>(config.output_width) - 0.5F;
    const int base = static_cast<int>(floorf(source_x)) - 2;
    const std::uint32_t source_phase = device::phase(source_x);
    float3 color = make_float3(0.0F, 0.0F, 0.0F);
#pragma unroll
    for (std::uint32_t tap = 0U; tap < device::kFilterTaps; ++tap) {
        const std::uint32_t crop_x = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.crop_width) - 1, base + static_cast<int>(tap))));
        const float coefficient = device::kScaleCoefficients.values[source_phase][tap];
        const float3 sample = source_pixel(source, source_pitch, config.crop_x + crop_x, config.crop_y + crop_y);
        color.x = fmaf(coefficient, sample.x, color.x);
        color.y = fmaf(coefficient, sample.y, color.y);
        color.z = fmaf(coefficient, sample.z, color.z);
    }
    horizontal[index] = HalfRgba{__float2half_rn(color.x), __float2half_rn(color.y), __float2half_rn(color.z), __float2half_rn(0.0F)};
}
__device__ __forceinline__ HalfRgba load_clamped(const HalfRgba* pixels, const Configuration config, const int x, const int y) {
    const std::uint32_t clamped_x = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.output_width) - 1, x)));
    const std::uint32_t clamped_y = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.output_height) - 1, y)));
    return pixels[static_cast<std::size_t>(clamped_y) * config.output_width + clamped_x];
}
__global__ void vertical_kernel(const HalfRgba* horizontal, HalfRgba* scaled, const Configuration config) {
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(config.output_width) * config.output_height;
    if (index >= total) return;
    const std::uint32_t output_x = static_cast<std::uint32_t>(index % config.output_width);
    const std::uint32_t output_y = static_cast<std::uint32_t>(index / config.output_width);
    const float source_y = (static_cast<float>(output_y) + 0.5F) * static_cast<float>(config.crop_height) / static_cast<float>(config.output_height) - 0.5F;
    const int base = static_cast<int>(floorf(source_y)) - 2;
    const std::uint32_t source_phase = device::phase(source_y);
    float3 color = make_float3(0.0F, 0.0F, 0.0F);
#pragma unroll
    for (std::uint32_t tap = 0U; tap < device::kFilterTaps; ++tap) {
        const std::uint32_t source_y_index = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.crop_height) - 1, base + static_cast<int>(tap))));
        const HalfRgba sample = horizontal[static_cast<std::size_t>(source_y_index) * config.output_width + output_x];
        const float coefficient = device::kScaleCoefficients.values[source_phase][tap];
        color.x = fmaf(coefficient, __half2float(sample.red), color.x);
        color.y = fmaf(coefficient, __half2float(sample.green), color.y);
        color.z = fmaf(coefficient, __half2float(sample.blue), color.z);
    }
    scaled[index] = HalfRgba{__float2half_rn(color.x), __float2half_rn(color.y), __float2half_rn(color.z), __float2half_rn(0.0F)};
}
__global__ void sharpen_kernel(const void* source, const std::size_t source_pitch, const HalfRgba* scaled, std::uint8_t* target, const std::size_t target_pitch,
                               const Configuration config) {
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(config.output_width) * config.output_height;
    if (index >= total) return;
    const std::uint32_t x = static_cast<std::uint32_t>(index % config.output_width);
    const std::uint32_t y = static_cast<std::uint32_t>(index / config.output_width);
    const HalfRgba center = load_clamped(scaled, config, static_cast<int>(x), static_cast<int>(y));
    const HalfRgba left = load_clamped(scaled, config, static_cast<int>(x) - 1, static_cast<int>(y));
    const HalfRgba right = load_clamped(scaled, config, static_cast<int>(x) + 1, static_cast<int>(y));
    const HalfRgba top = load_clamped(scaled, config, static_cast<int>(x), static_cast<int>(y) - 1);
    const HalfRgba bottom = load_clamped(scaled, config, static_cast<int>(x), static_cast<int>(y) + 1);
    const float source_x = (static_cast<float>(x) + 0.5F) * static_cast<float>(config.crop_width) / static_cast<float>(config.output_width) - 0.5F;
    const float source_y = (static_cast<float>(y) + 0.5F) * static_cast<float>(config.crop_height) / static_cast<float>(config.output_height) - 0.5F;
    const std::uint32_t phase_x = device::phase(source_x);
    const std::uint32_t phase_y = device::phase(source_y);
    const float horizontal_gradient =
        fabsf(0.2126F * (__half2float(left.red) - __half2float(right.red)) + 0.7152F * (__half2float(left.green) - __half2float(right.green)) +
              0.0722F * (__half2float(left.blue) - __half2float(right.blue)));
    const float vertical_gradient =
        fabsf(0.2126F * (__half2float(top.red) - __half2float(bottom.red)) + 0.7152F * (__half2float(top.green) - __half2float(bottom.green)) +
              0.0722F * (__half2float(top.blue) - __half2float(bottom.blue)));
    const bool horizontal = horizontal_gradient <= vertical_gradient;
    const std::uint32_t selected_phase = horizontal ? phase_x : phase_y;
    float3 detail = make_float3(0.0F, 0.0F, 0.0F);
#pragma unroll
    for (std::uint32_t tap = 0U; tap < device::kFilterTaps; ++tap) {
        const int offset = static_cast<int>(tap) - 2;
        const HalfRgba sample = load_clamped(scaled, config, static_cast<int>(x) + (horizontal ? offset : 0),
                                             static_cast<int>(y) + (horizontal ? 0 : offset));
        const float coefficient = device::kUsmCoefficients.values[selected_phase][tap];
        detail.x = fmaf(coefficient, __half2float(sample.red), detail.x);
        detail.y = fmaf(coefficient, __half2float(sample.green), detail.y);
        detail.z = fmaf(coefficient, __half2float(sample.blue), detail.z);
    }
    const float magnitude = fabsf(fmaf(0.2126F, detail.x, fmaf(0.7152F, detail.y, 0.0722F * detail.z)));
    const float strength = 0.18F + 0.16F * fminf(1.0F, magnitude * 8.0F);
    const float4 color = make_float4(fmaf(strength, detail.x, __half2float(center.red)), fmaf(strength, detail.y, __half2float(center.green)),
                                     fmaf(strength, detail.z, __half2float(center.blue)), 0.0F);
    auto* output = target + static_cast<std::size_t>(y) * target_pitch + static_cast<std::size_t>(x) * 4U;
    output[0] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.x)) * 255.0F));
    output[1] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.y)) * 255.0F));
    output[2] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.z)) * 255.0F));
    output[3] = source_alpha_byte(source, source_pitch, config, x, y);
}
}  // namespace
std::optional<ScratchRequirements> scratch_requirements(const Configuration& config) noexcept {
    if (!valid_configuration(config)) return std::nullopt;
    std::size_t horizontal_elements = 0U;
    std::size_t output_elements = 0U;
    if (!checked_elements(config.output_width, config.crop_height, horizontal_elements) ||
        !checked_elements(config.output_width, config.output_height, output_elements))
        return std::nullopt;
    return ScratchRequirements{.horizontal_bytes = horizontal_elements * sizeof(HalfRgba), .scaled_bytes = output_elements * sizeof(HalfRgba)};
}
cudaError_t launch_scale(const void* source, const std::size_t source_pitch, void* horizontal, void* scaled, const Configuration& config,
                         const cudaStream_t stream) noexcept {
    if (source == nullptr || horizontal == nullptr || scaled == nullptr || stream == nullptr || !valid_configuration(config)) return cudaErrorInvalidValue;
    if (source_pitch < static_cast<std::size_t>(config.source_width) * 4U) return cudaErrorInvalidPitchValue;
    // CUDA launch status is host-thread local.  This public algorithm boundary
    // must report its own kernels rather than inherit an inspected error left
    // by an unrelated caller on the same executor thread.
    static_cast<void>(cudaGetLastError());
    const std::uint64_t horizontal_elements = static_cast<std::uint64_t>(config.output_width) * config.crop_height;
    const std::uint64_t output_elements = static_cast<std::uint64_t>(config.output_width) * config.output_height;
    horizontal_kernel<<<static_cast<unsigned int>((horizontal_elements + kThreads - 1U) / kThreads), kThreads, 0U, stream>>>(
        source, source_pitch, static_cast<HalfRgba*>(horizontal), config);
    vertical_kernel<<<static_cast<unsigned int>((output_elements + kThreads - 1U) / kThreads), kThreads, 0U, stream>>>(static_cast<const HalfRgba*>(horizontal),
                                                                                                                       static_cast<HalfRgba*>(scaled), config);
    return cudaPeekAtLastError();
}
cudaError_t launch_sharpen(const void* source, const std::size_t source_pitch, const void* scaled, std::uint8_t* target, const std::size_t target_pitch,
                           const Configuration& config, const cudaStream_t stream) noexcept {
    if (source == nullptr || scaled == nullptr || target == nullptr || stream == nullptr || !valid_configuration(config)) return cudaErrorInvalidValue;
    if (source_pitch < static_cast<std::size_t>(config.source_width) * 4U) return cudaErrorInvalidPitchValue;
    if (target_pitch < static_cast<std::size_t>(config.output_width) * 4U) return cudaErrorInvalidPitchValue;
    static_cast<void>(cudaGetLastError());
    const std::uint64_t output_elements = static_cast<std::uint64_t>(config.output_width) * config.output_height;
    sharpen_kernel<<<static_cast<unsigned int>((output_elements + kThreads - 1U) / kThreads), kThreads, 0U, stream>>>(
        source, source_pitch, static_cast<const HalfRgba*>(scaled), target, target_pitch, config);
    return cudaPeekAtLastError();
}
}  // namespace mmltk::backend::imaging::upscale::image_upscaler_nis
