#pragma once  // backend.data private implementation boundary
#include "src/common/types/workspace_buffer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <limits>
#include <stdexcept>
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
 mmltk::common::types::WorkspaceBuffer<std::pair<std::uint32_t, std::uint32_t>> intervals;
};
class MaskRunCapacityError final : public std::length_error {
public:
 explicit MaskRunCapacityError(std::size_t required) : std::length_error("row-major mask exceeds bounded output"), required_(required) {}
 [[nodiscard]] std::size_t required_runs() const noexcept { return required_; }

private:
 std::size_t required_;
};
// Emits ordered, nonoverlapping runs of one independent mask. The explicit
// boundary prevents adjacency coalescing with a preceding mask. Callers own
// input validation and transactional rollback; overlap union is a separate algorithm.
class MaskRunEmitter final {
public:
 MaskRunEmitter(mmltk::common::types::WorkspaceOutput<RLEPair> output, std::size_t first_run, std::uint32_t width, RowMajorMaskBounds* bounds = nullptr, const char* start_error = "mask run start overflow",
  const char* length_error = "mask run length overflow", std::size_t maximum_runs = std::numeric_limits<std::size_t>::max())
     : output_(output), first_(first_run), width_(width), bounds_(bounds), start_error_(start_error), length_error_(length_error), maximum_(maximum_runs), required_(output.size()) {
  if (required_ > first_) last_ = output.back();
 }
 void run(std::uint64_t begin, std::uint64_t end);
 struct Slab {
  std::uint32_t row = 0, end = 0;
  std::span<const std::pair<std::uint32_t,std::uint32_t>> intervals;
  std::size_t interval = 0;
 };
 // Emits at most one interval, or one coalesced full-width slab.
 [[nodiscard]] bool step_slab(Slab&);
 void slab(std::uint32_t first_row, std::uint32_t end_row, std::span<const std::pair<std::uint32_t, std::uint32_t>>);
 void finish() const;

private:
 mmltk::common::types::WorkspaceOutput<RLEPair> output_;
 std::size_t first_;
 std::uint32_t width_;
 RowMajorMaskBounds* bounds_;
 const char* start_error_;
 const char* length_error_;
 std::size_t maximum_, required_;
 RLEPair last_{};
};
// Accumulates one validated nonempty [begin, end) run at a nonzero width.
// A null output skips geometry; validation and cancellation remain caller-owned.
void include_row_major_mask_run(RowMajorMaskBounds* bounds, std::size_t begin, std::size_t end, std::uint32_t width);
// Optionally derives foreground count during the same validated bounds visit.
[[nodiscard]] RowMajorMaskBounds row_major_mask_bounds(std::span<const RLEPair> pairs, MaskDimensions dimensions, std::uint64_t* foreground = nullptr);
[[nodiscard]] EncodedRowMajorMask encode_dense_row_major_mask(std::span<const std::uint8_t> dense, MaskDimensions dimensions);
void materialize_row_major_mask(std::span<const RLEPair> pairs, MaskDimensions dimensions, std::vector<std::uint8_t>* dense, RowMajorMaskBounds* bounds = nullptr);
// Explicit continuation of the same nearest-center sampler used by one-shot
// append. The caller retains source, scratch and output through every step.
// Each step is bounded by source transitions and emitted slab intervals/rows.
// Destruction before success rolls back this mask; earlier masks are untouched.
class RowMajorMaskProjection final {
public:
 RowMajorMaskProjection(std::span<const RLEPair>,MaskDimensions source,MaskDimensions target,
  const mmltk::backend::imaging::resample::ImageResizeGeometry&,MaskResizeScratch*,
  mmltk::common::types::WorkspaceOutput<RLEPair>,std::size_t maximum_runs=std::numeric_limits<std::size_t>::max());
 ~RowMajorMaskProjection();
 RowMajorMaskProjection(const RowMajorMaskProjection&)=delete;
 RowMajorMaskProjection& operator=(const RowMajorMaskProjection&)=delete;
 [[nodiscard]] bool step(std::size_t maximum_steps=1024);
 [[nodiscard]] const RowMajorMaskBounds& source_bounds() const noexcept { return source_; }
 [[nodiscard]] const RowMajorMaskBounds& target_bounds() const noexcept { return target_; }
private:
 enum class Stage { Source,Head,Full,FullSlab,Tail,FinalFlush,Finish };
 std::span<const RLEPair> pairs_;
 MaskDimensions source_dimensions_;
 mmltk::backend::imaging::resample::ImageResizeGeometry geometry_;
 MaskResizeScratch* scratch_;
 mmltk::common::types::WorkspaceOutput<RLEPair> output_;
 std::size_t first_,cursor_=0,pixels_=0;
 RowMajorMaskBounds source_,target_;
 MaskRunEmitter emitter_;
 MaskRunEmitter::Slab slab_;
 std::pair<std::uint32_t,std::uint32_t> full_;
 std::uint64_t previous_end_=0,remaining_=0;
 std::uint32_t pending_row_=0,row_=0,x_=0,rows_=0;
 Stage stage_=Stage::Source;
 bool identity_=false,slab_active_=false,clear_intervals_=false,finished_=false,failed_=false;
 void start_slab(std::uint32_t first,std::uint32_t end,std::span<const std::pair<std::uint32_t,std::uint32_t>>,bool clear);
 void flush();
 [[nodiscard]] bool partial(std::uint32_t row,std::uint32_t begin,std::uint32_t end);
 void advance();
 void rollback() noexcept;
};
// Appends one independent mask, coalescing only within that mask. Failure leaves
// output and source bounds unchanged; empty input leaves source bounds untouched.
[[nodiscard]] RowMajorMaskBounds append_resized_row_major_mask(std::span<const RLEPair>, MaskDimensions source, MaskDimensions target, const mmltk::backend::imaging::resample::ImageResizeGeometry&,
 MaskResizeScratch*, mmltk::common::types::WorkspaceOutput<RLEPair> output, RowMajorMaskBounds* source_bounds = nullptr, std::size_t maximum_runs = std::numeric_limits<std::size_t>::max());
}  // namespace mmltk::backend::data::dataset
