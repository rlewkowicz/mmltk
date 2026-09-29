#pragma once
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class CoconutNativeImage;
// Immutable input custody is independent of source controllers and CPU lanes.
// Normalized views own their mapping; native views additionally own the image.
class BenchmarkLabelInput final {
public:
 explicit BenchmarkLabelInput(NormalizedAnnotationReadView, std::size_t image);
 [[nodiscard]] const NormalizedAnnotationReadView& index() const noexcept { return index_; }
 [[nodiscard]] std::size_t position() const noexcept { return image_; }

private:
 friend class CoconutNativeImage;
 BenchmarkLabelInput(NormalizedAnnotationReadView, std::shared_ptr<const CoconutNativeImage>);
 std::shared_ptr<const CoconutNativeImage> native_;
 NormalizedAnnotationReadView index_;
 std::size_t image_ = 0;
};
struct BenchmarkLabelChunk {
 std::uint32_t width = 0, height = 0;
 std::uint64_t dropped = 0;
 std::vector<PackedInstance> labels;
 std::vector<RLEPair> runs;
};
class BenchmarkLabelWorkspace final {
public:
 struct OutputGrowthObserver {
  void* context = nullptr;
  void (*grown)(void*, bool mask_runs, std::size_t previous_bytes, std::size_t retained_bytes) noexcept = nullptr;
 };
 void observe_output_growth(OutputGrowthObserver observer) noexcept { growth_ = observer; }
 [[nodiscard]] static std::uint64_t required_bytes(std::uint32_t width) noexcept { return std::uint64_t{width} * 32U + 65536U + 2U * COMPILED_LABEL_CAPACITY; }
 [[nodiscard]] std::size_t retained_bytes() const noexcept {
  return scratch_.intervals.capacity() * sizeof(scratch_.intervals.front()) + output_.labels.capacity() * sizeof(PackedInstance) + output_.runs.capacity() * sizeof(RLEPair);
 }
 void retire() noexcept { scratch_ = {}; output_ = {}; }
 [[nodiscard]] dataset::MaskResizeScratch& scratch() noexcept { return scratch_; }
 // Borrow only until this lane writes its slot. Standalone products never alias it.
 [[nodiscard]] BenchmarkLabelChunk& output() {
  reserve_output(output_.labels, false);
  reserve_output(output_.runs, true);
  return output_;
 }

private:
 template <class T>
 void reserve_output(std::vector<T>& values, bool mask_runs) {
  const auto required = COMPILED_LABEL_CAPACITY / sizeof(T);
  if (values.capacity() >= required) return;
  const auto previous = growth_.grown ? values.capacity() * sizeof(T) : 0;
  values.reserve(required);
  if (growth_.grown) growth_.grown(growth_.context, mask_runs, previous, values.capacity() * sizeof(T));
 }
 OutputGrowthObserver growth_;
 dataset::MaskResizeScratch scratch_;
 BenchmarkLabelChunk output_;
};
[[nodiscard]] BenchmarkLabelChunk compile_benchmark_image_labels(const NormalizedAnnotationReadView&, std::size_t image, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode, BenchmarkLabelWorkspace&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] const BenchmarkLabelChunk& compile_benchmark_image_labels_reused(const NormalizedAnnotationReadView&, std::size_t image, std::pair<std::uint32_t, std::uint32_t> dimensions,
 std::uint32_t resolution, mmltk::backend::imaging::resample::ImageResizeMode, BenchmarkLabelWorkspace&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] PackedInstance benchmark_canvas_box(std::uint8_t class_id, float x1, float y1, float x2, float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox);
}  // namespace mmltk::backend::data::benchmark_internal
