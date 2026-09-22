#pragma once
#include "src/backend/imaging/upscale/detail/image_upscaler_nis.h"
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::upscale::tests {
[[nodiscard]] cudaError_t sharpen_reference(const void*, std::size_t, const void*, std::uint8_t*, std::size_t, const image_upscaler_nis::Configuration&, cudaStream_t);
[[nodiscard]] std::size_t scaled_pixel_bytes() noexcept;
[[nodiscard]] cudaError_t prepare_sharpen_pixels(void*, const image_upscaler_nis::Configuration&, unsigned pattern, cudaStream_t);
}  // namespace mmltk::backend::imaging::upscale::tests
