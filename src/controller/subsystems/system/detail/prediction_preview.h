#pragma once
#include <array>
#include <functional>
#include <cstddef>
#include "src/backend/imaging/raster/chw_image.h"
#include "src/frameworks/gpu/cuda/cuda_context_scope.h"
#include <cstdint>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_document.h"
#include "src/controller/contracts/validation_display.h"
#include "src/controller/contracts/prediction_output.h"
#include "src/frameworks/gpu/runtime/system_image_runtime.h"
#include "src/frameworks/gpu/image/image_types.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
namespace mmltk::controller::detail {
[[nodiscard]] mmltk::frameworks::gpu::DeviceContext CreatePredictionPreviewContext(
 const mmltk::frameworks::gpu::DeviceExecution&, const std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>&, mmltk::frameworks::gpu::CudaContextApi = {});
// A slot owns raw model-space products. Only the visual worker draws it.
class PredictionPreviewFrame final : public std::enable_shared_from_this<PredictionPreviewFrame> {
public:
 ~PredictionPreviewFrame();
 PredictionPreviewFrame(const PredictionPreviewFrame&) = delete;
 PredictionPreviewFrame& operator=(const PredictionPreviewFrame&) = delete;
 [[nodiscard]] bool CompatibleWith(const mmltk::frameworks::gpu::SystemImageRuntime&) const noexcept;
 void Draw(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate&) const;
 [[nodiscard]] std::span<const mmltk::backend::models::rfdetr::Prediction> ground_truth() const noexcept;
 [[nodiscard]] std::span<const mmltk::backend::models::rfdetr::Prediction> predictions() const noexcept;
 [[nodiscard]] std::span<const std::string> classes() const noexcept;
 [[nodiscard]] const mmltk::backend::data::catalog::ClassCatalog& catalog() const noexcept;
 [[nodiscard]] int class_count() const noexcept;
 [[nodiscard]] int receiver_device() const noexcept;
 [[nodiscard]] VisualSkeleton Skeleton(const mmltk::backend::models::rfdetr::Prediction&, bool ground_truth = false) const;
 [[nodiscard]] std::shared_ptr<const VisualDocument> Document(contracts::WorkspaceResource, std::uint32_t frame_index, bool ground_truth = false) const;

private:
 friend class PredictionPreviewPool;
 friend class PredictionPreviewComposition;
 void DrawRegion(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t stream,
  bool prediction_boxes, bool prediction_masks, bool ground_truth_boxes, bool ground_truth_masks, bool complementary_layers, bool write_clean, bool write_semantic,
  std::optional<float> confidence_threshold, bool prediction_keypoints, bool ground_truth_keypoints, bool prediction_uncertainty, bool ground_truth_uncertainty) const;
 struct State;
 PredictionPreviewFrame(
  const mmltk::frameworks::gpu::DeviceContext&, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>, std::shared_ptr<void>, mmltk::frameworks::gpu::CudaContextApi);
 void RetainUnsafe(cudaError_t) const noexcept;
 [[nodiscard]] mmltk::frameworks::gpu::CudaContextScope ContextScope() const noexcept;
 [[nodiscard]] mmltk::frameworks::gpu::CudaContextScope CompositionScope() const noexcept;
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_;
 mutable mmltk::frameworks::gpu::TerminalCudaRetirementLease lease_;
 std::shared_ptr<State> state_;
};
// One transaction retains the exact drawing set until outer publication settles.
// Regions and options are renderer facts; sample-selection policy stays native to
// the domain. The composition admits at most 63 simultaneous raw frames.
class PredictionPreviewComposition final {
public:
 static constexpr std::size_t kMaximumFrames = 63U;
 struct Region final {
  std::shared_ptr<const PredictionPreviewFrame> frame;
  VisualRegion crop;
 };
 struct Options final {
  bool prediction_boxes = true, prediction_masks = true;
  bool ground_truth_boxes = false, ground_truth_masks = false;
  bool complementary_layers = false;
  bool atlas_padding = false;
  std::optional<float> confidence_threshold{};
  bool prediction_keypoints = true, ground_truth_keypoints = false;
  bool prediction_uncertainty = false, ground_truth_uncertainty = false;
  [[nodiscard]] static Options Prediction(const contracts::PredictionRunPreview& preview) {
   return {
    .prediction_boxes = preview.boxes,
    .prediction_masks = preview.masks,
    .confidence_threshold = preview.confidence_threshold,
    .prediction_keypoints = preview.keypoints,
    .prediction_uncertainty = preview.uncertainty
   };
  }
  [[nodiscard]] static Options Validation(const ValidationOverlays& overlays, float threshold, bool atlas = false) {
   return {
    .prediction_boxes = overlays.prediction_layer && overlays.prediction_boxes,
    .prediction_masks = overlays.prediction_layer && overlays.prediction_masks,
    .ground_truth_boxes = overlays.ground_truth_layer && overlays.ground_truth_boxes,
    .ground_truth_masks = overlays.ground_truth_layer && overlays.ground_truth_masks,
    .complementary_layers = true,
    .atlas_padding = atlas,
    .confidence_threshold = threshold,
    .prediction_keypoints = overlays.prediction_layer && overlays.prediction_keypoints,
    .ground_truth_keypoints = overlays.ground_truth_layer && overlays.ground_truth_keypoints,
    .prediction_uncertainty = overlays.prediction_layer && overlays.uncertainty,
    .ground_truth_uncertainty = overlays.ground_truth_layer && overlays.uncertainty
   };
  }
  bool operator==(const Options&) const = default;
 };
 static void Draw(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate&, VisualExtent, std::span<const Region>, Options,
  PredictionPreviewComposition* retained = nullptr, mmltk::frameworks::gpu::ImageProductBuffer::ProductSubmit finalize = {});

private:
 // Allocation-local validity only; weak source references never occupy raw slots.
 // The capture serial distinguishes reuse of a frame object by its pool.
 struct Cell final {
  std::weak_ptr<const PredictionPreviewFrame> frame;
  std::uint64_t capture = 0U;
  VisualRegion crop;
  Options options;
  bool clean = false, semantic = false;
 };
 struct Allocation final {
  mmltk::frameworks::gpu::ImageAllocation clean{}, semantic{};
  VisualExtent extent;
  bool padding = false, initialized = false;
  std::array<Cell, 6U> cells;
  std::size_t count = 0U;
 };
 std::array<Allocation, 2U> allocations_{};
 std::size_t replacement_ = 0U;
};
class PredictionPreviewPool final {
public:
 static constexpr std::size_t kSlotCapacity = 3U;
 struct TransferOperations final {
  decltype(&cuMemcpyPeerAsync) copy;
  decltype(&cudaEventRecord) record;
  decltype(&cudaStreamSynchronize) settle;
  decltype(&cuMemHostRegister) register_host;
  cudaError_t (*upload)(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t) = &cudaMemcpyAsync;
  mmltk::frameworks::gpu::CudaContextApi context_api{};
  decltype(&mmltk::backend::imaging::raster::chw_float_to_rgba) convert = &mmltk::backend::imaging::raster::chw_float_to_rgba;
  decltype(&cudaStreamWaitEvent) wait = &cudaStreamWaitEvent;
  decltype(&cudaMemset2DAsync) clear_semantic = &cudaMemset2DAsync;
  decltype(&mmltk::backend::imaging::raster::rgb8_to_rgba) convert_rgb8 = &mmltk::backend::imaging::raster::rgb8_to_rgba;
  mmltk::frameworks::gpu::DeviceContext (*source_context)(const mmltk::frameworks::gpu::DeviceExecution&) = [](const auto& execution) {
   namespace gpu = mmltk::frameworks::gpu;
   return gpu::DeviceContext(execution.device, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::PrimaryInterop, execution.placement.numa_node, execution);
  };
 };
 PredictionPreviewPool(mmltk::frameworks::gpu::DeviceExecution, mmltk::frameworks::gpu::DeviceContext);
 PredictionPreviewPool(mmltk::frameworks::gpu::DeviceExecution, mmltk::frameworks::gpu::DeviceContext, TransferOperations operations,
  std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {}, std::size_t slots = kSlotCapacity);
 [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(const float*, VisualExtent, std::uintptr_t source_stream, std::span<const mmltk::backend::models::rfdetr::Prediction>,
  const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&, std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>, int classes, const std::uint8_t* rgb8 = nullptr,
  std::shared_ptr<void> source_custody = {}, void (*stop_source)(void*) = nullptr, void* source_control = nullptr, std::span<const mmltk::backend::models::rfdetr::Prediction> ground_truth = {},
  bool composition = false, int source_device = -1);
 [[nodiscard]] bool HasUnsafeSourceCustody() const noexcept { return unsafe_source_; }
 [[nodiscard]] bool HasUnsafeCustody() const noexcept { return !retirement_->admission_open(); }

private:
 bool unsafe_source_ = false;
 TransferOperations operations_;
 mmltk::frameworks::gpu::DeviceExecution execution_;
 mmltk::frameworks::gpu::DeviceContext context_;
 std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_;
 std::vector<std::shared_ptr<PredictionPreviewFrame>> slots_;
};
}  // namespace mmltk::controller::detail
