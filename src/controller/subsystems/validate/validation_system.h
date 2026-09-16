#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/frameworks/gpu/device_execution.h"
#include "validation_types.h"
#include "src/controller/contracts/workspace_input.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_source_projection.h"
namespace mmltk::controller {
class SettingsSystem;
class DatasetSystem;
class ModelSystem;
class ValidationRuntime;
using ValidationRuntimeFactory = std::function<std::unique_ptr<ValidationRuntime>()>;
class ValidationSystem final {
   public:
    using event_type = std::variant<ValidationChanged, ValidationProgress>;
    using visual_source =
        VisualSourceProjection<ValidationSnapshot, PresentationSourceKind::Validation, mmltk::frameworks::reflection::member_path<&ValidationSnapshot::frame>,
                               mmltk::frameworks::reflection::member_path<&ValidationSnapshot::frame, &VisualFrame::revision>, ValidationImageMetadata>;
    ValidationSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ValidationRuntimeFactory, SystemEventSink<event_type> = {},
                     std::optional<mmltk::frameworks::gpu::DeviceExecution> = {}, VisualDeviceSettings = {});
    ~ValidationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot Start(contracts::ValidateWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot Stop() noexcept;
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot SelectSample(ValidationSampleIdentity);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot CloseDetail();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot SetOverlays(ValidationOverlays);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::EvaluationDetailPage Details(
        mmltk::backend::models::rfdetr::EvaluationDetailQuery) const;
    [[= contracts::reflection::direct::InteractionEndpoint{}]] void Input(WorkspaceMouse);
    void SetInputPeer(std::uint64_t);
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] ValidationSnapshot snapshot() const;
    [[nodiscard]] std::optional<ValidationImageMetadata> ImageSnapshot(const VisualFrame&) const;
    [[nodiscard]] VisualSourceObservation ObserveSource() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::controller
