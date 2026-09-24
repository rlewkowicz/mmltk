#include "export_system.h"
#include "src/controller/services/run_output.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/detail/cuda_runtime_resources.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/runtime/local_run.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/image_failure.h"
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.models.rfdetr.inference.runtime_backend;
namespace mmltk::controller {
class CudaExportRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ExportOnnxSession> {
public:
 using CudaSessionRuntimeState::CudaSessionRuntimeState;
};
CudaExportRuntime::CudaExportRuntime(DirectComputeConfiguration configuration, services::RuntimeDiagnosticTarget diagnostics) : diagnostics_(std::move(diagnostics)), impl_(std::make_unique<Impl>(configuration)) {}
CudaExportRuntime::~CudaExportRuntime() = default;
void CudaExportRuntime::Close() { impl_->resources.CloseSession(); }
bool CudaExportRuntime::HasUnsafeCustody() const noexcept { return impl_->resources.HasUnsafeCustody(); }
contracts::ComputeTerminal CudaExportRuntime::Run(ExportRunRequest operation, std::stop_token stop, const ComputeProgressSink& progress, const ComputeArtifactSink& published, std::uint64_t generation) {
 return impl_->resources.Run(
  [this, generation, stop, &progress, &published, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
   if (operation.onnx.device_id != impl_->resources.device()) throw std::invalid_argument("export device disagrees with admitted execution");
    diagnostics_.Emit([&] {
     return services::RuntimeDiagnosticFact{.owner = contracts::DiagnosticOwner::Export, .event = "workflow.gpu_execution", .participant = "export",
      .sequence = generation, .value = 0U, .detail = 1U, .device = impl_->resources.device()};
    });
   return execute_export_run(
    operation, stop, progress, published, [&](const auto& request, const auto& committed) { impl_->session.Run(request, stream, stop, committed); },
    [&](const auto& request, const auto& committed) { mmltk::backend::models::rfdetr::build_tensorrt_engine(request, stream, {}, stop, committed); });
  },
  stop);
}
class ExportSystem::Impl final {
public:
 Impl(SettingsSystem& settings, DatasetSystem&, ModelSystem& model, ExportRuntimeFactory factory, SystemEventSink<ExportSystem::event_type> events,
  DirectComputeResolver resolver)
     : settings_(settings), model_(model), factory_(std::move(factory)), events_(std::move(events)), resolver_(std::move(resolver)) {
  if (!factory_ || !resolver_) throw contracts::UnavailableError("compute runtime factory or placement resolver is unavailable");
  // CLEANUP-IGNORE: Export and validation have distinct admission and worker preparation after this common settings guard.
 }
 ~Impl() { Shutdown(); }
 [[nodiscard]] contracts::ComputeUiState Start() {
  if (run_.active()) throw contracts::BusyError("compute operation is active");
  if (retirement_failed_) throw contracts::UnavailableError("compute session retirement failed");
  const auto settings = settings_.materialization_facts();
  if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
  const auto selection = model_.selection();
  auto prepared = subsystems::system::ComputeIntentMaterializer::Export(settings.settings, {}, selection);
  if (!prepared) throw contracts::InvalidIntentError(prepared.error().detail);
  const auto configuration = resolver_(prepared->onnx.device_id, -1);
  // CLEANUP-IGNORE: LocalRun owns execution; this prefix supplies Export-specific state admission before its distinct worker body.
  run_.Start({
   .policy = configuration.worker_policy(),
   .prepare =
    [this] {
     std::scoped_lock lock(mutex_);
     const auto next = contracts::next_compute_generation(state_.generation_frontier);
     if (!next) throw contracts::FailedError("compute operation generation exhausted");
     contracts::begin_compute(state_, *next, {});
    },
   .work = [this, prepared = std::move(prepared), output = settings.settings.workflows.export_state.output, configuration](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
    auto terminal = run_checked_compute(
     [&](const ComputeProgressSink& progress) {
      if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
      const auto directory = services::reserve_run_output(contracts::workflow_output_root(output, contracts::FeatureId::Export), !output.automatic);
      prepared->output_directory = directory;
      {
       std::scoped_lock lock(mutex_);
       state_.output.directory = directory.string();
      }
      direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeChanged{snapshot()}}; });
      if (runtime_ && runtime_configuration_ != configuration) {
       RetireRuntime();
       if (retirement_failed_) throw contracts::UnavailableError("compute CUDA retirement is unproved");
      }
      if (!runtime_) {
       try { runtime_ = factory_(configuration); }
       catch (...) {
        if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) retirement_failed_ = true;
        throw;
       }
       runtime_configuration_ = configuration;
      }
      if (!runtime_) throw std::runtime_error("compute runtime is unavailable");
      // CLEANUP-IGNORE: Export owns retirement and publication; LocalRun and run_checked_compute already share execution.
      const auto generation = [this] { std::scoped_lock lock(mutex_); return state_.generation_frontier; }();
      return runtime_->Run(std::move(*prepared), stop, progress, [this](const auto& path) { Artifact(path); }, generation);
     },
     [this](const contracts::ComputeProgress& progress) { Progress(progress); });
    if (terminal.outcome == contracts::ComputeOperationOutcome::Failed || (runtime_ && runtime_->HasUnsafeCustody())) RetireRuntime();
    return Complete(std::move(terminal));
   },
   .failure = [this](const std::exception_ptr failure) -> direct::LocalRun::Notification {
    RetireRuntime();
    return Complete(contracts::compute_failure_terminal(failure, "compute worker failed"));
   },
  });
  return snapshot();
 }
 // CLEANUP-IGNORE: Independent systems lock their own state and forward cancellation/join to LocalRun and the shared compute transitions.
 [[nodiscard]] contracts::ComputeUiState Stop() noexcept {
  static_cast<void>(run_.Stop());
  std::scoped_lock lock(mutex_);
  contracts::cancel_compute(state_);
  return state_;
 }
 void RetireRuntime() noexcept {
  if (!retirement_failed_ && !retire_compute_runtime(runtime_)) retirement_failed_ = true;
 }
 void Shutdown() noexcept {
  static_cast<void>(Stop());
  run_.StopAndJoin();
  RetireRuntime();
 }
 [[nodiscard]] contracts::ComputeUiState snapshot() const {
  std::scoped_lock lock(mutex_);
  return state_;
 }

