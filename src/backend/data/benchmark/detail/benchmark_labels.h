#pragma once
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/detail/compiled_label_workspace.h"
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
struct BenchmarkLabelView {
 std::uint32_t width = 0, height = 0;
 std::uint64_t dropped = 0;
 std::span<const PackedInstance> labels;
 std::span<const RLEPair> runs;
};
class BenchmarkLabelWorkspace final {
 detail::CompiledLabelWorkspace workspace_;
 std::uint32_t prepared_width_ = 0;
public:
 struct OutputGrowthObserver {
  void* context = nullptr;
  void (*grown)(void*,bool,std::size_t,std::size_t) noexcept = nullptr;
 };
 void observe_output_growth(OutputGrowthObserver observer) noexcept { growth_ = observer; }
 [[nodiscard]] static std::uint64_t required_bytes(std::uint32_t width) { return detail::CompiledLabelWorkspace::required_bytes(width); }
 void prepare(std::uint32_t width, detail::CompileAllowance allowance = {}) {
  const auto previous = workspace_.labels.capacity()*sizeof(PackedInstance);
  const auto previous_runs = workspace_.runs.capacity()*sizeof(RLEPair);
  workspace_.prepare(width,std::move(allowance));
  prepared_width_ = width;
  if (growth_.grown && !previous) growth_.grown(growth_.context,false,previous,workspace_.labels.capacity()*sizeof(PackedInstance));
  if (growth_.grown && !previous_runs) growth_.grown(growth_.context,true,previous_runs,workspace_.runs.capacity()*sizeof(RLEPair));
 }
 void finish() noexcept { workspace_.release_borrow(); prepared_width_=0; }
 void clear() noexcept { workspace_.labels.clear(); workspace_.runs.clear(); }
 void ensure_prepared(std::uint32_t width) { if (width > prepared_width_) prepare(width); }
 [[nodiscard]] std::size_t retained_bytes() const noexcept { return workspace_.retained_bytes(); }
 [[nodiscard]] dataset::MaskResizeScratch& scratch() noexcept { return workspace_.scratch(); }
 [[nodiscard]] detail::CompiledLabelWorkspace& output() noexcept { return workspace_; }
 [[nodiscard]] detail::CompiledLabelWorkspace::OutputLease release_output() { prepared_width_ = 0; return workspace_.release_output(); }
private:
 OutputGrowthObserver growth_;
};
[[nodiscard]] BenchmarkLabelChunk compile_benchmark_image_labels(const NormalizedAnnotationReadView&, std::size_t image, std::pair<std::uint32_t, std::uint32_t> dimensions, std::uint32_t resolution,
 mmltk::backend::imaging::resample::ImageResizeMode, BenchmarkLabelWorkspace&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] BenchmarkLabelView compile_benchmark_image_labels_reused(const NormalizedAnnotationReadView&, std::size_t image, std::pair<std::uint32_t, std::uint32_t> dimensions,
 std::uint32_t resolution, mmltk::backend::imaging::resample::ImageResizeMode, BenchmarkLabelWorkspace&, mmltk::common::concurrency::CancellationObservation = {});
[[nodiscard]] PackedInstance benchmark_canvas_box(std::uint8_t class_id, float x1, float y1, float x2, float y2, const mmltk::backend::imaging::resample::ImageResizeGeometry& letterbox);
}  // namespace mmltk::backend::data::benchmark_internal
