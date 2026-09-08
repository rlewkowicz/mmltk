#include "detail/image_upscaler_cuda.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace mmltk::backend::imaging::upscale::image_upscaler_cuda {

namespace {

constexpr std::uint32_t kInputExtent = 256U;
constexpr std::uint32_t kOutputExtent = kInputExtent * 4U;

__device__ __forceinline__ float channel_mean(const std::uint32_t channel) {
    return channel == 0U ? 0.485F : (channel == 1U ? 0.456F : 0.406F);
}

__device__ __forceinline__ float channel_std(const std::uint32_t channel) {
    return channel == 0U ? 0.229F : (channel == 1U ? 0.224F : 0.225F);
}

__device__ __forceinline__ std::uint32_t reflect_coordinate(const int coordinate, const std::uint32_t extent) {
    if (extent <= 1U) { return 0U; }
    const int period = static_cast<int>((extent - 1U) * 2U);
    int reflected = coordinate % period;
    if (reflected < 0) { reflected += period; }
    if (reflected >= static_cast<int>(extent)) { reflected = period - reflected; }
    return static_cast<std::uint32_t>(reflected);
}

__global__ void prepare_tile_kernel(const std::uint8_t* source, const std::size_t source_pitch, const std::uint32_t source_width, const std::uint32_t source_height,
                                    const std::uint32_t crop_x, const std::uint32_t crop_y, const std::uint32_t crop_width,
                                    const std::uint32_t crop_height, const Tile tile, const bool shift_lut, const std::uint32_t halo,
                                    float* input) {
    const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    constexpr std::uint32_t kPlane = kInputExtent * kInputExtent;
    if (index >= kPlane * 3U) { return; }
    const std::uint32_t channel = index / kPlane;
    const std::uint32_t local = index - channel * kPlane;
    const int content_x = static_cast<int>(tile.origin_x) + static_cast<int>(local % kInputExtent) - static_cast<int>(halo);
    const int content_y = static_cast<int>(tile.origin_y) + static_cast<int>(local / kInputExtent) - static_cast<int>(halo);
    const std::uint32_t source_x = min(source_width - 1U, crop_x + reflect_coordinate(content_x, crop_width));
    const std::uint32_t source_y = min(source_height - 1U, crop_y + reflect_coordinate(content_y, crop_height));
    const float byte = static_cast<float>(source[static_cast<std::size_t>(source_y) * source_pitch +
                                                static_cast<std::size_t>(source_x) * 4U + channel]);
    // Retain NVCC's contracted multiply/subtract from the former normalization
    // kernel, the rounded division, and the restoration FMA. Replacing this
    // round trip with byte RGB changes decisions at LUT thresholds.
    const float normalized = __fdiv_rn(__fmaf_rn(byte, 1.0F / 255.0F, -channel_mean(channel)), channel_std(channel));
    const float rgb = fminf(1.0F, fmaxf(0.0F, fmaf(normalized, channel_std(channel), channel_mean(channel))));
    input[index] = shift_lut ? rgb * 255.0F : rgb;
}

__global__ void stitch_tile_kernel(const float* output, const Tile tile, const bool shift_lut, const std::uint32_t halo, std::uint8_t* restored,
                                   const std::size_t restored_pitch,
                                   const std::uint32_t restored_width, const std::uint32_t restored_height) {
    const std::uint32_t core_output_width = tile.core_width * 4U;
    const std::uint32_t core_output_height = tile.core_height * 4U;
    const std::uint64_t core_plane = static_cast<std::uint64_t>(core_output_width) * core_output_height;
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= core_plane * 3U) { return; }
    const std::uint32_t channel = static_cast<std::uint32_t>(index / core_plane);
    const std::uint64_t local = index - static_cast<std::uint64_t>(channel) * core_plane;
    const std::uint32_t local_x = static_cast<std::uint32_t>(local % core_output_width);
    const std::uint32_t local_y = static_cast<std::uint32_t>(local / core_output_width);
    const std::uint32_t destination_x = tile.origin_x * 4U + local_x;
    const std::uint32_t destination_y = tile.origin_y * 4U + local_y;
    if (destination_x >= restored_width || destination_y >= restored_height) { return; }
    constexpr std::size_t kOutputPlane = static_cast<std::size_t>(kOutputExtent) * kOutputExtent;
    const std::size_t scaled_halo = static_cast<std::size_t>(halo) * 4U;
    const std::size_t source = static_cast<std::size_t>(channel) * kOutputPlane +
                               (static_cast<std::size_t>(local_y) + scaled_halo) * kOutputExtent + static_cast<std::size_t>(local_x) +
                               scaled_halo;
    const float scale = shift_lut ? (1.0F / 255.0F) : 1.0F;
    const float value = fminf(1.0F, fmaxf(0.0F, output[source] * scale));
    auto* rgba = restored + static_cast<std::size_t>(destination_y) * restored_pitch +
                 static_cast<std::size_t>(destination_x) * 4U;
    rgba[channel] = static_cast<std::uint8_t>(__float2uint_rn(value * 255.0F));
    if (channel == 0U) rgba[3] = 255U;
}

}  // namespace

void prepare_tile(const std::uint8_t* source, const std::size_t source_pitch, const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t crop_x,
                  const std::uint32_t crop_y, const std::uint32_t crop_width, const std::uint32_t crop_height, const Tile tile,
                  const bool shift_lut, const std::uint32_t halo, float* input, const cudaStream_t stream) {
    constexpr std::uint32_t kElements = kInputExtent * kInputExtent * 3U;
    constexpr std::uint32_t kThreads = 256U;
    prepare_tile_kernel<<<(kElements + kThreads - 1U) / kThreads, kThreads, 0U, stream>>>(
        source, source_pitch, source_width, source_height, crop_x, crop_y, crop_width, crop_height, tile, shift_lut, halo, input);
}

void stitch_tile(const float* output, const Tile tile, const bool shift_lut, const std::uint32_t halo, std::uint8_t* restored,
                 const std::size_t restored_pitch,
                 const std::uint32_t restored_width, const std::uint32_t restored_height, const cudaStream_t stream) {
    const std::uint64_t elements =
        static_cast<std::uint64_t>(tile.core_width) * 4U * static_cast<std::uint64_t>(tile.core_height) * 4U * 3U;
    constexpr std::uint32_t kThreads = 256U;
    stitch_tile_kernel<<<static_cast<unsigned int>((elements + kThreads - 1U) / kThreads), kThreads, 0U, stream>>>(
        output, tile, shift_lut, halo, restored, restored_pitch, restored_width, restored_height);
}

}  // namespace mmltk::backend::imaging::upscale::image_upscaler_cuda
