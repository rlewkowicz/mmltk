#pragma once
#include <cstdint>
namespace mmltk::backend::imaging::annotation::detail {
// CLEANUP-IGNORE: This fixed CUDA mapping ABI is unrelated to the presentation frame-signal layout.
struct ManualMaskMappingAbi {
 // CLEANUP-IGNORE: The mapping ABI's ordered dimensions are distinct kernel parameters, not a reusable scalar row.
 std::uint32_t source_width = 0U;
 std::uint32_t source_height = 0U;
 std::uint32_t source_crop_x = 0U;
 std::uint32_t source_crop_y = 0U;
 std::uint32_t source_crop_width = 0U;
 std::uint32_t source_crop_height = 0U;
 std::uint32_t output_width = 0U;
 std::uint32_t output_height = 0U;
 std::uint32_t view_x = 0U;
 std::uint32_t view_y = 0U;
 [[nodiscard]] constexpr bool operator==(const ManualMaskMappingAbi& other) const noexcept {
  return source_width == other.source_width && source_height == other.source_height && source_crop_x == other.source_crop_x && source_crop_y == other.source_crop_y &&
         source_crop_width == other.source_crop_width && source_crop_height == other.source_crop_height && output_width == other.output_width && output_height == other.output_height &&
         view_x == other.view_x && view_y == other.view_y;
 }
};
}  // namespace mmltk::backend::imaging::annotation::detail
