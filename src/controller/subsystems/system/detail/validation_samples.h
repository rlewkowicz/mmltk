#pragma once
#include <memory>
#include <functional>
#include <optional>
#include <span>
#include "src/controller/subsystems/system/compute_systems.h"
#include "prediction_preview.h"
namespace mmltk::controller::detail {
// Domain storage and rendering only. The validation session remains with its
// existing CudaSessionRuntimeState execution owner.
class ValidationSamples final {
 public:
    ValidationSamples(VisualDeviceSettings, std::function<void()> changed,
        PredictionPreviewPool::TransferOperations = {&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister});
    ~ValidationSamples();
    void Begin(std::uint64_t generation, std::span<const std::uint32_t> indices);
    void Capture(mmltk::backend::models::rfdetr::ValidationSampleView);
    void Select(ValidationSampleIdentity);
    void CloseDetail();
    void SetOverlays(ValidationOverlays);
    [[nodiscard]] ValidationSnapshot snapshot() const;
    [[nodiscard]] std::optional<ValidationImageMetadata> ImageSnapshot(const VisualFrame&) const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);
    void Shutdown() noexcept;
 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
