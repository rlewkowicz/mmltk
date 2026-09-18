#pragma once
#include <memory>
#include <functional>
#include <optional>
#include <span>
#include "src/controller/subsystems/validate/validation_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
namespace mmltk::controller::detail {
// Domain storage and rendering only. The validation session remains with its
// existing CudaSessionRuntimeState execution owner.
class ValidationSamples final {
   public:
    ValidationSamples(VisualDeviceSettings, std::function<void()> changed,
                      PredictionPreviewPool::TransferOperations = {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister});
    ~ValidationSamples();
    void Begin(std::uint64_t generation, std::span<const std::uint32_t> indices);
    void Capture(std::uint64_t generation, mmltk::backend::models::rfdetr::ValidationSampleView);
    void Settle(std::uint64_t generation, bool succeeded);
    void Select(ValidationSampleIdentity);
    void CloseDetail();
    void SetOverlays(ValidationOverlays);
    [[nodiscard]] ValidationSnapshot snapshot() const;
    [[nodiscard]] std::optional<ValidationImageMetadata> ImageSnapshot(const VisualFrame&) const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame&) const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);
    void Shutdown() noexcept;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::controller::detail
