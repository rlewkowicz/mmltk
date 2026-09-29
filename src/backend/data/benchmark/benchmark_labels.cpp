#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/common/math/checked_arithmetic.h"
#include <stdexcept>
namespace mmltk::backend::data::benchmark_internal {
BenchmarkLabelInput::BenchmarkLabelInput(NormalizedAnnotationReadView index, std::size_t image) : index_(std::move(index)), image_(image) {
 admit_normalized_annotation_image(index_.storage(), index_.source_position(image_));
}
BenchmarkLabelInput::BenchmarkLabelInput(NormalizedAnnotationReadView index, std::shared_ptr<const CoconutNativeImage> native) : native_(std::move(native)), index_(std::move(index)) {
 if (!native_) throw std::invalid_argument("benchmark labels require native input custody");
 (void)index_.image(0);
}
namespace {
void project_labels(const NormalizedAnnotationReadView& index, std::size_t position, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode, BenchmarkLabelWorkspace& workspace, BenchmarkLabelChunk& chunk, mmltk::common::concurrency::CancellationObservation cancel_requested) {
 namespace common_math = mmltk::common::math;
 chunk.labels.clear();
 chunk.runs.clear();
 const auto& image = index.image(position);
 const auto [source_width, source_height] = dimensions;
 chunk.width = source_width;
 chunk.height = source_height;
 const bool matching_geometry = index.source == BenchmarkDatasetSource::kOpenImagesV7 || (source_width == image.width && source_height == image.height);
 const auto box_count = matching_geometry ? image.box_count : 0U;
 chunk.dropped = image.box_count - box_count;
 const auto geometry = mmltk::backend::imaging::resample::compute_image_resize_geometry(source_width, source_height, resolution, resolution, resize_mode);
 ImageEntry extent;
 const auto identity = std::to_string(image.source_image_id);
 bool exceeded = box_count > COMPILED_LABEL_CAPACITY / sizeof(PackedInstance);
 const auto maximum_runs = exceeded ? 0 : (COMPILED_LABEL_CAPACITY - annotation_layout(box_count, 0).run_offset) / sizeof(RLEPair);
 std::size_t required_runs = 0;
 if (!exceeded) chunk.labels.reserve(box_count);
 for (const auto& box : index.storage().boxes.subspan(common_math::checked_cast<std::size_t>(image.first_box, "box index overflow"), box_count)) {
  throw_if_benchmark_cancelled(cancel_requested);
  PackedInstance label = benchmark_canvas_box(box.class_id, box.x1, box.y1, box.x2, box.y2, geometry);
  label.flags = box.flags;
  label.annotation_id = box.annotation_id;
  label.source_category_id = box.source_category_id;
  label.source_ordinal = box.source_ordinal;
  label.original_area = box.original_area;
  if (index.source == BenchmarkDatasetSource::kOpenImagesV7) label.original_area *= static_cast<double>(source_width) * source_height;
  if (label.bbox_x2 <= label.bbox_x1 || label.bbox_y2 <= label.bbox_y1) throw std::runtime_error("benchmark continuous box is not representable on the compiled canvas");
  const auto begin = chunk.runs.size();
  if (box.mask_rle_offset > index.storage().mask_rle_pairs.size() || box.mask_rle_pairs > index.storage().mask_rle_pairs.size() - box.mask_rle_offset)
   throw std::runtime_error("benchmark source mask range is invalid");
  const auto source_mask = index.storage().mask_rle_pairs.subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs);
  try {
   (void)dataset::append_resized_row_major_mask(source_mask, {image.width, image.height}, {resolution, resolution}, geometry, &workspace.scratch(), chunk.runs, nullptr, exceeded ? 0 : maximum_runs);
  } catch (const dataset::MaskRunCapacityError& error) {
   // The failed append rolled back its prefix. Continue counting subsequent
   // independent masks without retaining output, so the image's final error
   // states its complete payload extent rather than the first excessive prefix.
   required_runs = common_math::checked_add(required_runs, error.required_runs() - begin, "benchmark required mask runs overflow");
   exceeded = true;
  }
  required_runs = common_math::checked_add(required_runs, chunk.runs.size() - begin, "benchmark required mask runs overflow");
  if (exceeded) continue;
  label.mask_rle_offset = common_math::checked_multiply<decltype(PackedInstance::mask_rle_offset)>(begin, sizeof(RLEPair), "benchmark mask offset overflow");
  label.mask_rle_pairs = common_math::checked_cast<std::uint16_t>(chunk.runs.size() - begin, "benchmark instance mask run count overflow");
  chunk.labels.push_back(label);
 }
 set_annotation_extent(extent, box_count, required_runs, identity);
}
}  // namespace
BenchmarkLabelChunk compile_benchmark_image_labels(const NormalizedAnnotationReadView& index, std::size_t position, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode mode, BenchmarkLabelWorkspace& workspace, mmltk::common::concurrency::CancellationObservation cancellation) {
 BenchmarkLabelChunk result;
 project_labels(index, position, dimensions, resolution, mode, workspace, result, cancellation);
 return result;
}
const BenchmarkLabelChunk& compile_benchmark_image_labels_reused(const NormalizedAnnotationReadView& index, std::size_t position, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode mode, BenchmarkLabelWorkspace& workspace, mmltk::common::concurrency::CancellationObservation cancellation) {
 auto& result = workspace.output();
 project_labels(index, position, dimensions, resolution, mode, workspace, result, cancellation);
 return result;
}
PackedInstance benchmark_canvas_box(
 const std::uint8_t class_id, const float x1, const float y1, const float x2, const float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox) {
 if (letterbox.resized_width == 0U || letterbox.resized_height == 0U) { throw std::runtime_error("benchmark box requires a valid letterbox"); }
 PackedInstance result{};
 result.class_id = class_id;
 result.bbox_x1 = x1 * static_cast<float>(letterbox.resized_width) + static_cast<float>(letterbox.offset_x);
 result.bbox_y1 = y1 * static_cast<float>(letterbox.resized_height) + static_cast<float>(letterbox.offset_y);
 result.bbox_x2 = x2 * static_cast<float>(letterbox.resized_width) + static_cast<float>(letterbox.offset_x);
 result.bbox_y2 = y2 * static_cast<float>(letterbox.resized_height) + static_cast<float>(letterbox.offset_y);
 return result;
}
}  // namespace mmltk::backend::data::benchmark_internal
