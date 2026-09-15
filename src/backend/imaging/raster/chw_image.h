#pragma once
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::raster {
// Unit-range RGB float planes; source and destination retain caller custody
// through completion on the supplied stream.
[[nodiscard]] int chw_float_to_rgba(const float* source, std::uint32_t width, std::uint32_t height,
                                     std::uint8_t* destination, std::size_t pitch, std::uintptr_t stream) noexcept;
}
