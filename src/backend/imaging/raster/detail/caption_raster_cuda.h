#pragma once
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::raster::detail {
int composite_caption_source(std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t, unsigned, unsigned, std::uintptr_t);
struct CaptionCommand final {
 std::uint32_t name, width, color;
 int x, y;
};
int paint_captions(std::uint8_t*, std::size_t, unsigned, unsigned, const std::uint8_t*, std::size_t,
 const CaptionCommand*, unsigned, std::uint32_t*, std::size_t, std::uintptr_t);
}
