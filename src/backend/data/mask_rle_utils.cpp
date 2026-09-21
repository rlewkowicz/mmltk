#include "detail/mask_rle_utils.h"
#include <immintrin.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <span>
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::dataset {
using mmltk::common::math::checked_cast;
void fill_center_scale_lookup(const std::span<std::uint32_t> lookup, const std::uint32_t target_extent, const std::uint32_t source_extent,
                              const char* overflow_context) {
 const std::uint64_t target_twice = static_cast<std::uint64_t>(2U) * target_extent;
 for (std::uint32_t index = 0U; index < target_extent; ++index) {
  lookup[index] = std::min<std::uint32_t>(
   source_extent - 1U, checked_cast<std::uint32_t>(((static_cast<std::uint64_t>(2U) * index + 1U) * source_extent) / target_twice, overflow_context));
 }
}
namespace {
[[nodiscard]] std::size_t checked_pixel_count(const MaskDimensions dimensions) {
 if (dimensions.width == 0U || dimensions.height == 0U || dimensions.width > std::numeric_limits<std::size_t>::max() / dimensions.height) {
  throw std::runtime_error("mask dimensions are invalid");
 }
 return static_cast<std::size_t>(dimensions.width) * dimensions.height;
}
void include_run(RowMajorMaskBounds* bounds, const std::size_t begin, const std::size_t end, const std::uint32_t width) {
 if (bounds == nullptr) { return; }
 const std::uint32_t begin_y = checked_cast<std::uint32_t>(begin / width, "mask run y overflow");
 const std::uint32_t begin_x = checked_cast<std::uint32_t>(begin % width, "mask run x overflow");
 const std::size_t final = end - 1U;
 // CLEANUP-IGNORE: Run start and inclusive final pixel have distinct bound roles; retain explicit quotient and remainder coordinates.
 const std::uint32_t end_y = checked_cast<std::uint32_t>(final / width, "mask run y overflow");
 const std::uint32_t end_x = checked_cast<std::uint32_t>(final % width, "mask run x overflow");
 if (!bounds->has_foreground) {
  bounds->min_x = begin_x;
  bounds->min_y = begin_y;
  bounds->max_x = end_x + 1U;
  bounds->max_y = end_y + 1U;
  bounds->has_foreground = true;
 } else {
  bounds->min_y = std::min(bounds->min_y, begin_y);
  bounds->max_y = std::max(bounds->max_y, end_y + 1U);
  bounds->min_x = std::min(bounds->min_x, begin_x);
  bounds->max_x = std::max(bounds->max_x, end_x + 1U);
 }
 if (begin_y != end_y) {
  bounds->min_x = 0U;
  bounds->max_x = width;
 }
}
void prepare_lookup(const MaskDimensions source, const std::uint32_t width, const std::uint32_t height, MaskResizeScratch* scratch) {
 if (scratch->lookup_source.width == source.width && scratch->lookup_source.height == source.height && scratch->lookup_width == width &&
     scratch->lookup_height == height) {
  return;
 }
 scratch->lookup_source = source;
 scratch->lookup_width = width;
 scratch->lookup_height = height;
 scratch->source_x.resize(width);
 scratch->source_y.resize(height);
 fill_center_scale_lookup(scratch->source_x, width, source.width, "scaled mask x overflow");
 fill_center_scale_lookup(scratch->source_y, height, source.height, "scaled mask y overflow");
}
void clear_padding(std::vector<std::uint8_t>* target, const MaskDimensions dimensions,
                   const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox) {
 const std::size_t top = static_cast<std::size_t>(letterbox.offset_y) * dimensions.width;
 std::fill_n(target->data(), top, std::uint8_t{0U});
 const std::uint32_t right = dimensions.width - letterbox.offset_x - letterbox.resized_width;
 for (std::uint32_t row = 0U; row < letterbox.resized_height; ++row) {
  std::uint8_t* output = target->data() + static_cast<std::size_t>(letterbox.offset_y + row) * dimensions.width;
  std::fill_n(output, letterbox.offset_x, std::uint8_t{0U});
  std::fill_n(output + letterbox.offset_x + letterbox.resized_width, right, std::uint8_t{0U});
 }
 const std::uint32_t content_end = letterbox.offset_y + letterbox.resized_height;
 const std::size_t bottom = static_cast<std::size_t>(dimensions.height - content_end) * dimensions.width;
 std::fill_n(target->data() + static_cast<std::size_t>(content_end) * dimensions.width, bottom, std::uint8_t{0U});
}
}  // namespace
EncodedRowMajorMask encode_dense_row_major_mask(const std::span<const std::uint8_t> dense, const MaskDimensions dimensions) {
 const std::size_t pixels = checked_pixel_count(dimensions);
 if (dense.size() != pixels) { throw std::runtime_error("dense mask size does not match its dimensions"); }
 EncodedRowMajorMask encoded;
 const __m256i zero = _mm256_setzero_si256();
 std::size_t cursor = 0U;
 while (cursor < pixels) {
  while (cursor + 32U <= pixels) {
   const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dense.data() + cursor));
   const std::uint32_t nonzero = ~static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(values, zero)));
   if (nonzero != 0U) {
    cursor += static_cast<std::uint32_t>(__builtin_ctz(nonzero));
    break;
   }
   cursor += 32U;
  }
  while (cursor < pixels && dense[cursor] == 0U) { ++cursor; }
  if (cursor == pixels) { break; }
  const std::size_t begin = cursor;
  while (cursor + 32U <= pixels) {
   const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dense.data() + cursor));
   const std::uint32_t zeros = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(values, zero)));
   if (zeros != 0U) {
    cursor += static_cast<std::uint32_t>(__builtin_ctz(zeros));
    break;
   }
   cursor += 32U;
  }
  while (cursor < pixels && dense[cursor] != 0U) { ++cursor; }
  encoded.pairs.push_back({
   checked_cast<std::uint32_t>(begin, "mask run start overflow"),
   checked_cast<std::uint32_t>(cursor - begin, "mask run length overflow"),
  });
  include_run(&encoded.bounds, begin, cursor, dimensions.width);
 }
 return encoded;
}
namespace {
void inspect_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions dimensions, RowMajorMaskBounds* bounds) {
 const auto pixels = checked_pixel_count(dimensions);
 if (bounds != nullptr) *bounds = {};
 std::size_t previous_end = 0U;
 for (const auto pair : pairs) {
  if (pair.length == 0U || pair.start < previous_end || pair.start > pixels || pair.length > pixels - pair.start)
   throw std::runtime_error("row-major mask contains an invalid run");
  previous_end = static_cast<std::size_t>(pair.start) + pair.length;
  include_run(bounds, pair.start, previous_end, dimensions.width);
 }
}
}  // namespace
RowMajorMaskBounds row_major_mask_bounds(const std::span<const RLEPair> pairs, const MaskDimensions dimensions) {
 RowMajorMaskBounds bounds;
 inspect_row_major_mask(pairs, dimensions, &bounds);
 return bounds;
}
void materialize_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions dimensions, std::vector<std::uint8_t>* dense,
                                RowMajorMaskBounds* bounds) {
 if (dense == nullptr) { throw std::invalid_argument("dense mask output is required"); }
 const std::size_t pixels = checked_pixel_count(dimensions);
 dense->assign(pixels, std::uint8_t{0U});
 if (bounds != nullptr) { *bounds = {}; }
 std::size_t previous_end = 0U;
 for (const RLEPair pair : pairs) {
  const std::size_t begin = pair.start;
  const std::size_t length = pair.length;
  if (length == 0U || begin < previous_end || begin > pixels || length > pixels - begin) { throw std::runtime_error("row-major mask contains an invalid run"); }
  const std::size_t end = begin + length;
  std::fill(dense->data() + begin, dense->data() + end, std::uint8_t{1U});
  include_run(bounds, begin, end, dimensions.width);
  previous_end = end;
 }
}
EncodedRowMajorMask resize_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions source_dimensions, const MaskDimensions target_dimensions,
                                          const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox, MaskResizeScratch* scratch,
                                          RowMajorMaskBounds* source_bounds) {
 if (scratch == nullptr || letterbox.resized_width == 0U || letterbox.resized_height == 0U || letterbox.resized_width > target_dimensions.width ||
     letterbox.resized_height > target_dimensions.height || letterbox.offset_x > target_dimensions.width - letterbox.resized_width ||
     letterbox.offset_y > target_dimensions.height - letterbox.resized_height) {
  throw std::invalid_argument("mask resize parameters are invalid");
 }
 if (pairs.empty()) { return {}; }
 inspect_row_major_mask(pairs, source_dimensions, source_bounds);
 scratch->target_mask.resize(checked_pixel_count(target_dimensions));
 clear_padding(&scratch->target_mask, target_dimensions, letterbox);
 prepare_lookup(source_dimensions, letterbox.resized_width, letterbox.resized_height, scratch);
 std::size_t run = 0U;
 for (std::uint32_t y = 0U; y < letterbox.resized_height; ++y) {
  std::uint8_t* target = scratch->target_mask.data() + static_cast<std::size_t>(letterbox.offset_y + y) * target_dimensions.width + letterbox.offset_x;
  if (y != 0U && scratch->source_y[y] == scratch->source_y[y - 1U]) {
   std::copy_n(target - target_dimensions.width, letterbox.resized_width, target);
   continue;
  }
  const auto row = static_cast<std::size_t>(scratch->source_y[y]) * source_dimensions.width;
  for (std::uint32_t x = 0U; x < letterbox.resized_width; ++x) {
   const auto pixel = row + scratch->source_x[x];
   while (run < pairs.size() && static_cast<std::size_t>(pairs[run].start) + pairs[run].length <= pixel) ++run;
   target[x] = static_cast<std::uint8_t>(run < pairs.size() && pairs[run].start <= pixel);
  }
 }
 return encode_dense_row_major_mask(scratch->target_mask, target_dimensions);
}
}  // namespace mmltk::backend::data::dataset
