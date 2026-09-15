#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <variant>

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::controller {

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
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ComputeProgressEvent final {
    contracts::ComputeUiState snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ComputeChanged final {
    contracts::ComputeUiState snapshot{};
};
using ComputeSystemEvent = std::variant<ComputeProgressEvent, ComputeChanged>;

using ValidationRuntimeFactory = std::function<std::unique_ptr<ValidationRuntime>()>;
using ExportRuntimeFactory = std::function<std::unique_ptr<ExportRuntime>()>;

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

MMLTK_REFLECT_FIELDS(ComputeProgressEvent)
MMLTK_REFLECT_FIELDS(ComputeChanged)

}  // namespace mmltk::controller
