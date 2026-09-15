#include "src/frameworks/gpu/image_failure.h"
#include "detail/prediction_preview.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/backend/imaging/raster/chw_image.h"
#include "src/controller/subsystems/system/compute_systems.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/subsystems/system/detail/predict_revision.h"
#include "src/controller/subsystems/system/detail/prediction_playback.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "src/backend/data/compiled_format.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/frameworks/gpu/cuda_device_scope.h"

import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.model_export;
import mmltk.backend.imaging.raster;

namespace mmltk::controller {
namespace {

using MaterializationRefusal = subsystems::system::ComputeIntentMaterializer::Refusal;

template <class Request>
using ComputeMaterializer = std::move_only_function<std::expected<Request, MaterializationRefusal>(
    const contracts::GuiSettingsState&, const contracts::ArtifactInspection&, const contracts::ModelSelection&)>;

class CudaRuntimeResources final {
   public:
    using Close = std::move_only_function<void()>;
    using Work = std::move_only_function<contracts::ComputeTerminal(mmltk::backend::ml::runtime::BorrowedCommandStream)>;

    CudaRuntimeResources(const DirectComputeConfiguration config, Close close)
        : execution_(config.execution), device_(config.execution ? config.execution->device : -1), close_(std::move(close)) {
        if (!config.valid()) throw contracts::UnavailableError("compute CUDA device is unavailable");
        WithExecution([this] {
            const auto status = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
            if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
        });
    }
    ~CudaRuntimeResources() noexcept {
        try {
            WithDevice([this] {
                if (close_) {
                    try {
                        close_();
                    } catch (...) {}
                }
                if (stream_ != nullptr) {
                    static_cast<void>(cudaStreamSynchronize(stream_));
                    static_cast<void>(cudaStreamDestroy(stream_));
                    stream_ = nullptr;
                }
            });
        } catch (...) {}
    }
    [[nodiscard]] contracts::ComputeTerminal Run(Work work, const std::stop_token stop, bool settle = true) {
        if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
        contracts::ComputeTerminal result;
        WithExecution([&] {
            result = work({.native_handle = reinterpret_cast<std::uintptr_t>(stream_), .valid = true});
            if (settle) {
                const auto status = cudaStreamSynchronize(stream_);
                if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
            }
        });
        return stop.stop_requested() ? contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, 0U, result.completed) : result;
    }
    [[nodiscard]] int device() const noexcept { return device_; }

   private:
    using Command = std::move_only_function<void()>;

    void WithExecution(Command work) {
        mmltk::common::system::ScopedExecutionPolicy policy(
            {execution_->placement.cpus, {}, 0, execution_->placement.numa_node, -10, false});
        WithDevice(std::move(work));
    }
    void WithDevice(Command work) {
        frameworks::gpu::CudaDeviceScope scope{device_};
        if (!scope) throw std::runtime_error(cudaGetErrorString(scope.status()));
        try {
            work();
        } catch (...) {
            static_cast<void>(scope.Finalize());
            throw;
        }
        const auto status = scope.Finalize();
        if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    }
    std::optional<mmltk::frameworks::gpu::DeviceExecution> execution_;
    int device_ = -1;
    cudaStream_t stream_ = nullptr;
    Close close_;
};

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

template <class Session>
class CudaSessionRuntimeState {
   public:
    explicit CudaSessionRuntimeState(const DirectComputeConfiguration config)
        : resources(config, [this] { static_cast<void>(session.Close()); }) {}

    Session session;
    CudaRuntimeResources resources;
};

}  // namespace

