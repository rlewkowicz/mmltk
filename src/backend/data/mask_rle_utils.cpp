#include "detail/mask_rle_utils.h"
#include <immintrin.h>
#include "src/pch_std.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::dataset {
using mmltk::common::math::checked_cast;
namespace {
[[nodiscard]] std::size_t checked_pixel_count(const MaskDimensions dimensions) {
 if (dimensions.width == 0U || dimensions.height == 0U || dimensions.width > std::numeric_limits<std::size_t>::max() / dimensions.height) { throw std::runtime_error("mask dimensions are invalid"); }
 return static_cast<std::size_t>(dimensions.width) * dimensions.height;
}
}  // namespace
void include_row_major_mask_run(RowMajorMaskBounds* bounds, const std::size_t begin, const std::size_t end, const std::uint32_t width) {
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
  include_row_major_mask_run(&encoded.bounds, begin, cursor, dimensions.width);
 }
 return encoded;
}
namespace {
void inspect_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions dimensions, RowMajorMaskBounds* bounds, std::uint64_t* foreground) {
 const auto pixels = checked_pixel_count(dimensions);
 if (bounds != nullptr) *bounds = {};
 std::size_t previous_end = 0U;
 if (foreground) *foreground = 0;
 for (const auto pair : pairs) {
  if (pair.length == 0U || pair.start < previous_end || pair.start > pixels || pair.length > pixels - pair.start) throw std::runtime_error("row-major mask contains an invalid run");
  previous_end = static_cast<std::size_t>(pair.start) + pair.length;
  include_row_major_mask_run(bounds, pair.start, previous_end, dimensions.width);
  if (foreground) *foreground += pair.length;
 }
}
}  // namespace
RowMajorMaskBounds row_major_mask_bounds(const std::span<const RLEPair> pairs, const MaskDimensions dimensions, std::uint64_t* foreground) {
 RowMajorMaskBounds bounds;
 inspect_row_major_mask(pairs, dimensions, &bounds, foreground);
 return bounds;
}
void materialize_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions dimensions, std::vector<std::uint8_t>* dense, RowMajorMaskBounds* bounds) {
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
  include_row_major_mask_run(bounds, begin, end, dimensions.width);
  previous_end = end;
 }
}
RowMajorMaskBounds append_resized_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions source_dimensions, const MaskDimensions target_dimensions,
 const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox, MaskResizeScratch* scratch, std::vector<RLEPair>& output, RowMajorMaskBounds* source_bounds) {
 if (scratch == nullptr || letterbox.resized_width == 0U || letterbox.resized_height == 0U || letterbox.resized_width > target_dimensions.width ||
     letterbox.resized_height > target_dimensions.height || letterbox.offset_x > target_dimensions.width - letterbox.resized_width ||
     letterbox.offset_y > target_dimensions.height - letterbox.resized_height) {
  throw std::invalid_argument("mask resize parameters are invalid");
 }
 if (pairs.empty()) return {};
 const auto pixels = checked_pixel_count(source_dimensions);
 (void)checked_pixel_count(target_dimensions);
 const auto output_begin = output.size();
 RowMajorMaskBounds source, target;
 // First target coordinate whose nearest-center source coordinate is >= bound.
 // The wide intermediate covers every pair of uint32_t dimensions exactly.
 const auto inverse = [](std::uint32_t bound, std::uint32_t source, std::uint32_t target) {
  const auto numerator = static_cast<__uint128_t>(2) * target * bound;
  if (numerator <= source) return std::uint32_t{0};
  const auto denominator = std::uint64_t{2} * source;
  return static_cast<std::uint32_t>(std::min<__uint128_t>(target, (numerator - source + denominator - 1) / denominator));
 };
 const auto append = [&](std::uint64_t begin, std::uint64_t end) {
  if (begin == end) return;
  const auto start = checked_cast<std::uint32_t>(begin, "mask run start overflow");
  const auto length = checked_cast<std::uint32_t>(end - begin, "mask run length overflow");
  if (output.size() > output_begin && std::uint64_t{output.back().start} + output.back().length == begin)
   output.back().length = checked_cast<std::uint32_t>(end - output.back().start, "mask run length overflow");
  else output.push_back({start, length});
  include_row_major_mask_run(&target, begin, end, target_dimensions.width);
 };
 scratch->intervals.clear();
 std::uint32_t pending_row = 0;
 const auto slab = [&](std::uint32_t first, std::uint32_t last, std::span<const std::pair<std::uint32_t, std::uint32_t>> intervals) {
  if (first == last || intervals.empty()) return;
  first += letterbox.offset_y;
  last += letterbox.offset_y;
  if (intervals.size() == 1 && intervals.front().first == 0 && intervals.front().second == target_dimensions.width) {
   append(std::uint64_t{first} * target_dimensions.width, std::uint64_t{last} * target_dimensions.width);
   return;
  }
  for (auto y = first; y < last; ++y)
   for (const auto [begin, end] : intervals) append(std::uint64_t{y} * target_dimensions.width + begin, std::uint64_t{y} * target_dimensions.width + end);
 };
 const auto flush = [&] {
  slab(inverse(pending_row, source_dimensions.height, letterbox.resized_height), inverse(pending_row + 1, source_dimensions.height, letterbox.resized_height), scratch->intervals);
  scratch->intervals.clear();
 };
 const auto partial = [&](std::uint32_t row, std::uint32_t begin, std::uint32_t end) {
  if (row != pending_row) { flush(); pending_row = row; }
  const auto first = inverse(begin, source_dimensions.width, letterbox.resized_width) + letterbox.offset_x;
  const auto last = inverse(end, source_dimensions.width, letterbox.resized_width) + letterbox.offset_x;
  if (first == last) return;
  if (!scratch->intervals.empty() && first == scratch->intervals.back().second) scratch->intervals.back().second = last;
  else scratch->intervals.emplace_back(first, last);
 };
 const bool identity = source_dimensions.width == target_dimensions.width && source_dimensions.height == target_dimensions.height &&
  letterbox.resized_width == source_dimensions.width && letterbox.resized_height == source_dimensions.height && !letterbox.offset_x && !letterbox.offset_y;
 std::uint64_t previous_end = 0;
 try {
  for (const auto run : pairs) {
   const auto end = std::uint64_t{run.start} + run.length;
   if (!run.length || run.start < previous_end || end > pixels) throw std::runtime_error("row-major mask contains an invalid run");
   previous_end = end;
   include_row_major_mask_run(&source, run.start, end, source_dimensions.width);
   if (identity) { append(run.start, end); continue; }
   auto row = run.start / source_dimensions.width;
   auto x = run.start % source_dimensions.width;
   auto remaining = std::uint64_t{run.length};
   if (x) {
    const auto count = static_cast<std::uint32_t>(std::min(remaining, std::uint64_t{source_dimensions.width - x}));
    partial(row, x, x + count); remaining -= count; ++row;
   }
   const auto rows = static_cast<std::uint32_t>(remaining / source_dimensions.width);
   if (rows) {
    flush();
    const std::pair<std::uint32_t, std::uint32_t> full{letterbox.offset_x, letterbox.offset_x + letterbox.resized_width};
    slab(inverse(row, source_dimensions.height, letterbox.resized_height), inverse(row + rows, source_dimensions.height, letterbox.resized_height), std::span(&full, 1));
    row += rows; remaining %= source_dimensions.width;
   }
   if (remaining) partial(row, 0, static_cast<std::uint32_t>(remaining));
  }
  if (!identity) flush();
 } catch (...) { output.resize(output_begin); throw; }
 if (source_bounds) *source_bounds = source;
 return target;
}
}  // namespace mmltk::backend::data::dataset
