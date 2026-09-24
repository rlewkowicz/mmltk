#pragma once
#include <cstddef>
#include <cstdint>
#include <cuda_runtime_api.h>
namespace mmltk::backend::imaging::raster::detail {
int composite_caption_source(std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t, unsigned, unsigned, cudaStream_t);
struct CaptionCommand final {
 std::uint32_t name, width, color;
 int x, y;
 struct SuffixGlyph final {
  std::uint32_t row, begin, end;
 };
 SuffixGlyph suffix[32]{};
 std::uint32_t suffix_count = 0, name_width = 0;
};
int paint_captions(std::uint8_t*, std::size_t, unsigned, unsigned, const std::uint8_t*, std::size_t, const CaptionCommand*, unsigned, std::uint32_t*, std::size_t, cudaStream_t);
}  // namespace mmltk::backend::imaging::raster::detail