class CudaValidationRuntime::Impl final : public CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ValidationSession> {
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

class CudaExportRuntime::Impl final : public CudaSessionRuntimeState<mmltk::backend::models::rfdetr::ExportOnnxSession> {
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

class CudaPredictRuntime::Impl final : public CudaSessionRuntimeState<mmltk::backend::models::rfdetr::PredictionSession> {
   public:
    explicit Impl(DirectComputeConfiguration configuration) : CudaSessionRuntimeState(configuration), config(std::move(configuration)) {}
    DirectComputeConfiguration config;
    std::unique_ptr<detail::PredictionPreviewPool> preview;
    std::optional<mmltk::frameworks::gpu::DeviceContext> preview_context;
};

CudaPredictRuntime::CudaPredictRuntime(DirectComputeConfiguration configuration) : impl_(std::make_unique<Impl>(configuration)) {}
CudaPredictRuntime::~CudaPredictRuntime() = default;
contracts::ComputeTerminal CudaPredictRuntime::Run(mmltk::backend::models::rfdetr::PredictRequest operation, const std::stop_token stop,
                                                    const ComputeProgressSink& progress, const ProductSink& products, const PlaybackGate& gate, VisualExtent maximum, const ContextProvider& current_context) {
    return impl_->resources.Run(
        [this, &progress, &products, &gate, stop, operation, maximum, &current_context](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            operation.device_id = impl_->resources.device();
            std::shared_ptr<const std::vector<std::string>> classes;
            int class_count = 0;
            std::uint64_t sequence = 0U;
            const auto result = impl_->session.RunAndWrite(operation, stream, {
                .stop = stop,
                .source_pixels = static_cast<bool>(products),
                .maximum_pixel_width = maximum.width,
                .maximum_pixel_height = maximum.height,
                .before_frame = gate,
                .begin = [&](const auto& summary) {
                    if (!products) return;
                    try {
                        classes = std::make_shared<const std::vector<std::string>>(summary.class_names);
                        class_count = summary.artifacts.config.num_classes;
                    } catch (const std::exception& error) {
                        if (products) products(std::unexpected{std::string{error.what()}});
                    }
                },
                .completed = [&](const auto& record, const auto pixels, const auto& annotations) {
                    if (!products || !classes) return;
                    try {
                        const auto context = current_context ? current_context() : std::nullopt;
                        if (!context) {
                            products(std::unexpected{std::string{"Prediction preview context is recovering"}});
                            return;
                        }
                        if (!pixels.preview_failure.empty()) throw std::runtime_error(std::string{pixels.preview_failure});
                        if (pixels.width > maximum.width || pixels.height > maximum.height)
                            throw std::runtime_error("Prediction preview exceeds the visual dimensions");
                        if (!impl_->preview || impl_->preview_context != context) {
                            auto candidate = std::make_unique<detail::PredictionPreviewPool>(*impl_->config.execution, *context);
                            impl_->preview = std::move(candidate);
                            impl_->preview_context = context;
                        }
                        auto raw = impl_->preview->Capture(pixels.chw, {pixels.width, pixels.height}, pixels.stream,
                            record.detections, annotations, classes, class_count, pixels.rgb8, pixels.custody, pixels.stop_source, pixels.source_control);
                        if (raw) products(Product{.extent = {pixels.width, pixels.height}, .raw = std::move(raw), .image_id = record.image_id});
                    } catch (const mmltk::backend::ml::runtime::CudaOperationError&) {
                        throw;
                    } catch (const std::exception& error) {
                        products(std::unexpected{std::string{error.what()}});
                    } catch (...) {
                        products(std::unexpected{std::string{"Prediction preview custody transfer failed"}});
                    }
                },
                .progress = [&](std::size_t completed, std::size_t total) {
                    if (progress) progress({++sequence, completed, total, "Processed"});
                },
                .decoded = [&](std::size_t decoded, std::size_t total) {
                    if (progress) progress({++sequence, decoded, total, "Decoded"});
                },
            });
            return contracts::make_compute_terminal(result.cancelled ? contracts::ComputeOperationOutcome::Cancelled : contracts::ComputeOperationOutcome::Succeeded,
                0U, result.processed_images, result.cancelled ? std::string{} : operation.output_path.string());
        }, {}, false); // PredictionSession owns the atomic output completion boundary.
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

class PredictSystem::Impl final {
    friend class PredictSystem;

    WorkspaceInput input_;

   public:
    Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, const VisualDeviceSettings visual,
         PredictRuntimeFactory factory, SystemEventSink<event_type> events)
        : settings_(settings),
          dataset_(dataset),
          model_(model),
          visual_(visual),
          events_(std::move(events)),
          factory_(std::move(factory)),
          worker_(
              [this, visual, topology = mmltk::common::system::NumaTopology::Capture(),
               execution = std::optional<mmltk::frameworks::gpu::DeviceExecution>{}](auto revisions) mutable {
                  if (!execution) execution = mmltk::frameworks::gpu::resolve_device_execution(visual.device, topology, visual.numa_node);
                  mmltk::frameworks::gpu::SystemImageRuntimeConfig config{
                      .device = visual.device,
                      .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                      .output_buffer_count = 2U,
                      .numa_node = visual.numa_node,
                      .execution = *execution,
                      .product_revisions = std::move(revisions),
                      .adopted_context = PreviewContext(*execution),
                  };
                  configure_visual_workspace_finalization(config);
                  return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(std::move(config));
              },
              [this](const std::exception_ptr failure) { RenderingFailed(failure); }) {
        if (!visual_.valid()) throw contracts::UnavailableError("prediction visual device is unavailable");
        worker_.RegisterContinuation([this](auto& runtime, auto stop) { return Render(runtime, stop); }, {}, true);
    }

    ~Impl() { Shutdown(); }

    [[nodiscard]] PredictSnapshot Start() {
        {
            std::scoped_lock lock(mutex_);
            if (state_.operation.active) throw contracts::BusyError("prediction is busy");
        }
        // CLEANUP-IGNORE: Predict begins from canonical settings but then owns a distinct visual operation path.
        const auto settings = settings_.materialization_facts();
        if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
        // CLEANUP-IGNORE: Predict materialization additionally owns private visual admission and publication.
        const auto selection = model_.selection();
        PredictSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            if (state_.operation.active) throw contracts::BusyError("prediction is busy");
            const auto prior = state_;
            const auto& source = settings.settings.workflows.predict.source;
            const auto key = std::to_string(static_cast<unsigned>(source.kind)) + ":" +
                (source.kind == contracts::SourceKind::CompiledDataset ? source.compiled_path :
                 source.kind == contracts::SourceKind::SingleImage ? source.single_image_path : source.video_file_path);
            if (source_key_ != key) {
                if (source_instance_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("prediction source identity exhausted");
                ++source_instance_;
                source_key_ = key;
            }
            const auto generation = contracts::next_compute_generation(state_.operation.generation_frontier);
            if (!generation) throw contracts::FailedError("prediction operation generation exhausted");
            state_.revision = detail::PredictRevision::Admit(state_.revision);
            state_.operation.generation_frontier = *generation;
            state_.operation.active = true;
            state_.operation.progress = {};
            state_.paused = false;
            preview_failure_.clear();
            state_.video = source.kind == contracts::SourceKind::VideoFile;
            playback_.Reset();
            state_.operation.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Running, *generation,
                                                                         0U, {}, "Preparing selected prediction inputs");
            try {
                run_.Start({
                    .work = [this, settings = settings.settings, selection, source_instance = source_instance_, generation = *generation](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                        contracts::ArtifactInspection inspection;
                        if (!stop.stop_requested() && settings.workflows.predict.source.kind == contracts::SourceKind::CompiledDataset)
                            inspection = dataset_.Inspect({settings.workflows.predict.source.compiled_path, {}, {}}, selection.key.preset, selection.key.resolution, stop);
                        if (stop.stop_requested()) return [this, generation] { Settled(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, generation)); };
                        auto request = subsystems::system::ComputeIntentMaterializer::Predict(settings, inspection, selection);
                        if (!request) throw contracts::InvalidIntentError(request.error().detail);
                        const bool initial_runtime = !runtime_;
                        if (!runtime_) runtime_ = factory_();
                        if (!runtime_) throw std::runtime_error("prediction runtime factory returned no runtime");
                        const auto execution = mmltk::frameworks::gpu::resolve_device_execution(visual_.device, mmltk::common::system::NumaTopology::Capture(), visual_.numa_node);
                        try { if (initial_runtime) static_cast<void>(PreviewContext(execution)); }
                        catch (const std::exception& error) { Product(std::unexpected{std::string{error.what()}}, source_instance); }
                        auto terminal = runtime_->Run(std::move(*request), stop,
                            [this](const auto& progress) { Progress(progress); },
                            [this, source_instance](std::expected<PredictRuntime::Product, std::string> product) {
                                Product(std::move(product), source_instance);
                            },
                            [this, stop](std::optional<double> timestamp, double fps) { return playback_.Wait(timestamp, fps, stop); }, {visual_.maximum_width, visual_.maximum_height}, [this] {
                                std::unique_lock lock(preview_context_mutex_, std::try_to_lock);
                                return lock.owns_lock() ? preview_context_ : std::nullopt;
                            });
                        if (!terminal.valid_worker_terminal()) throw std::runtime_error("prediction runtime returned an invalid terminal");
                        terminal.generation = generation;
                        return [this, terminal = std::move(terminal)]() mutable { Settled(std::move(terminal)); };
                    },
                    .failure = [this](std::exception_ptr failure) -> direct::LocalRun::Notification {
                        runtime_.reset();
                        return [this, failure] { Failed(failure); };
                    },
                });
            } catch (...) {
                state_ = prior;
                throw;
            }
            admitted = state_;
        }
        return admitted;
    }