private:
 void Artifact(const std::filesystem::path& path) {
  {
   std::scoped_lock lock(mutex_);
   if (state_.output.artifacts.size() == contracts::kWorkflowArtifactCapacity) throw std::runtime_error("export artifact publication capacity exhausted");
   state_.output.artifacts.push_back(path);
  }
  direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeChanged{snapshot()}}; });
 }
 [[nodiscard]] direct::LocalRun::Notification Complete(contracts::ComputeTerminal terminal) {
  contracts::ComputeUiState settled;
  {
   std::scoped_lock lock(mutex_);
   terminal.generation = state_.generation_frontier;
   contracts::complete_compute(state_, std::move(terminal));
   settled = state_;
  }
  return [this, settled = std::move(settled)]() mutable noexcept { direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeChanged{std::move(settled)}}; }); };
 }
 void Progress(const contracts::ComputeProgress& progress) noexcept {
  contracts::ComputeUiState snapshot;
  {
   std::scoped_lock lock(mutex_);
   if (!contracts::advance_compute(state_, progress)) return;
   snapshot = state_;
  }
  direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeProgressEvent{std::move(snapshot)}}; });
 }
 SettingsSystem& settings_;
 ModelSystem& model_;
 ExportRuntimeFactory factory_;
 SystemEventSink<ExportSystem::event_type> events_;
 mutable std::mutex mutex_;
 contracts::ComputeUiState state_{};
 std::unique_ptr<ExportRuntime> runtime_;
 DirectComputeResolver resolver_;
 std::optional<DirectComputeConfiguration> runtime_configuration_;
 std::atomic_bool retirement_failed_ = false;
 direct::LocalRun run_;
};
ExportSystem::ExportSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ExportRuntimeFactory factory, SystemEventSink<event_type> events,
 DirectComputeResolver resolver)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(factory), std::move(events), std::move(resolver))) {}
ExportSystem::~ExportSystem() = default;
contracts::ComputeUiState ExportSystem::Start(contracts::ExportWorkflowIntent) { return impl_->Start(); }
contracts::ComputeUiState ExportSystem::Stop() noexcept { return impl_->Stop(); }
void ExportSystem::Shutdown() noexcept { impl_->Shutdown(); }
contracts::ComputeUiState ExportSystem::snapshot() const { return impl_->snapshot(); }
}  // namespace mmltk::controller
