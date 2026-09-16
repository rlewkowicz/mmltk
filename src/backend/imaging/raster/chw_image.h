#pragma once
#include <cstddef>
#include <cstdint>
#include <cuda_runtime_api.h>
namespace mmltk::backend::imaging::raster {
// Unit-range RGB float planes; source and destination retain caller custody
// through completion on the supplied stream.
[[nodiscard]] int chw_float_to_rgba(const float* source, std::uint32_t width, std::uint32_t height, std::uint8_t* destination, std::size_t pitch,
                                    cudaStream_t stream) noexcept;
}  // namespace mmltk::backend::imaging::raster
