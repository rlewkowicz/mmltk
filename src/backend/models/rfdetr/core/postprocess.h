#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <cuda_runtime_api.h>
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {}
namespace mmltk::backend::models::rfdetr {
// Coordinate clipping is independent of corner ordering. Bounds are image content in canvas pixels.
void clip_prediction_boxes_(const torch::Tensor& boxes, double left, double top, double right, double bottom);
class ClassPostprocessLane final {
public:
 explicit ClassPostprocessLane(std::shared_ptr<const ResolvedClassLayout> layout) : layout_(std::move(layout)) { layout_->require_execution(); }
 void Prepare(const torch::Device& device);
 [[nodiscard]] torch::Tensor ValidateLogits(const torch::Tensor& logits) const;
 [[nodiscard]] torch::Tensor References(const torch::Tensor& physical_indices) const;
 [[nodiscard]] std::size_t eligible_count() const noexcept { return layout_->eligible_count(); }

private:
 std::shared_ptr<const ResolvedClassLayout> layout_;
 torch::Tensor references_;
 cudaStream_t prepared_stream_ = nullptr;
};
// Fixed-capacity products contain a compact valid prefix described by device counts.
// Top-k products retain their originating query until the consumer selects survivors.
// Mask logits remain at model resolution; selection never expands them.
struct PostprocessedSelection {
 torch::Tensor scores{};
 torch::Tensor labels{};
 torch::Tensor boxes{};
 torch::Tensor query_indices{};
 std::optional<torch::Tensor> mask_logits{};
 torch::Tensor counts{};
};
PostprocessedSelection select_output_batch_fixed_size(const OutputTensors&, int64_t height, int64_t width, int64_t count, bool require_masks = false, ClassPostprocessLane* classes = nullptr);
// Complete concurrently live scratch: gathered logits, expanded logits, device
// bools, and host bools. Dense preview storage and encoded records are separate.
struct SelectedMaskCapacity final {
 std::array<std::size_t, 4U> bytes{};
 [[nodiscard]] static SelectedMaskCapacity Resolve(std::size_t count, std::size_t model_height, std::size_t model_width, std::size_t height, std::size_t width, std::size_t dtype_bytes) {
  const auto model_pixels = checked_prediction_extent(model_height, model_width, kMaximumPredictionTensorBytes);
  const auto pixels = checked_prediction_extent(height, width, kMaximumEncodedMaskPixels);
  const auto floating = [&](
                         std::size_t plane) { return checked_prediction_extent(count, checked_prediction_extent(plane, dtype_bytes, kMaximumPredictionTensorBytes), kMaximumPredictionTensorBytes); };
  const auto boolean = checked_prediction_extent(count, pixels, kMaximumPredictionTensorBytes);
  return {{floating(model_pixels), floating(pixels), boolean, boolean}};
 }
 [[nodiscard]] std::size_t Total() const {
  std::size_t result = 0U;
  for (const auto size : bytes) {
   if (size > std::numeric_limits<std::size_t>::max() - result) throw std::invalid_argument("RF-DETR mask scratch sum overflows");
   result += size;
  }
  return result;
 }
};
class SelectedMaskWorkspace final {
public:
 torch::Tensor Materialize(const torch::Tensor& logits, const torch::Tensor& query_indices, int64_t height, int64_t width);
 [[nodiscard]] SelectedMaskCapacity RetainedCapacity(std::size_t host_bytes) const;
 // The caller has completed the previous mask readback before trimming.
 void ResetSettled();

private:
 torch::Tensor gathered_, expanded_, masks_;
};
torch::Tensor materialize_selected_masks(const torch::Tensor& logits, const torch::Tensor& query_indices, int64_t height, int64_t width);
struct PostprocessedBatch {
 torch::Tensor scores;
 torch::Tensor labels;
 torch::Tensor boxes;
 std::optional<torch::Tensor> masks;
 torch::Tensor counts{};
 [[nodiscard]] int64_t size() const { return scores.defined() ? scores.size(0) : 0; }
};
PostprocessedBatch postprocess_output_batch_fixed_size(const OutputTensors& outputs, int64_t target_height, int64_t target_width, int64_t num_select, ClassPostprocessLane* classes = nullptr);
}  // namespace mmltk::backend::models::rfdetr