    [[nodiscard]] PredictSnapshot Stop() noexcept {
        bool active = false;
        {
            std::scoped_lock lock(mutex_);
            active = state_.operation.active;
            if (active && state_.operation.terminal.outcome != contracts::ComputeOperationOutcome::CancellationRequested) {
                const auto revision = detail::PredictRevision::Cancel(state_.revision);
                // Admission and progress preserve this identity while Running.
                if (!revision) return state_;
                state_.revision = *revision;
                state_.operation.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::CancellationRequested,
                                                                             state_.operation.generation_frontier);
            }
        }
        if (active) static_cast<void>(run_.Stop());
        return snapshot();
    }
    void Shutdown() noexcept { run_.StopAndJoin(); worker_.StopAndWait(); pending_product_.reset(); runtime_.reset(); worker_.FinishStoppedRetirement(); }
    [[nodiscard]] std::optional<PredictImageMetadata> ImageSnapshot(const VisualFrame& frame) const {
        std::scoped_lock lock(mutex_);
        if (state_.frame != frame) return std::nullopt;
        return PredictSystem::visual_source::ImageOf(state_);
    }
    [[nodiscard]] PredictSnapshot snapshot() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
        VisualFrame committed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.frame.valid())
                return {};
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, worker_);
    }

   private:
    mmltk::frameworks::gpu::DeviceContext PreviewContext(const mmltk::frameworks::gpu::DeviceExecution& execution) {
        std::scoped_lock lock(preview_context_mutex_);
        if (!preview_context_) {
            CUcontext previous{};
            if (cuInit(0U) != CUDA_SUCCESS || cuCtxGetCurrent(&previous) != CUDA_SUCCESS) throw std::runtime_error("prediction context query failed");
            try {
                preview_context_.emplace(execution.device, mmltk::frameworks::gpu::cuda_image_copy_backend(),
                    mmltk::frameworks::gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
            } catch (...) { static_cast<void>(cuCtxSetCurrent(previous)); throw; }
            if (cuCtxSetCurrent(previous) != CUDA_SUCCESS) throw std::runtime_error("prediction context restore failed");
        }
        return *preview_context_;
    }
    std::mutex preview_context_mutex_;
    std::optional<mmltk::frameworks::gpu::DeviceContext> preview_context_;
    struct PendingProduct final {
        std::expected<PredictRuntime::Product, std::string> product;
        std::uint64_t source_instance = 0U;
    };
    void Product(std::expected<PredictRuntime::Product, std::string> product, std::uint64_t source_instance) {
        {
            std::scoped_lock lock(mutex_);
            preview_failure_.clear();
            pending_product_ = PendingProduct{std::move(product), source_instance};
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    detail::VisualRuntimeOwner::Notification Render(mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token stop) {
        if (stop.stop_requested()) return {};
        {
            std::scoped_lock lock(mutex_);
            if (!pending_product_) return {};
            if (pending_product_->product && pending_product_->product->raw && !pending_product_->product->raw->CompatibleWith(runtime)) {
                pending_product_.reset();
                return [this] { PreviewFailed("Prediction preview belongs to a retired visual context"); };
            }
            if (!pending_product_->product) {
                auto error = std::move(pending_product_->product.error());
                pending_product_.reset();
                return [this, error = std::move(error)]() mutable { PreviewFailed(std::move(error)); };
            }
        }
        auto baseline = runtime.Completed();
        auto candidate = worker_.TryAcquireOutput(runtime, baseline);
        if (!candidate.valid()) return {};
        PendingProduct pending;
        {
            std::scoped_lock lock(mutex_);
            pending = std::move(*pending_product_);
            pending_product_.reset();
        }
        if (!pending.product) throw std::runtime_error(pending.product.error());
        auto& product = *pending.product;
        if (!product.extent.valid() || !product.raw || product.extent.width > visual_.maximum_width || product.extent.height > visual_.maximum_height)
            throw std::runtime_error("Prediction preview exceeds the visual product limits");
        std::vector<PredictLabel> labels;
        if (preview_class_count_ != product.raw->class_count()) {
            preview_palette_ = contracts::annotation_class_palette(static_cast<std::size_t>(product.raw->class_count()));
            preview_class_count_ = product.raw->class_count();
        }
        const auto& palette = preview_palette_;
        const auto& classes = product.raw->classes();
        labels.reserve(product.raw->predictions().size());
        for (const auto& detection : product.raw->predictions()) {
            const auto category = detection.category_id - 1;
            labels.push_back({{{detection.bbox_xyxy[0], detection.bbox_xyxy[1]}, {detection.bbox_xyxy[2], detection.bbox_xyxy[3]}}, category, detection.score,
                category >= 0 && static_cast<std::size_t>(category) < palette.size() ? palette[category] : contracts::AnnotationColor{},
                category >= 0 && static_cast<std::size_t>(category) < classes.size() ? classes[category] : std::to_string(detection.category_id)});
            const auto& label = labels.back();
            if (!label.box.valid() || !label.color.valid() || !std::isfinite(label.confidence) || label.name.size() > mmltk::frameworks::reflection::kMaximumNameBytes)
                throw std::runtime_error("Prediction preview metadata exceeds the visual product limits");
        }
        product.raw->Draw(runtime, candidate);
        const auto completed = runtime.CommitOutput(std::move(candidate));
        const auto frame = visual_frame({PresentationSourceKind::Predict, pending.source_instance}, product.extent, completed.revision());
        return [this, frame, labels = std::move(labels), identity = product.image_id]() mutable {
            PredictSnapshot changed;
            {
                std::scoped_lock lock(mutex_);
                const auto revision = detail::PredictRevision::Frame(state_.revision, state_.operation.active, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
                if (!revision) return;
                state_.frame = frame;
                state_.labels = std::move(labels);
                state_.image_id = identity;
                state_.revision = *revision;
                changed = state_;
            }
            Publish(PredictChanged{std::move(changed)});
        };
    }
    void Progress(const contracts::ComputeProgress& progress) noexcept {
        PredictSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.operation.active || !contracts::compute_progress_follows(progress, state_.operation.progress.sequence)) return;
            const auto revision = detail::PredictRevision::Progress(
                state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
            if (!revision) return;
            state_.revision = *revision;
            state_.operation.progress = progress;
            if (state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::Running) state_.operation.terminal.detail.clear();
            changed = state_;
        }
        Publish(PredictProgress{std::move(changed)});
    }
    void PreviewFailed(std::string detail) noexcept {
        PredictSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            if (preview_failure_ == detail) return;
            const auto revision = detail::PredictRevision::Progress(state_.revision,
                state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
            if (!revision) return;
            preview_failure_ = detail;
            state_.revision = *revision;
            changed = state_;
        }
        Publish(PredictFailed{std::move(changed), std::move(detail)});
    }
    void RenderingFailed(const std::exception_ptr failure) noexcept {
        if (mmltk::frameworks::gpu::is_image_execution_failure(failure)) {
            std::scoped_lock lock(preview_context_mutex_);
            preview_context_.reset();
        }
        PreviewFailed(visual_failure_detail(failure, "prediction rendering failed"));
    }
    void Settled(contracts::ComputeTerminal terminal) noexcept {
        PredictSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            const auto revision = detail::PredictRevision::Complete(state_.revision);
            if (!revision) return;
            state_.revision = *revision;
            state_.operation.active = false;
            state_.paused = false;
            state_.operation.terminal = std::move(terminal);
            changed = state_;
        }
        Publish(PredictChanged{std::move(changed)});
    }
    void Failed(const std::exception_ptr failure) noexcept {
        auto detail = visual_failure_detail(failure, "prediction worker failed");
        PredictSnapshot failed;
        {
            std::scoped_lock lock(mutex_);
            const auto revision = detail::PredictRevision::Fail(state_.revision);
            if (!revision) return;
            state_.revision = *revision;
            state_.operation.active = false;
            state_.operation.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Failed,
                                                                         state_.operation.generation_frontier, 0U, {}, detail);
            failed = state_;
        }
        Publish(PredictFailed{std::move(failed), std::move(detail)});
    }
    template <class Event>
    void Publish(Event event) noexcept {
        publish_visual_event_noexcept(events_, event_type{std::move(event)});
    }

    SettingsSystem& settings_;
    DatasetSystem& dataset_;
    ModelSystem& model_;
    VisualDeviceSettings visual_;
    SystemEventSink<event_type> events_;
    mutable std::mutex mutex_;
    PredictSnapshot state_;
    std::optional<PendingProduct> pending_product_;
    int preview_class_count_ = 0;
    std::vector<contracts::AnnotationColor> preview_palette_;
    std::string source_key_;
    std::string preview_failure_;
    std::uint64_t source_instance_ = 0U;
    detail::PredictionPlayback playback_;
    PredictRuntimeFactory factory_;
    std::unique_ptr<PredictRuntime> runtime_;
    direct::LocalRun run_;
    detail::VisualRuntimeOwner worker_;
};

