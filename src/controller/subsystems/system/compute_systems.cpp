#include "compute_systems.h"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "detail/cuda_runtime_resources.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/system_events.h"

import mmltk.backend.models.rfdetr.model_export;

namespace mmltk::controller {

namespace {

using MaterializationRefusal = subsystems::system::ComputeIntentMaterializer::Refusal;

template <class Request>
using ComputeMaterializer = std::move_only_function<std::expected<Request, MaterializationRefusal>(
    const contracts::GuiSettingsState&, const contracts::ArtifactInspection&, const contracts::ModelSelection&)>;

template <class Request, class Runtime>
class ComputeSystemCore final {
   public:
    using RuntimeFactory = std::function<std::unique_ptr<Runtime>()>;

    ComputeSystemCore(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, RuntimeFactory factory,
                      SystemEventSink<ComputeSystemEvent> events, ComputeMaterializer<Request> materialize,
                      std::optional<mmltk::frameworks::gpu::DeviceExecution> execution)
        : settings_(settings),
          dataset_(dataset),
          model_(model),
          factory_(std::move(factory)),
          events_(std::move(events)),
          materialize_(std::move(materialize)),
          execution_(std::move(execution)) {
        if (!factory_) throw contracts::UnavailableError("compute runtime factory is unavailable");
    }
    [[nodiscard]] contracts::ComputeUiState Start() {
        // CLEANUP-IGNORE: ComputeSystemCore begins from canonical settings before its typed LocalRun path.
        const auto settings = settings_.materialization_facts();
        if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
        // CLEANUP-IGNORE: ComputeSystemCore materializes its own typed operation before LocalRun admission.
        const auto selection = model_.selection();
        std::optional<Request> prepared;
        if constexpr (!std::same_as<Request, mmltk::backend::models::rfdetr::ValidateRequest>) {
            auto operation = materialize_(settings.settings, {}, selection);
            if (!operation) throw contracts::InvalidIntentError(operation.error().detail);
            prepared = std::move(*operation);
        }
        run_.Start({
            .policy = execution_ ? std::optional<mmltk::common::system::ExecutionPolicyRequest>{{execution_->placement.cpus,
                                                                                                 {},
                                                                                                 0,
                                                                                                 execution_->placement.numa_node,
                                                                                                 -10,
                                                                                                 false}}
                                 : std::nullopt,
            .prepare =
                [this] {
                    std::scoped_lock lock(mutex_);
                    const auto next = contracts::next_compute_generation(state_.generation_frontier);
                    if (!next) throw contracts::FailedError("compute operation generation exhausted");
                    state_.generation_frontier = *next;
                    state_.active = true;
                    state_.progress = {};
                    state_.terminal =
                        contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Running, state_.generation_frontier);
                    if constexpr (std::same_as<Request, mmltk::backend::models::rfdetr::ValidateRequest>)
                        state_.terminal.detail = "Inspecting selected inputs";
                },
            .work = [this, settings = settings.settings, selection, prepared = std::move(prepared)](
                        const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                contracts::ComputeTerminal terminal;
                bool failed = false;
                std::atomic_bool malformed_progress = false;
                try {
                    if (stop.stop_requested())
                        return Complete(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled));
                    if constexpr (std::same_as<Request, mmltk::backend::models::rfdetr::ValidateRequest>) {
                        const auto inspection = dataset_.Inspect({settings.workflows.validate.request.compiled_path, {}, {}},
                                                                                           selection.key.preset, selection.key.resolution, stop);
                        if (stop.stop_requested())
                            return Complete(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled));
                        auto operation = materialize_(settings, inspection, selection);
                        if (!operation) throw contracts::InvalidIntentError(operation.error().detail);
                        prepared = std::move(*operation);
                    }
                    if (stop.stop_requested())
                        return Complete(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled));
                    if (!runtime_) runtime_ = factory_();
                    if (!runtime_) throw std::runtime_error("compute runtime is unavailable");
                    terminal = runtime_->Run(std::move(*prepared), stop, [this, &malformed_progress](const contracts::ComputeProgress& p) {
                        if (!p.valid()) {
                            malformed_progress.store(true, std::memory_order_relaxed);
                            return;
                        }
                        Progress(p);
                    });
                    if (malformed_progress.load(std::memory_order_relaxed) || !terminal.valid_worker_terminal())
                        throw std::runtime_error("compute runtime returned an invalid terminal");
                    failed = terminal.outcome == contracts::ComputeOperationOutcome::Failed;
                } catch (...) {
                    failed = true;
                    terminal = contracts::compute_failure_terminal(std::current_exception(), "compute runtime failed");
                }
                if (failed) runtime_.reset();
                return Complete(std::move(terminal));
            },
            .failure = [this](const std::exception_ptr failure) -> direct::LocalRun::Notification {
                runtime_.reset();
                return Complete(contracts::compute_failure_terminal(failure, "compute worker failed"));
            },
        });
        return snapshot();
    }
    [[nodiscard]] contracts::ComputeUiState Stop() noexcept {
        static_cast<void>(run_.Stop());
        std::scoped_lock lock(mutex_);
        if (state_.active)
            state_.terminal =
                contracts::make_compute_terminal(contracts::ComputeOperationOutcome::CancellationRequested, state_.generation_frontier);
        return state_;
    }
    void Shutdown() noexcept {
        static_cast<void>(Stop());
        run_.StopAndJoin();
    }
    [[nodiscard]] contracts::ComputeUiState snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }

   private:
    [[nodiscard]] direct::LocalRun::Notification Complete(contracts::ComputeTerminal terminal) {
        contracts::ComputeUiState settled;
        {
            std::scoped_lock lock(mutex_);
            terminal.generation = state_.generation_frontier;
            state_.active = false;
            state_.terminal = std::move(terminal);
            settled = state_;
        }
        return [this, settled = std::move(settled)]() mutable noexcept {
            direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeChanged{std::move(settled)}}; });
        };
    }
    void Progress(const contracts::ComputeProgress& progress) noexcept {
        contracts::ComputeUiState snapshot;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.active || !contracts::compute_progress_follows(progress, state_.progress.sequence)) return;
            state_.progress = progress;
            if (state_.terminal.outcome == contracts::ComputeOperationOutcome::Running) state_.terminal.detail.clear();
            snapshot = state_;
        }
        direct::PublishLazyNoexcept(events_, [&] { return ComputeSystemEvent{ComputeProgressEvent{std::move(snapshot)}}; });
    }
    SettingsSystem& settings_;
    DatasetSystem& dataset_;
    ModelSystem& model_;
    RuntimeFactory factory_;
    SystemEventSink<ComputeSystemEvent> events_;
    ComputeMaterializer<Request> materialize_;
    mutable std::mutex mutex_;
    contracts::ComputeUiState state_{};
    std::unique_ptr<Runtime> runtime_;
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution_;
    direct::LocalRun run_;
};

}  // namespace

class CudaValidationRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ValidationSession> {
   public:
    using CudaSessionRuntimeState::CudaSessionRuntimeState;
};
CudaValidationRuntime::CudaValidationRuntime(DirectComputeConfiguration c) : impl_(std::make_unique<Impl>(c)) {}
CudaValidationRuntime::~CudaValidationRuntime() = default;
contracts::ComputeTerminal CudaValidationRuntime::Run(mmltk::backend::models::rfdetr::ValidateRequest operation, std::stop_token stop,
                                                      const ComputeProgressSink& progress) {
    return impl_->resources.Run(
        [this, &progress, stop, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            auto& request = operation;
            request.device_id = impl_->resources.device();
            request.compile_cuda_device_id = impl_->resources.device();
            request = mmltk::backend::models::rfdetr::finalize_validate_request(std::move(request));
            std::uint64_t sequence = 0U;
            const auto images = impl_->session.RunImageCount(request, stream, {.stop = stop, .progress = [&](std::size_t completed, std::size_t total) {
                if (progress) progress({++sequence, completed, total, "Validating"});
            }});
            return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0, images);
        },
        // CLEANUP-IGNORE: Validation closes its own CUDA session run; export and prediction have distinct result
        // products.
        stop);
}

class CudaExportRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ExportOnnxSession> {
   public:
    using CudaSessionRuntimeState::CudaSessionRuntimeState;
};

