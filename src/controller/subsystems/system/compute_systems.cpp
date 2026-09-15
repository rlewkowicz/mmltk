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
#include "detail/validation_samples.h"
#include "src/controller/presentation/workspace_input.h"
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
                      std::optional<mmltk::frameworks::gpu::DeviceExecution> execution,
                      mmltk::backend::models::rfdetr::ValidationDelivery validation_delivery = {},
                      std::function<void(ValidationRuntimeResult)> validation_result = {})
        : settings_(settings),
          dataset_(dataset),
          model_(model),
          factory_(std::move(factory)),
          events_(std::move(events)),
          materialize_(std::move(materialize)),
          execution_(std::move(execution)), validation_delivery_(std::move(validation_delivery)), validation_result_(std::move(validation_result)) {
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
                    const auto progress = [this, &malformed_progress](const contracts::ComputeProgress& p) {
                        if (!p.valid()) { malformed_progress.store(true, std::memory_order_relaxed); return; }
                        Progress(p);
                    };
                    if constexpr (std::same_as<Request, mmltk::backend::models::rfdetr::ValidateRequest>) {
                        auto result = runtime_->Run(std::move(*prepared), stop, progress, validation_delivery_);
                        terminal = result.terminal;
                        if (validation_result_) validation_result_(std::move(result));
                    } else terminal = runtime_->Run(std::move(*prepared), stop, progress);
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
    mmltk::backend::models::rfdetr::ValidationDelivery validation_delivery_;
    std::function<void(ValidationRuntimeResult)> validation_result_;
};

}  // namespace

class CudaValidationRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ValidationSession> {
   public:
    using CudaSessionRuntimeState::CudaSessionRuntimeState;
};
CudaValidationRuntime::CudaValidationRuntime(DirectComputeConfiguration c) : impl_(std::make_unique<Impl>(c)) {}
CudaValidationRuntime::~CudaValidationRuntime() = default;
ValidationRuntimeResult CudaValidationRuntime::Run(mmltk::backend::models::rfdetr::ValidateRequest operation, std::stop_token stop,
                                                      const ComputeProgressSink& progress, const mmltk::backend::models::rfdetr::ValidationDelivery& delivery) {
    ValidationRuntimeResult result;
    result.terminal = impl_->resources.Run(
        [this, &result, &progress, &delivery, stop, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            operation.device_id = impl_->resources.device();
            operation.compile_cuda_device_id = impl_->resources.device();
            operation = mmltk::backend::models::rfdetr::finalize_validate_request(std::move(operation));
            std::uint64_t sequence = 0U;
            auto callbacks = delivery;
            callbacks.stop = stop;
            callbacks.progress = [&](std::size_t completed, std::size_t total) {
                if (progress) progress({++sequence, completed, total, "Validating"});
            };
            auto evaluated = impl_->session.Run(operation, stream, callbacks);
            if (evaluated.backends.size() > 1U) throw std::logic_error("GUI validation requires one selected backend");
            if (!evaluated.backends.empty()) result.evaluation = std::move(evaluated.backends.begin()->second);
            return contracts::make_compute_terminal(evaluated.cancelled ? contracts::ComputeOperationOutcome::Cancelled : contracts::ComputeOperationOutcome::Succeeded,
                                                     0U, evaluated.processed_images);
        }, stop);
    return result;
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
        [this, stop, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            if (auto* request = std::get_if<BuildEngineRequest>(&operation)) {
                request->device_id = impl_->resources.device();
                mmltk::backend::models::rfdetr::build_tensorrt_engine(*request, stream, {}, stop);
                return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0, 0, request->output_path.string());
            }
            auto& request = std::get<ExportOnnxRequest>(operation);
            request.device_id = impl_->resources.device();
            impl_->session.Run(request, stream, stop);
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
         SystemEventSink<ValidationSystem::event_type> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution,
         VisualDeviceSettings visual)
        : events_(std::move(events)), samples_(visual, [this] { Changed(); }),
          core_(settings, dataset, model, std::move(factory), [this](ComputeSystemEvent event) {
              if (const auto* progress = std::get_if<ComputeProgressEvent>(&event))
                  direct::PublishLazyNoexcept(events_, [&] { return ValidationSystem::event_type{ValidationProgress{progress->snapshot}}; });
              else Changed();
          }, subsystems::system::ComputeIntentMaterializer::Validation, std::move(execution),
          {.samples_selected = visual.valid() ? std::function<void(std::span<const std::uint32_t>, std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>)>{
              [this](auto indices, auto) { samples_.Begin(core_.snapshot().generation_frontier, indices); }} : decltype(mmltk::backend::models::rfdetr::ValidationDelivery::samples_selected){},
           .sample = visual.valid() ? std::function<void(mmltk::backend::models::rfdetr::ValidationSampleView)>{[this](auto sample) {
               try { samples_.Capture(std::move(sample)); }
               catch (const mmltk::backend::ml::runtime::CudaOperationError&) { throw; }
               catch (...) { /* An ordinary preview refusal cannot discard measured metrics. */ }
           }} : decltype(mmltk::backend::models::rfdetr::ValidationDelivery::sample){}},
          [this](ValidationRuntimeResult result) {
              std::scoped_lock lock(mutex_);
              evaluation_ = std::move(result.evaluation);
              evaluation_generation_ = core_.snapshot().generation_frontier;
          }) {}
    ~Impl() { Shutdown(); }
    ValidationSnapshot snapshot() const {
        auto result = samples_.snapshot();
        result.operation = core_.snapshot();
        std::scoped_lock lock(mutex_);
        if (evaluation_ && evaluation_generation_ == result.operation.generation_frontier) {
            result.metrics = evaluation_->summary;
            result.detail_rows = static_cast<std::uint32_t>(evaluation_->details.size());
        }
        return result;
    }
    void Changed() noexcept { direct::PublishLazyNoexcept(events_, [&] { return ValidationSystem::event_type{ValidationChanged{snapshot()}}; }); }
    void Shutdown() noexcept { core_.Shutdown(); samples_.Shutdown(); }
    SystemEventSink<ValidationSystem::event_type> events_;
    mutable std::mutex mutex_;
    std::optional<mmltk::backend::models::rfdetr::ValidationBackendResult> evaluation_;
    std::uint64_t evaluation_generation_ = 0U;
    WorkspaceInput input_;
    detail::ValidationSamples samples_;
    ComputeSystemCore<mmltk::backend::models::rfdetr::ValidateRequest, ValidationRuntime> core_;
};
ValidationSystem::ValidationSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, ValidationRuntimeFactory factory,
                                   SystemEventSink<event_type> events, std::optional<mmltk::frameworks::gpu::DeviceExecution> execution, VisualDeviceSettings visual)
    : impl_(std::make_unique<Impl>(settings, dataset, model, std::move(factory), std::move(events), std::move(execution), visual)) {}
