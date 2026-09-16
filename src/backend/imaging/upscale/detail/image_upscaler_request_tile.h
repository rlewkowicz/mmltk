#pragma once
#include <algorithm>
#include <cstdint>
#include "src/backend/imaging/upscale/upscale_execution.h"
#include "image_upscaler_cuda.h"
namespace mmltk::backend::imaging::upscale::image_upscaler_cuda {
[[nodiscard]] inline Tile prepare_request_tile(const ImageUpscalerRequest& request, const std::uint32_t x, const std::uint32_t y,
                                               const std::uint32_t core_extent, const ImageUpscalerKind kind, const std::uint32_t halo, float* input,
                                               const cudaStream_t stream) {
    const Tile tile{
        .origin_x = x,
        .origin_y = y,
        .core_width = std::min(core_extent, request.crop_width - x),
        .core_height = std::min(core_extent, request.crop_height - y),
    };
    prepare_tile(request.device_pixels, request.source_pitch, request.source_width, request.source_height, request.crop_x, request.crop_y, request.crop_width,
                 request.crop_height, tile, kind == ImageUpscalerKind::ShiftLUT, halo, input, stream);
    return tile;
}
inline void stitch_request_tile(const float* output, const Tile tile, const ImageUpscalerKind kind, const std::uint32_t halo,
                                const ImageUpscalerRequest& request, const std::uint32_t restored_width, const std::uint32_t restored_height,
                                const cudaStream_t stream) {
    stitch_tile(output, tile, kind == ImageUpscalerKind::ShiftLUT, halo, request.target_pixels, request.target_pitch, restored_width, restored_height, stream);
}
}  // namespace mmltk::backend::imaging::upscale::image_upscaler_cuda
