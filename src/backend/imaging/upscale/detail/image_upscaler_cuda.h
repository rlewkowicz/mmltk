#pragma once

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mmltk::backend::imaging::upscale::image_upscaler_cuda {

struct Tile {
    std::uint32_t origin_x = 0U;
    std::uint32_t origin_y = 0U;
    std::uint32_t core_width = 0U;
    std::uint32_t core_height = 0U;
};

void prepare_tile(const float* source, std::uint32_t source_width, std::uint32_t source_height, std::uint32_t crop_x, std::uint32_t crop_y,
                  std::uint32_t crop_width, std::uint32_t crop_height, Tile tile, bool shift_lut, std::uint32_t halo, float* input,
                  cudaStream_t stream);

void stitch_tile(const float* output, Tile tile, bool shift_lut, std::uint32_t halo, float* restored, std::uint32_t restored_width,
                 std::uint32_t restored_height, cudaStream_t stream);

void rgba8_to_normalized_nchw(const std::uint8_t* source, std::size_t source_pitch, std::uint32_t width, std::uint32_t height,
                              float* target, cudaStream_t stream);
void normalized_nchw_to_rgba8(const float* source, std::uint32_t width, std::uint32_t height, std::uint8_t* target,
                              std::size_t target_pitch, cudaStream_t stream);
}  // namespace mmltk::backend::imaging::upscale::image_upscaler_cuda
