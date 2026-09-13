#pragma once

#include "src/controller/presentation/visual_source_projection.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include "src/frameworks/gpu/device_execution.h"
#include <stop_token>
#include <variant>
#include <vector>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/system_events.h"

namespace mmltk::controller {

struct DirectComputeConfiguration final {
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution;
    [[nodiscard]] bool valid() const noexcept {
        return execution && execution->device >= 0 && execution->placement.numa_node >= 0 && !execution->placement.cpus.empty();
    }
};

using ComputeProgressSink = std::function<void(const contracts::ComputeProgress&)>;

class ValidationRuntime {
   public:
    virtual ~ValidationRuntime() = default;
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                         const ComputeProgressSink&) = 0;
};
class ExportRuntime {
   public:
    virtual ~ExportRuntime() = default;
    [[nodiscard]] virtual contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token,
                                                         const ComputeProgressSink&) = 0;
};
class PredictRuntime {
   public:
    struct Box final {
        float x1 = 0.0F;
        float y1 = 0.0F;
        float x2 = 0.0F;
        float y2 = 0.0F;
    };
    struct Product final {
        contracts::ComputeTerminal terminal{};
        VisualExtent extent{};
        std::vector<std::uint8_t> rgba{};
        std::vector<Box> boxes{};
    };
    virtual ~PredictRuntime() = default;
    [[nodiscard]] virtual Product Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&) = 0;
};

class CudaValidationRuntime final : public ValidationRuntime {
   public:
    explicit CudaValidationRuntime(DirectComputeConfiguration);
    ~CudaValidationRuntime() override;
    [[nodiscard]] contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token,
                                                 const ComputeProgressSink&) override;

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
class CudaPredictRuntime final : public PredictRuntime {
   public:
    explicit CudaPredictRuntime(DirectComputeConfiguration);
    ~CudaPredictRuntime() override;
    [[nodiscard]] Product Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&) override;

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
using PredictRuntimeFactory = std::function<std::unique_ptr<PredictRuntime>()>;

class ValidationSystem final {
   public:
    using event_type = ComputeSystemEvent;
    ValidationSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ValidationRuntimeFactory, SystemEventSink<event_type> = {},
                     std::optional<mmltk::frameworks::gpu::DeviceExecution> = {});
    ~ValidationSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Start(contracts::ValidateWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ComputeUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] contracts::ComputeUiState snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
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
struct PredictSnapshot final {
    std::uint64_t revision = 0U;
    // CLEANUP-IGNORE: Predict composes compute facts with its private visual frame in one canonical snapshot.
    contracts::ComputeUiState operation{};
    VisualFrame frame{};
    // CLEANUP-IGNORE: PredictSnapshot is a distinct reflected boundary type.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] PredictProgress final {
    // CLEANUP-IGNORE: Predict progress is an independently identified transient event.
    PredictSnapshot snapshot{};
    // CLEANUP-IGNORE: The transient event terminator intentionally mirrors other snapshot events.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PredictChanged final {
    PredictSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] PredictFailed final {
    PredictSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{kVisualFailureByteCapacity}]] std::string detail;
};
class PredictSystem final {
   public:
    using visual_source = VisualSourceProjection<PredictSnapshot, PresentationSourceKind::Predict,
                                                 mmltk::frameworks::reflection::member_path<&PredictSnapshot::frame>,
                                                 mmltk::frameworks::reflection::member_path<&PredictSnapshot::revision>>;
    using event_type = std::variant<PredictProgress, PredictChanged, PredictFailed>;
    PredictSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, VisualDeviceSettings, PredictRuntimeFactory,
                  SystemEventSink<event_type> = {});
    ~PredictSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Start(contracts::PredictWorkflowIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] PredictSnapshot Stop(contracts::PredictWorkflowIntent) noexcept;
    // CLEANUP-IGNORE: Predict shutdown is ordinary facade forwarding for this sealed system.
    void Shutdown() noexcept;
    // CLEANUP-IGNORE: Predict owns its reflected snapshot and public read surface independently of Live and Annotation.
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] PredictSnapshot snapshot() const;
    [[nodiscard]] std::optional<VisualImageMetadata> ImageSnapshot(const VisualFrame&) const;
    // CLEANUP-IGNORE: Predict retains a sealed raw/display read API backed by the shared private VisualRuntimeOwner.
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const;
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const;
    void RequestWorkspace(VisualWorkspaceRequest);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

MMLTK_REFLECT_FIELDS(ComputeProgressEvent)
MMLTK_REFLECT_FIELDS(ComputeChanged)
MMLTK_REFLECT_FIELDS(PredictSnapshot)
MMLTK_REFLECT_FIELDS(PredictProgress)
MMLTK_REFLECT_FIELDS(PredictChanged)
MMLTK_REFLECT_FIELDS(PredictFailed)

}  // namespace mmltk::controller
