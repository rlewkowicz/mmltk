#pragma once
#include <array>
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
// A slot owns raw model-space products. Only the visual worker draws it.
class PredictionPreviewFrame final {
   public:
    ~PredictionPreviewFrame();
    PredictionPreviewFrame(const PredictionPreviewFrame&) = delete;
    PredictionPreviewFrame& operator=(const PredictionPreviewFrame&) = delete;
    [[nodiscard]] bool CompatibleWith(const mmltk::frameworks::gpu::SystemImageRuntime&) const noexcept;
    void Draw(mmltk::frameworks::gpu::SystemImageRuntime&, mmltk::frameworks::gpu::SystemImageRuntime::OutputCandidate&) const;
    [[nodiscard]] std::span<const mmltk::backend::models::rfdetr::Prediction> predictions() const noexcept;
    [[nodiscard]] const std::vector<std::string>& classes() const noexcept;
    [[nodiscard]] int class_count() const noexcept;
   private:
    friend class PredictionPreviewPool;
    struct State;
    PredictionPreviewFrame(const mmltk::frameworks::gpu::DeviceExecution&, const mmltk::frameworks::gpu::DeviceContext&, std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>);
    std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_;
    mmltk::frameworks::gpu::TerminalCudaRetirementLease lease_;
    std::shared_ptr<State> state_;
};
class PredictionPreviewPool final {
   public:
    struct TransferOperations final {
        decltype(&cuMemcpyPeerAsync) copy;
        decltype(&cudaEventRecord) record;
        decltype(&cudaStreamSynchronize) settle;
        decltype(&cuMemHostRegister) register_host;
    };
    PredictionPreviewPool(mmltk::frameworks::gpu::DeviceExecution, mmltk::frameworks::gpu::DeviceContext,
                          TransferOperations operations = {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister});
    [[nodiscard]] std::shared_ptr<const PredictionPreviewFrame> Capture(
        const float*, VisualExtent, std::uintptr_t source_stream,
        std::span<const mmltk::backend::models::rfdetr::Prediction>,
        const mmltk::backend::ml::runtime::AnalysisAnnotationStorage&, std::shared_ptr<const std::vector<std::string>>, int classes,
        const std::uint8_t* rgb8 = nullptr, std::shared_ptr<void> source_custody = {}, void (*stop_source)(void*) = nullptr, void* source_control = nullptr);
   private:
    TransferOperations operations_;
    mmltk::frameworks::gpu::DeviceExecution execution_;
    mmltk::frameworks::gpu::DeviceContext context_;
    std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> retirement_ = std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(3U);
    std::array<std::shared_ptr<PredictionPreviewFrame>, 3U> slots_{};
};
}