CudaExportRuntime::CudaExportRuntime(DirectComputeConfiguration configuration) : impl_(std::make_unique<Impl>(configuration)) {}
CudaExportRuntime::~CudaExportRuntime() = default;
contracts::ComputeTerminal CudaExportRuntime::Run(mmltk::backend::models::rfdetr::ModelExportRequest operation, std::stop_token stop,
                                                  const ComputeProgressSink&) {
    using mmltk::backend::models::rfdetr::BuildEngineRequest;
    using mmltk::backend::models::rfdetr::ExportOnnxRequest;
    return impl_->resources.Run(
        [this, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            if (auto* request = std::get_if<BuildEngineRequest>(&operation)) {
                request->device_id = impl_->resources.device();
                mmltk::backend::models::rfdetr::build_tensorrt_engine(*request, stream);
                return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0, 0, request->output_path.string());
            }
            auto& request = std::get<ExportOnnxRequest>(operation);
            request.device_id = impl_->resources.device();
            impl_->session.Run(request, stream);
            return contracts::make_compute_terminal(
                contracts::ComputeOperationOutcome::Succeeded, 0, 0,
                // CLEANUP-IGNORE: ONNX export publishes its domain output path from the validated request.
                request.output_path.string());
        },
        // CLEANUP-IGNORE: Export closes its own CUDA session run independently of prediction result ownership.
        stop);
}

class ValidationSystem::Impl final {
   public:
    Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ValidationRuntimeFactory factory,
         SystemEventSink<ComputeSystemEvent> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution)
        : core(settings, dataset, model, std::move(factory), std::move(events), subsystems::system::ComputeIntentMaterializer::Validation,
               std::move(execution)) {}
    ComputeSystemCore<mmltk::backend::models::rfdetr::ValidateRequest, ValidationRuntime> core;
};
ValidationSystem::ValidationSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ValidationRuntimeFactory factory,
                                   SystemEventSink<event_type> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(factory), std::move(events), std::move(execution))) {}
ValidationSystem::~ValidationSystem() = default;
contracts::ComputeUiState ValidationSystem::Start(contracts::ValidateWorkflowIntent) { return impl_->core.Start(); }
contracts::ComputeUiState ValidationSystem::Stop() noexcept { return impl_->core.Stop(); }
void ValidationSystem::Shutdown() noexcept { impl_->core.Shutdown(); }
contracts::ComputeUiState ValidationSystem::snapshot() const { return impl_->core.snapshot(); }

class ExportSystem::Impl final {
   public:
    Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ExportRuntimeFactory factory,
         SystemEventSink<ComputeSystemEvent> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution)
        : core(settings, dataset, model, std::move(factory), std::move(events), subsystems::system::ComputeIntentMaterializer::Export,
               std::move(execution)) {}
    ComputeSystemCore<mmltk::backend::models::rfdetr::ModelExportRequest, ExportRuntime> core;
};
ExportSystem::ExportSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ExportRuntimeFactory factory,
                           SystemEventSink<event_type> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(factory), std::move(events), std::move(execution))) {}
ExportSystem::~ExportSystem() = default;
contracts::ComputeUiState ExportSystem::Start(contracts::ExportWorkflowIntent) { return impl_->core.Start(); }
contracts::ComputeUiState ExportSystem::Stop() noexcept { return impl_->core.Stop(); }
void ExportSystem::Shutdown() noexcept { impl_->core.Shutdown(); }
contracts::ComputeUiState ExportSystem::snapshot() const { return impl_->core.snapshot(); }


}  // namespace mmltk::controller
