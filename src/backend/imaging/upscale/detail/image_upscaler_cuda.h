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
void prepare_tile(const std::uint8_t* source, std::size_t source_pitch, std::uint32_t source_width, std::uint32_t source_height, std::uint32_t crop_x, std::uint32_t crop_y, std::uint32_t crop_width,
 std::uint32_t crop_height, Tile tile, bool shift_lut, std::uint32_t halo, float* input, cudaStream_t stream);
void stitch_tile(const float* output, Tile tile, bool shift_lut, std::uint32_t halo, std::uint8_t* restored, std::size_t restored_pitch, std::uint32_t restored_width, std::uint32_t restored_height,
 cudaStream_t stream);
}  // namespace mmltk::backend::imaging::upscale::image_upscaler_cuda
