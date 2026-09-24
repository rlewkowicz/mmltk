#pragma once
#include "src/controller/services/runtime_diagnostics.h"
#include <functional>
#include "export_run.h"
#include <memory>
#include <optional>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/frameworks/gpu/device_execution.h"
#include <stop_token>
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/subsystems/system/compute_runtime.h"
namespace mmltk::controller {
class SettingsSystem;
class DatasetSystem;
class ModelSystem;
class ExportRuntime {
public:
 virtual ~ExportRuntime() = default;
 // Complete physical retirement; successful repeated Close is inert.
 virtual void Close() {}
 [[nodiscard]] virtual bool HasUnsafeCustody() const noexcept { return false; }
 [[nodiscard]] virtual contracts::ComputeTerminal Run(ExportRunRequest, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink& = {}, std::uint64_t generation = 0) = 0;
};
class CudaExportRuntime final : public ExportRuntime {
public:
 explicit CudaExportRuntime(DirectComputeConfiguration, services::RuntimeDiagnosticTarget = {});
 ~CudaExportRuntime() override;
 void Close() override;
 [[nodiscard]] bool HasUnsafeCustody() const noexcept override;
 [[nodiscard]] contracts::ComputeTerminal Run(ExportRunRequest, std::stop_token, const ComputeProgressSink&, const ComputeArtifactSink& = {}, std::uint64_t generation = 0) override;

private:
 services::RuntimeDiagnosticTarget diagnostics_;
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
using ExportRuntimeFactory = std::function<std::unique_ptr<ExportRuntime>(DirectComputeConfiguration)>;
class ExportSystem final {
public:
 using event_type = ComputeSystemEvent;
 ExportSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, ExportRuntimeFactory, SystemEventSink<event_type> = {}, DirectComputeResolver = resolve_compute_configuration);
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
