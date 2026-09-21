#pragma once
#include <cstdint>
#include <limits>
#include <numeric>
namespace mmltk::frameworks::gpu {
struct ImageRegion final {
 std::uint32_t x = 0U;
 std::uint32_t y = 0U;
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
 [[nodiscard]] constexpr bool valid() const noexcept {
  return width != 0U && height != 0U && static_cast<std::uint64_t>(x) + width <= std::numeric_limits<std::uint32_t>::max() &&
         static_cast<std::uint64_t>(y) + height <= std::numeric_limits<std::uint32_t>::max();
 }
 constexpr bool operator==(const ImageRegion&) const noexcept = default;
};
struct ImageAxisScale final {
 std::uint32_t backing_pixels = 1U;
 std::uint32_t original_pixels = 1U;
 [[nodiscard]] static constexpr ImageAxisScale From(const std::uint32_t backing, const std::uint32_t original) noexcept {
  if (backing == 0U || original == 0U) return {};
  const std::uint32_t divisor = std::gcd(backing, original);
  return {
   .backing_pixels = backing / divisor,
   .original_pixels = original / divisor,
  };
 }
 [[nodiscard]] constexpr bool valid() const noexcept {
  return backing_pixels != 0U && original_pixels != 0U && std::gcd(backing_pixels, original_pixels) == 1U;
 }
 constexpr bool operator==(const ImageAxisScale&) const noexcept = default;
};
struct ImageGeometry final {
 std::uint32_t backing_width = 0U;
 std::uint32_t backing_height = 0U;
 ImageRegion original_content{};
 ImageAxisScale scale_x{};
 ImageAxisScale scale_y{};
 [[nodiscard]] constexpr bool valid() const noexcept {
  return backing_width != 0U && backing_height != 0U && original_content.width != 0U && original_content.height != 0U && scale_x.valid() && scale_y.valid() &&
         static_cast<std::uint64_t>(original_content.width) * scale_x.backing_pixels == static_cast<std::uint64_t>(backing_width) * scale_x.original_pixels &&
         static_cast<std::uint64_t>(original_content.height) * scale_y.backing_pixels == static_cast<std::uint64_t>(backing_height) * scale_y.original_pixels;
 }
 constexpr bool operator==(const ImageGeometry&) const noexcept = default;
};
}  // namespace mmltk::frameworks::gpu
