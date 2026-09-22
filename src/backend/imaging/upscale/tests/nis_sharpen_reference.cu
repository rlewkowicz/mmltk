#include "nis_sharpen_reference.h"
#include "src/backend/imaging/upscale/detail/image_upscaler_nis_device.cuh"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::upscale::tests {
namespace {
namespace device = image_upscaler_nis::device;
using device::HalfRgba;
using image_upscaler_nis::Configuration;
__device__ __constant__ const device::DeviceCoefficientTable kReferenceUsm = image_upscaler_nis::coefficients::make_usm();
__device__ std::uint32_t reference_phase(float coordinate) {
 const float fraction = coordinate - floorf(coordinate);
 return min(device::kPhaseCount - 1U, static_cast<std::uint32_t>(fraction * static_cast<float>(device::kPhaseCount)));
}
__global__ void fixture_pixels(HalfRgba* scaled, Configuration config, unsigned pattern) {
 const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
 if (index >= static_cast<std::uint64_t>(config.output_width) * config.output_height) return;
 const auto x = static_cast<std::uint32_t>(index % config.output_width);
 const auto y = static_cast<std::uint32_t>(index / config.output_width);
 float value = 0.5F;
 if (pattern == 1U) value = ((x + y) % 2U == 0U) ? -0.125F : 1.125F;
 if (pattern == 2U) value = static_cast<float>((x * 31U + y * 17U) % 1024U) / 1024.0F + 1.0F / 4096.0F;
 if (pattern == 3U) value = static_cast<float>((x + y) % 32U) / 32.0F;
 if (pattern == 4U) value = static_cast<float>((x % 2U == 0U ? y * 19U : x * 43U) % 256U) / 255.0F;
 scaled[index] = HalfRgba{__float2half_rn(value), __float2half_rn(pattern == 0U ? value : 1.0F - value), __float2half_rn(value * 0.75F), __float2half_rn(0.25F)};
}
// Independent numerical oracle: the reviewed six-tap chains are both
// evaluated before gradient selection. Do not share production sharpening.
// CLEANUP-IGNORE: Independent numerical oracle retains original alpha-coordinate arithmetic; sharing production code would invalidate the comparison.
__device__ __forceinline__ std::uint32_t nearest_source_coordinate(const std::uint32_t output_coordinate, const std::uint32_t crop_extent, const std::uint32_t output_extent) {
 const std::uint64_t centered = static_cast<std::uint64_t>(output_coordinate) * crop_extent + crop_extent / 2U;
 return min(crop_extent - 1U, static_cast<std::uint32_t>(centered / output_extent));
}
__device__ __forceinline__ std::uint8_t source_alpha_byte(const void* source, const std::size_t pitch, const Configuration config, const std::uint32_t x, const std::uint32_t y) {
 const std::uint32_t source_x = config.crop_x + nearest_source_coordinate(x, config.crop_width, config.output_width);
 const std::uint32_t source_y = config.crop_y + nearest_source_coordinate(y, config.crop_height, config.output_height);
 const auto* row = static_cast<const std::uint8_t*>(source) + static_cast<std::size_t>(source_y) * pitch;
 return row[static_cast<std::size_t>(source_x) * 4U + 3U];
}
__device__ __forceinline__ HalfRgba load_clamped(const HalfRgba* pixels, const Configuration config, const int x, const int y) {
 const std::uint32_t clamped_x = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.output_width) - 1, x)));
 const std::uint32_t clamped_y = static_cast<std::uint32_t>(max(0, min(static_cast<int>(config.output_height) - 1, y)));
 return pixels[static_cast<std::size_t>(clamped_y) * config.output_width + clamped_x];
}
__global__ void dual_direction_kernel(const void* source, const std::size_t source_pitch, const HalfRgba* scaled, std::uint8_t* target, const std::size_t target_pitch, const Configuration config) {
 const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
 // CLEANUP-IGNORE: Independent dual-direction oracle retains original pixel and neighbor selection; do not share production sharpening.
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
 const std::uint32_t phase_x = reference_phase(source_x);
 const std::uint32_t phase_y = reference_phase(source_y);
 float3 horizontal = make_float3(0.0F, 0.0F, 0.0F);
 float3 vertical = make_float3(0.0F, 0.0F, 0.0F);
#pragma unroll
 for (std::uint32_t tap = 0U; tap < device::kFilterTaps; ++tap) {
  const int offset = static_cast<int>(tap) - 2;
  const HalfRgba horizontal_sample = load_clamped(scaled, config, static_cast<int>(x) + offset, static_cast<int>(y));
  const HalfRgba vertical_sample = load_clamped(scaled, config, static_cast<int>(x), static_cast<int>(y) + offset);
  const float horizontal_coefficient = kReferenceUsm.values[phase_x][tap];
  const float vertical_coefficient = kReferenceUsm.values[phase_y][tap];
  horizontal.x = fmaf(horizontal_coefficient, __half2float(horizontal_sample.red), horizontal.x);
  horizontal.y = fmaf(horizontal_coefficient, __half2float(horizontal_sample.green), horizontal.y);
  horizontal.z = fmaf(horizontal_coefficient, __half2float(horizontal_sample.blue), horizontal.z);
  vertical.x = fmaf(vertical_coefficient, __half2float(vertical_sample.red), vertical.x);
  vertical.y = fmaf(vertical_coefficient, __half2float(vertical_sample.green), vertical.y);
  vertical.z = fmaf(vertical_coefficient, __half2float(vertical_sample.blue), vertical.z);
 }
 const float horizontal_gradient = fabsf(
  0.2126F * (__half2float(left.red) - __half2float(right.red)) + 0.7152F * (__half2float(left.green) - __half2float(right.green)) + 0.0722F * (__half2float(left.blue) - __half2float(right.blue)));
 const float vertical_gradient = fabsf(
  0.2126F * (__half2float(top.red) - __half2float(bottom.red)) + 0.7152F * (__half2float(top.green) - __half2float(bottom.green)) + 0.0722F * (__half2float(top.blue) - __half2float(bottom.blue)));
 const float3 detail = horizontal_gradient <= vertical_gradient ? horizontal : vertical;
 // CLEANUP-IGNORE: Independent numerical oracle retains original output arithmetic and rounding; production sharing would make byte equivalence circular.
 const float magnitude = fabsf(fmaf(0.2126F, detail.x, fmaf(0.7152F, detail.y, 0.0722F * detail.z)));
 const float strength = 0.18F + 0.16F * fminf(1.0F, magnitude * 8.0F);
 const float4 color = make_float4(fmaf(strength, detail.x, __half2float(center.red)), fmaf(strength, detail.y, __half2float(center.green)), fmaf(strength, detail.z, __half2float(center.blue)), 0.0F);
 auto* output = target + static_cast<std::size_t>(y) * target_pitch + static_cast<std::size_t>(x) * 4U;
 output[0] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.x)) * 255.0F));
 output[1] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.y)) * 255.0F));
 output[2] = static_cast<std::uint8_t>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, color.z)) * 255.0F));
 output[3] = source_alpha_byte(source, source_pitch, config, x, y);
}
}  // namespace
cudaError_t sharpen_reference(
 const void* source, std::size_t source_pitch, const void* scaled, std::uint8_t* target, std::size_t target_pitch, const image_upscaler_nis::Configuration& config, cudaStream_t stream) {
 const std::uint64_t count = static_cast<std::uint64_t>(config.output_width) * config.output_height;
 dual_direction_kernel<<<static_cast<unsigned int>((count + 255U) / 256U), 256U, 0U, stream>>>(source, source_pitch, static_cast<const HalfRgba*>(scaled), target, target_pitch, config);
 return cudaPeekAtLastError();
}
std::size_t scaled_pixel_bytes() noexcept { return sizeof(HalfRgba); }
cudaError_t prepare_sharpen_pixels(void* scaled, const image_upscaler_nis::Configuration& config, unsigned pattern, cudaStream_t stream) {
 const std::uint64_t count = static_cast<std::uint64_t>(config.output_width) * config.output_height;
 fixture_pixels<<<static_cast<unsigned int>((count + 255U) / 256U), 256U, 0U, stream>>>(static_cast<HalfRgba*>(scaled), config, pattern);
 return cudaPeekAtLastError();
}
}  // namespace mmltk::backend::imaging::upscale::tests