ValidationSystem::~ValidationSystem() = default;
ValidationSnapshot ValidationSystem::Start(contracts::ValidateWorkflowIntent) { static_cast<void>(impl_->core_.Start()); return snapshot(); }
ValidationSnapshot ValidationSystem::Stop() noexcept { static_cast<void>(impl_->core_.Stop()); return snapshot(); }
ValidationSnapshot ValidationSystem::SelectSample(ValidationSampleIdentity identity) { impl_->samples_.Select(identity); return snapshot(); }
ValidationSnapshot ValidationSystem::CloseDetail() { impl_->samples_.CloseDetail(); return snapshot(); }
ValidationSnapshot ValidationSystem::SetOverlays(ValidationOverlays overlays) { impl_->samples_.SetOverlays(overlays); return snapshot(); }
mmltk::backend::models::rfdetr::EvaluationDetailPage ValidationSystem::Details(mmltk::backend::models::rfdetr::EvaluationDetailQuery query) const {
    namespace rfdetr = mmltk::backend::models::rfdetr;
    if (query.count == 0U || query.count > rfdetr::kEvaluationDetailPageSize) throw contracts::InvalidIntentError("validation detail page is too large");
    std::scoped_lock lock(impl_->mutex_);
    if (!impl_->evaluation_ || query.generation != impl_->evaluation_generation_ || query.generation != impl_->core_.snapshot().generation_frontier) throw contracts::InvalidIntentError("validation metric query is stale");
    const auto& rows = impl_->evaluation_->details;
    if (query.offset > rows.size()) throw contracts::InvalidIntentError("validation metric offset is invalid");
    rfdetr::EvaluationDetailPage result{query.generation, static_cast<std::uint32_t>(rows.size()), query.offset, {}};
    const auto count = std::min<std::size_t>(query.count, rows.size() - query.offset);
    result.rows.assign(rows.begin() + query.offset, rows.begin() + query.offset + count);
    return result;
}
void ValidationSystem::Shutdown() noexcept { impl_->Shutdown(); }
ValidationSnapshot ValidationSystem::snapshot() const { return impl_->snapshot(); }
std::optional<ValidationImageMetadata> ValidationSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->samples_.ImageSnapshot(frame); }
VisualSourceObservation ValidationSystem::ObserveSource() const { const auto image = impl_->samples_.snapshot(); return {image.frame, image.frame.revision}; }
mmltk::frameworks::gpu::BorrowedImageProductReadView ValidationSystem::BorrowFrame() const { return impl_->samples_.BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace ValidationSystem::BorrowWorkspace() const { return impl_->samples_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation ValidationSystem::ObserveWorkspace() const { return impl_->samples_.ObserveWorkspace(); }
void ValidationSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->samples_.RequestWorkspace(std::move(request)); }
void ValidationSystem::Input(WorkspaceMouse mouse) { std::scoped_lock lock(impl_->mutex_); impl_->input_.Accept(std::move(mouse), PresentationSourceKind::Validation); }
void ValidationSystem::SetInputPeer(std::uint64_t epoch) { std::scoped_lock lock(impl_->mutex_); impl_->input_.SetPeer(epoch); }

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
