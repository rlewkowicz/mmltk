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
void MaskRunEmitter::run(std::uint64_t begin, std::uint64_t end) {
 if (begin == end) return;
 const auto start = checked_cast<std::uint32_t>(begin, start_error_);
 if (end < begin) throw std::overflow_error(length_error_);
 const auto length = checked_cast<std::uint32_t>(end - begin, length_error_);
 if (required_ > first_ && std::uint64_t{last_.start} + last_.length == begin) {
  last_.length = checked_cast<std::uint32_t>(end - last_.start, length_error_);
  if (required_ <= maximum_) output_.back() = last_;
 } else {
  const auto required = mmltk::common::math::checked_add(required_, std::size_t{1}, "mask run count overflow");
  const RLEPair next{start, length};
  if (required <= maximum_) {
   if (output_.size() == output_.capacity() && maximum_ != std::numeric_limits<std::size_t>::max()) {
    const auto growth = std::min(maximum_ - output_.capacity(), std::max(std::size_t{1}, output_.capacity()));
    output_.reserve(output_.capacity() + growth);
   }
   output_.push_back(next);
  }
  required_ = required;
  last_ = next;
 }
 include_row_major_mask_run(bounds_, begin, end, width_);
}
void MaskRunEmitter::finish() const {
 if (required_ > maximum_) throw MaskRunCapacityError(required_);
}
bool MaskRunEmitter::step_slab(Slab& slab) {
 if (slab.row == slab.end || slab.intervals.empty()) return true;
 if (slab.intervals.size() == 1 && slab.intervals.front().first == 0 && slab.intervals.front().second == width_) {
  run(std::uint64_t{slab.row} * width_, std::uint64_t{slab.end} * width_);
  slab.row = slab.end;
  return true;
 }
 if (slab.row >= slab.end) return true;
 const auto [begin, end] = slab.intervals[slab.interval];
 run(std::uint64_t{slab.row} * width_ + begin, std::uint64_t{slab.row} * width_ + end);
 if (++slab.interval == slab.intervals.size()) {
  slab.interval = 0;
  ++slab.row;
 }
 return slab.row == slab.end;
}
void MaskRunEmitter::slab(std::uint32_t first, std::uint32_t end, std::span<const std::pair<std::uint32_t, std::uint32_t>> intervals) {
 Slab pending{first, end, intervals};
 while (!step_slab(pending)) {}
}
EncodedRowMajorMask encode_dense_row_major_mask(const std::span<const std::uint8_t> dense, const MaskDimensions dimensions) {
 const std::size_t pixels = checked_pixel_count(dimensions);
 if (dense.size() != pixels) { throw std::runtime_error("dense mask size does not match its dimensions"); }
 EncodedRowMajorMask encoded;
 MaskRunEmitter emitter(encoded.pairs, 0, dimensions.width, &encoded.bounds);
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
  emitter.run(begin, cursor);
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
namespace {
// First target coordinate whose nearest-center source coordinate is >= bound.
// The wide intermediate covers every pair of uint32_t dimensions exactly.
std::uint32_t inverse_mask_coordinate(std::uint32_t bound, std::uint32_t source_extent, std::uint32_t target_extent) {
 const auto numerator = static_cast<__uint128_t>(2) * target_extent * bound;
 if (numerator <= source_extent) return 0;
 const auto denominator = std::uint64_t{2} * source_extent;
 return static_cast<std::uint32_t>(std::min<__uint128_t>(target_extent, (numerator - source_extent + denominator - 1) / denominator));
}
}  // namespace
RowMajorMaskProjection::RowMajorMaskProjection(std::span<const RLEPair> pairs, MaskDimensions source, MaskDimensions target, const mmltk::backend::imaging::resample::ImageResizeGeometry& geometry,
 MaskResizeScratch* scratch, mmltk::common::types::WorkspaceOutput<RLEPair> output, std::size_t maximum_runs)
    : pairs_(pairs),
      source_dimensions_(source),
      geometry_(geometry),
      scratch_(scratch),
      output_(output),
      first_(output.size()),
      emitter_(output, first_, target.width, &target_, "mask run start overflow", "mask run length overflow", maximum_runs) {
 if (!scratch || !geometry.resized_width || !geometry.resized_height || geometry.resized_width > target.width || geometry.resized_height > target.height ||
     geometry.offset_x > target.width - geometry.resized_width || geometry.offset_y > target.height - geometry.resized_height)
  throw std::invalid_argument("mask resize parameters are invalid");
 if (pairs.empty()) {
  finished_ = true;
  return;
 }
 pixels_ = checked_pixel_count(source);
 (void)checked_pixel_count(target);
 identity_ =
  source.width == target.width && source.height == target.height && geometry.resized_width == source.width && geometry.resized_height == source.height && !geometry.offset_x && !geometry.offset_y;
 full_ = {geometry.offset_x, geometry.offset_x + geometry.resized_width};
 scratch_->intervals.clear();
}
RowMajorMaskProjection::~RowMajorMaskProjection() {
 if (!finished_ && !failed_) rollback();
}
void RowMajorMaskProjection::rollback() noexcept {
 output_.resize(first_);
 failed_ = true;
}
void RowMajorMaskProjection::start_slab(std::uint32_t first, std::uint32_t end, std::span<const std::pair<std::uint32_t, std::uint32_t>> intervals, bool clear) {
 slab_ = {first + geometry_.offset_y, end + geometry_.offset_y, intervals};
 slab_active_ = true;
 clear_intervals_ = clear;
}
void RowMajorMaskProjection::flush() {
 start_slab(inverse_mask_coordinate(pending_row_, source_dimensions_.height, geometry_.resized_height), inverse_mask_coordinate(pending_row_ + 1, source_dimensions_.height, geometry_.resized_height),
  scratch_->intervals, true);
}
bool RowMajorMaskProjection::partial(std::uint32_t row, std::uint32_t begin, std::uint32_t end) {
 if (row != pending_row_) {
  flush();
  pending_row_ = row;
  return false;
 }
 const auto first = inverse_mask_coordinate(begin, source_dimensions_.width, geometry_.resized_width) + geometry_.offset_x;
 const auto last = inverse_mask_coordinate(end, source_dimensions_.width, geometry_.resized_width) + geometry_.offset_x;
 if (first != last) {
  if (!scratch_->intervals.empty() && first == scratch_->intervals.back().second)
   scratch_->intervals.back().second = last;
  else
   scratch_->intervals.emplace_back(first, last);
 }
 return true;
}
void RowMajorMaskProjection::advance() {
 if (slab_active_) {
  if (emitter_.step_slab(slab_)) {
   slab_active_ = false;
   if (clear_intervals_) scratch_->intervals.clear();
  }
  return;
 }
 switch (stage_) {
  case Stage::Source: {
   if (cursor_ == pairs_.size()) {
    stage_ = Stage::FinalFlush;
    break;
   }
   const auto run = pairs_[cursor_++];
   const auto end = std::uint64_t{run.start} + run.length;
   if (!run.length || run.start < previous_end_ || end > pixels_) throw std::runtime_error("row-major mask contains an invalid run");
   previous_end_ = end;
   include_row_major_mask_run(&source_, run.start, end, source_dimensions_.width);
   if (identity_) {
    emitter_.run(run.start, end);
    break;
   }
   row_ = run.start / source_dimensions_.width;
   x_ = run.start % source_dimensions_.width;
   remaining_ = run.length;
   stage_ = Stage::Head;
   break;
  }
  case Stage::Head:
   if (x_) {
    const auto count = static_cast<std::uint32_t>(std::min(remaining_, std::uint64_t{source_dimensions_.width - x_}));
    if (!partial(row_, x_, x_ + count)) break;
    remaining_ -= count;
    ++row_;
   }
   stage_ = Stage::Full;
   break;
  case Stage::Full:
   rows_ = static_cast<std::uint32_t>(remaining_ / source_dimensions_.width);
   if (rows_) {
    flush();
    stage_ = Stage::FullSlab;
   } else
    stage_ = Stage::Tail;
   break;
  case Stage::FullSlab:
   start_slab(inverse_mask_coordinate(row_, source_dimensions_.height, geometry_.resized_height), inverse_mask_coordinate(row_ + rows_, source_dimensions_.height, geometry_.resized_height),
    std::span(&full_, 1), false);
   row_ += rows_;
   remaining_ %= source_dimensions_.width;
   stage_ = Stage::Tail;
   break;
  case Stage::Tail:
   if (remaining_ && !partial(row_, 0, static_cast<std::uint32_t>(remaining_))) break;
   stage_ = Stage::Source;
   break;
  case Stage::FinalFlush:
   if (!identity_) flush();
   stage_ = Stage::Finish;
   break;
  case Stage::Finish:
   emitter_.finish();
   finished_ = true;
   break;
 }
}
bool RowMajorMaskProjection::step(std::size_t maximum_steps) {
 if (!maximum_steps) throw std::invalid_argument("mask projection step requires a positive bound");
 if (failed_) throw std::logic_error("mask projection resumed after failure");
 try {
  while (!finished_ && maximum_steps--) advance();
 } catch (...) {
  rollback();
  throw;
 }
 return finished_;
}
RowMajorMaskBounds append_resized_row_major_mask(const std::span<const RLEPair> pairs, const MaskDimensions source, const MaskDimensions target,
 const mmltk::backend::imaging::resample::ImageResizeGeometry& geometry, MaskResizeScratch* scratch, mmltk::common::types::WorkspaceOutput<RLEPair> output, RowMajorMaskBounds* source_bounds,
 std::size_t maximum_runs) {
 RowMajorMaskProjection projection(pairs, source, target, geometry, scratch, output, maximum_runs);
 while (!projection.step()) {}
 if (source_bounds && !pairs.empty()) *source_bounds = projection.source_bounds();
 return projection.target_bounds();
}
}  // namespace mmltk::backend::data::dataset
