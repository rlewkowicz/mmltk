#pragma once  // backend.data private implementation boundary
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
namespace mmltk::backend::data::dataset {
struct MaskDimensions {
 std::uint32_t width = 0U;
 std::uint32_t height = 0U;
};
struct RowMajorMaskBounds {
 std::uint32_t min_x = 0U;
 std::uint32_t min_y = 0U;
 std::uint32_t max_x = 0U;
 std::uint32_t max_y = 0U;
 bool has_foreground = false;
};
struct EncodedRowMajorMask {
 std::vector<RLEPair> pairs;
 RowMajorMaskBounds bounds;
};
struct MaskResizeScratch {
 std::vector<std::uint8_t> target_mask;
 std::vector<std::uint32_t> source_x;
 std::vector<std::uint32_t> source_y;
 MaskDimensions lookup_source{};
 std::uint32_t lookup_width = 0U;
 std::uint32_t lookup_height = 0U;
};
// Fills a nearest-center source-coordinate lookup for scaling a mask axis from source_extent to
// target_extent. lookup must already be sized to target_extent.
void fill_center_scale_lookup(std::span<std::uint32_t> lookup, std::uint32_t target_extent, std::uint32_t source_extent, const char* overflow_context);
[[nodiscard]] RowMajorMaskBounds row_major_mask_bounds(std::span<const RLEPair> pairs, MaskDimensions dimensions);
[[nodiscard]] EncodedRowMajorMask encode_dense_row_major_mask(std::span<const std::uint8_t> dense, MaskDimensions dimensions);
void materialize_row_major_mask(std::span<const RLEPair> pairs, MaskDimensions dimensions, std::vector<std::uint8_t>* dense,
                                RowMajorMaskBounds* bounds = nullptr);
[[nodiscard]] EncodedRowMajorMask resize_row_major_mask(std::span<const RLEPair> pairs, MaskDimensions source_dimensions, MaskDimensions target_dimensions,
                                                        const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox, MaskResizeScratch* scratch,
                                                        RowMajorMaskBounds* source_bounds = nullptr);
}  // namespace mmltk::backend::data::dataset
