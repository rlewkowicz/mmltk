#pragma once
#include <algorithm>
#include <cstdint>
namespace mmltk::backend::imaging::raster {
struct AtlasPaddingColor final {
 std::uint8_t r, g, b, a;
};
inline constexpr AtlasPaddingColor kAtlasPadding{24U, 18U, 35U, 255U};
struct ImageContainRect final {
 std::uint32_t x = 0U, y = 0U, width = 0U, height = 0U;
 bool operator==(const ImageContainRect&) const noexcept = default;
};
[[nodiscard]] inline ImageContainRect contain_image(std::uint32_t source_width, std::uint32_t source_height, std::uint32_t cell_width, std::uint32_t cell_height) noexcept {
 if (!source_width || !source_height || !cell_width || !cell_height) return {};
 auto width = cell_width;
 auto height = cell_height;
 if (static_cast<std::uint64_t>(source_width) * cell_height > static_cast<std::uint64_t>(source_height) * cell_width)
  height = std::max(1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(cell_width) * source_height / source_width));
 else
  width = std::max(1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(cell_height) * source_width / source_height));
 return {(cell_width - width) / 2U, (cell_height - height) / 2U, width, height};
}
}  // namespace mmltk::backend::imaging::raster
