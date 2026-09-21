#pragma once
#include <cuda_runtime.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
namespace mmltk::backend::imaging::sampling {
// Normalized closed conservative bounds; all-zero denotes empty support.
using SupportBounds = std::array<float, 4>;
template <typename Run>
[[nodiscard]] SupportBounds rle_support_bounds(std::span<const Run> runs, std::uint32_t width, std::uint32_t height) {
 if (runs.empty()) return {};
 if (width == 0 || height == 0) throw std::invalid_argument("mask support requires positive dimensions");
 SupportBounds bounds{1, 1, 0, 0};
 const float image_width = static_cast<float>(width), image_height = static_cast<float>(height);
 std::uint64_t previous_end = 0;
 for (const auto& run : runs) {
  const auto [start, length] = run;
  const auto end = static_cast<std::uint64_t>(start) + length;
  if (length == 0 || start < previous_end || end > static_cast<std::uint64_t>(width) * height)
   throw std::runtime_error("mask_rle run is empty, overlaps a previous run, or exceeds compiled image bounds");
  previous_end = end;
  const auto first_y = static_cast<std::uint64_t>(start) / width;
  const auto last_y = (end - 1) / width;
  bounds[0] = std::min(bounds[0], first_y == last_y ? static_cast<float>(start % width) / image_width : 0.0F);
  bounds[2] = std::max(bounds[2], first_y == last_y ? static_cast<float>((end - 1) % width + 1) / image_width : 1.0F);
  bounds[1] = std::min(bounds[1], static_cast<float>(first_y) / image_height);
  bounds[3] = std::max(bounds[3], static_cast<float>(last_y + 1) / image_height);
 }
 return bounds;
}
[[nodiscard]] __host__ __device__ inline std::int64_t support_pixel_index(float coordinate, std::int64_t extent) noexcept {
 return static_cast<std::int64_t>(fminf(static_cast<float>(extent - 1), fmaxf(0.0F, nearbyintf(coordinate * static_cast<float>(extent) - 0.5F))));
}
template <typename Run>
[[nodiscard]] __host__ __device__ bool rle_support_contains(const Run* runs, std::size_t count, std::uint64_t pixel) noexcept {
 std::size_t low = 0, high = count;
 while (low < high) {
  const auto middle = low + (high - low) / 2;
  if (runs[middle].start <= pixel)
   low = middle + 1;
  else
   high = middle;
 }
 return low != 0 && pixel - runs[low - 1].start < runs[low - 1].length;
}
}  // namespace mmltk::backend::imaging::sampling
