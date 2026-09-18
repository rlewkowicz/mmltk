#pragma once
#include <array>
#include <cstddef>
#include "src/backend/imaging/raster/chw_image.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include <cstdint>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::controller::detail {
[[nodiscard]] mmltk::frameworks::gpu::DeviceContext CreatePredictionPreviewContext(const mmltk::frameworks::gpu::DeviceExecution&,
                                                                                   const std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>&,
                                                                                   mmltk::frameworks::gpu::CudaContextApi = {});
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
    [[nodiscard]] int class_count() const noexcept;

   private:
    friend class PredictionPreviewPool;
    friend class PredictionPreviewComposition;
    void DrawRegion(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic,
                    std::uintptr_t stream, bool prediction_boxes, bool prediction_masks, bool ground_truth_boxes, bool ground_truth_masks, bool complementary_layers) const;
    struct State;
    PredictionPreviewFrame(const mmltk::frameworks::gpu::DeviceContext&, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>,
                           std::shared_ptr<void>, mmltk::frameworks::gpu::CudaContextApi);
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
    };
    static void Draw(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate&, VisualExtent,
                     std::span<const Region>, Options);
};
class PredictionPreviewPool final {
   public:
    static constexpr std::size_t kSlotCapacity = 3U;
    struct TransferOperations final {
        decltype(&cuMemcpyPeerAsync) copy;
        decltype(&cudaEventRecord) record;
        decltype(&cudaStreamSynchronize) settle;
        decltype(&cuMemHostRegister) register_host;
        decltype(&cudaMemcpyAsync) upload = &cudaMemcpyAsync;
        mmltk::frameworks::gpu::CudaContextApi context_api{};
        decltype(&mmltk::backend::imaging::raster::chw_float_to_rgba) convert = &mmltk::backend::imaging::raster::chw_float_to_rgba;
        decltype(&cudaStreamWaitEvent) wait = &cudaStreamWaitEvent;
    };
    PredictionPreviewPool(mmltk::frameworks::gpu::DeviceExecution, mmltk::frameworks::gpu::DeviceContext);
    PredictionPreviewPool(mmltk::frameworks::gpu::DeviceExecution, mmltk::frameworks::gpu::DeviceContext, TransferOperations operations,
                          std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement = {}, std::size_t slots = kSlotCapacity);
    [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(
        const float*, VisualExtent, std::uintptr_t source_stream, std::span<const mmltk::backend::models::rfdetr::Prediction>,
        const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&, std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>, int classes,
        const std::uint8_t* rgb8 = nullptr, std::shared_ptr<void> source_custody = {}, void (*stop_source)(void*) = nullptr, void* source_control = nullptr,
        std::span<const mmltk::backend::models::rfdetr::Prediction> ground_truth = {}, bool composition = false);
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
