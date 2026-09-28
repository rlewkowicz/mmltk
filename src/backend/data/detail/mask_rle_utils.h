#pragma once  // backend.data private implementation boundary
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <utility>
#include "src/backend/data/compiled/compiled_format.h"
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
 std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
};
// Accumulates one validated nonempty [begin, end) run at a nonzero width.
// A null output skips geometry; validation and cancellation remain caller-owned.
void include_row_major_mask_run(RowMajorMaskBounds* bounds, std::size_t begin, std::size_t end, std::uint32_t width);
// Optionally derives foreground count during the same validated bounds visit.
[[nodiscard]] RowMajorMaskBounds row_major_mask_bounds(std::span<const RLEPair> pairs, MaskDimensions dimensions, std::uint64_t* foreground = nullptr);
[[nodiscard]] EncodedRowMajorMask encode_dense_row_major_mask(std::span<const std::uint8_t> dense, MaskDimensions dimensions);
void materialize_row_major_mask(std::span<const RLEPair> pairs, MaskDimensions dimensions, std::vector<std::uint8_t>* dense, RowMajorMaskBounds* bounds = nullptr);
// Appends one independent mask, coalescing only within that mask. Failure leaves
// output and source bounds unchanged; empty input leaves source bounds untouched.
[[nodiscard]] RowMajorMaskBounds append_resized_row_major_mask(std::span<const RLEPair>, MaskDimensions source, MaskDimensions target,
 const mmltk::backend::imaging::resample::ImageResizeGeometry&, MaskResizeScratch*, std::vector<RLEPair>& output, RowMajorMaskBounds* source_bounds = nullptr);
}  // namespace mmltk::backend::data::dataset
