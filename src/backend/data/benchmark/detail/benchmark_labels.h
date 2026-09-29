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
class BenchmarkLabelWorkspace final {
public:
 [[nodiscard]] static std::uint64_t required_bytes(std::uint32_t width) noexcept { return std::uint64_t{width} * 32U + 65536U; }
 [[nodiscard]] std::size_t retained_bytes() const noexcept { return scratch_.intervals.capacity() * sizeof(scratch_.intervals.front()); }
 void retire() noexcept { scratch_ = {}; }
 [[nodiscard]] dataset::MaskResizeScratch& scratch() noexcept { return scratch_; }

private:
 dataset::MaskResizeScratch scratch_;
};
struct BenchmarkLabelChunk {
 std::uint32_t width = 0, height = 0;
 std::uint64_t dropped = 0;
 std::vector<PackedInstance> labels;
 std::vector<RLEPair> runs;
};
[[nodiscard]] BenchmarkLabelChunk compile_benchmark_image_labels(const NormalizedAnnotationReadView&, std::size_t image, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode, BenchmarkLabelWorkspace&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] PackedInstance benchmark_canvas_box(std::uint8_t class_id, float x1, float y1, float x2, float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox);
}  // namespace mmltk::backend::data::benchmark_internal
