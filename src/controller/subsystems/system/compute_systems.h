#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <stop_token>
#include <variant>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/workspace_input.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_source_projection.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::controller {

struct ValidationRuntimeResult final {
    contracts::ComputeTerminal terminal{};
    std::optional<mmltk::backend::models::rfdetr::ValidationBackendResult> evaluation;
};
class ValidationRuntime {
   public:
    virtual ~ValidationRuntime() = default;
    [[nodiscard]] virtual ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                         const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&) = 0;
};
class ExportRuntime {
   public:
    virtual ~ExportRuntime() = default;
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token,
                                                         const ComputeProgressSink&) = 0;
};
class CudaValidationRuntime final : public ValidationRuntime {
   public:
    explicit CudaValidationRuntime(DirectComputeConfiguration);
    ~CudaValidationRuntime() override;
    [[nodiscard]] ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                 const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
class CudaExportRuntime final : public ExportRuntime {
   public:
    explicit CudaExportRuntime(DirectComputeConfiguration);
    ~CudaExportRuntime() override;
    [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token,
                                                 const ComputeProgressSink&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ComputeProgressEvent final {
    contracts::ComputeUiState snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ComputeChanged final {
    contracts::ComputeUiState snapshot{};
};
using ComputeSystemEvent = std::variant<ComputeProgressEvent, ComputeChanged>;

using ValidationRuntimeFactory = std::function<std::unique_ptr<ValidationRuntime>()>;
using ExportRuntimeFactory = std::function<std::unique_ptr<ExportRuntime>()>;

struct ValidationSampleIdentity final {
    std::uint64_t generation = 0U;
    std::uint32_t dataset_index = 0U;
    auto operator<=>(const ValidationSampleIdentity&) const = default;
};
struct ValidationLabel final {
    contracts::AnnotationBox box{};
    contracts::AnnotationColor color{};
    std::uint32_t category = 0U;
    bool ground_truth = false;
    float confidence = 0.0F;
    [[= mmltk::frameworks::reflection::MaxBytes{256U}]] std::string name;
};
struct ValidationSampleMetadata final {
    ValidationSampleIdentity identity{};
    bool available = false;
    VisualRegion crop{};
    VisualExtent original_extent{};
    [[= mmltk::frameworks::reflection::MaxItems{2U * contracts::kAnnotationObjectCapacity}]] std::vector<ValidationLabel> labels;
};
struct ValidationOverlays final {
    bool prediction_boxes = true, prediction_masks = true;
    bool ground_truth_boxes = true, ground_truth_masks = true;
    bool operator==(const ValidationOverlays&) const = default;
};
struct ValidationImageMetadata {
    VisualFrame frame{};
    ValidationOverlays overlays{};
    std::uint64_t content_identity = 0U;
    bool detail = false;
    std::array<ValidationSampleMetadata, 6> samples{};
};
struct ValidationSnapshot final {
    VisualFrame frame{};
    std::uint64_t content_identity = 0U;
    bool detail = false;
    std::array<ValidationSampleIdentity, 6> sample_identities{};
    std::array<bool, 6> sample_available{};
    contracts::ComputeUiState operation{};
    std::optional<mmltk::backend::models::rfdetr::EvalSummary> metrics;
    std::uint32_t detail_rows = 0U;
    ValidationOverlays overlays{};
};
struct [[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ValidationChanged final { ValidationSnapshot snapshot{}; };
struct [[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ValidationProgress final { contracts::ComputeUiState operation{}; };

class ValidationSystem final {
   public:
    using event_type = std::variant<ValidationChanged, ValidationProgress>;
    using visual_source = VisualSourceProjection<ValidationSnapshot, PresentationSourceKind::Validation,
        mmltk::frameworks::reflection::member_path<&ValidationSnapshot::frame>,
        mmltk::frameworks::reflection::member_path<&ValidationSnapshot::frame, &VisualFrame::revision>, ValidationImageMetadata>;
    ValidationSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ValidationRuntimeFactory, SystemEventSink<event_type> = {},
                     std::optional<mmltk::frameworks::gpu::DeviceExecution> = {}, VisualDeviceSettings = {});
    ~ValidationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot Start(contracts::ValidateWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot Stop() noexcept;
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot SelectSample(ValidationSampleIdentity);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot CloseDetail();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] ValidationSnapshot SetOverlays(ValidationOverlays);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::EvaluationDetailPage Details(mmltk::backend::models::rfdetr::EvaluationDetailQuery) const;
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
MMLTK_REFLECT_FIELDS(ValidationSampleIdentity)
MMLTK_REFLECT_FIELDS(ValidationLabel)
MMLTK_REFLECT_FIELDS(ValidationSampleMetadata)
MMLTK_REFLECT_FIELDS(ValidationOverlays)
MMLTK_REFLECT_FIELDS(ValidationImageMetadata)
MMLTK_REFLECT_FIELDS(ValidationSnapshot)
MMLTK_REFLECT_FIELDS(ValidationChanged)
MMLTK_REFLECT_FIELDS(ValidationProgress)
class ExportSystem final {
   public:
    using event_type = ComputeSystemEvent;
    ExportSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ExportRuntimeFactory, SystemEventSink<event_type> = {},
                 std::optional<mmltk::frameworks::gpu::DeviceExecution> = {});
    ~ExportSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Start(contracts::ExportWorkflowIntent);
    // CLEANUP-IGNORE: Export exposes its own reflected typed action surface; Validation remains an independently sealed
    // system.
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] contracts::ComputeUiState snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

MMLTK_REFLECT_FIELDS(ComputeProgressEvent)
MMLTK_REFLECT_FIELDS(ComputeChanged)

}  // namespace mmltk::controller