PredictSystem::PredictSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, const VisualDeviceSettings visual,
                             PredictRuntimeFactory factory, SystemEventSink<event_type> events)
    : impl_(std::make_unique<Impl>(settings, dataset, model, visual, std::move(factory), std::move(events))) {}
PredictSystem::~PredictSystem() = default;
PredictSnapshot PredictSystem::Start(contracts::PredictWorkflowIntent) { return impl_->Start(); }
PredictSnapshot PredictSystem::Pause(PredictPauseIntent intent) {
    {
        std::scoped_lock lock(impl_->mutex_);
        if (!impl_->state_.operation.active || !impl_->state_.video) return impl_->state_;
        const auto revision = detail::PredictRevision::Progress(impl_->state_.revision, false);
        if (!revision) return impl_->state_;
        impl_->state_.revision = *revision;
        impl_->state_.paused = intent.paused;
    }
    impl_->playback_.Pause(intent.paused);
    const auto changed = impl_->snapshot();
    impl_->Publish(PredictChanged{changed});
    return changed;
}
PredictSnapshot PredictSystem::Stop(contracts::PredictWorkflowIntent) noexcept { return impl_->Stop(); }
// CLEANUP-IGNORE: Predict exposes ordinary sealed lifecycle/read methods; VisualRuntimeOwner already owns shared workspace behavior.
void PredictSystem::Shutdown() noexcept { impl_->Shutdown(); }
PredictSnapshot PredictSystem::snapshot() const { return impl_->snapshot(); }
// CLEANUP-IGNORE: Predict forwards its sealed source API to its own owner and the existing shared renderer.
std::optional<PredictImageMetadata> PredictSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->ImageSnapshot(frame); }
mmltk::frameworks::gpu::BorrowedImageProductReadView PredictSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
mmltk::frameworks::gpu::BorrowedImageWorkspace PredictSystem::BorrowWorkspace() const { return impl_->worker_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation PredictSystem::ObserveWorkspace() const { return impl_->worker_.ObserveWorkspace(); }
void PredictSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->worker_.RequestWorkspace(std::move(request)); }

}  // namespace mmltk::controller

namespace mmltk::controller {
void PredictSystem::Input(WorkspaceMouse mouse) {
    std::scoped_lock lock(impl_->mutex_);
    if (impl_->worker_.stopped()) throw contracts::UnavailableError("Predict input is unavailable");
    impl_->input_.Accept(std::move(mouse), PresentationSourceKind::Predict);
}
void PredictSystem::SetInputPeer(std::uint64_t epoch) {
    std::scoped_lock lock(impl_->mutex_);
    impl_->input_.SetPeer(epoch);
}
}  // namespace mmltk::controller
