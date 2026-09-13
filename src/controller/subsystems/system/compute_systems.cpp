#include "src/controller/presentation/workspace_input.h"
#include "src/backend/data/compiled_dataset.h"
#include "src/controller/subsystems/system/compute_systems.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/subsystems/system/detail/predict_revision.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
    [[nodiscard]] contracts::ComputeTerminal Run(Work work, const std::stop_token stop) {
        if (stop.stop_requested()) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
        contracts::ComputeTerminal result;
        WithExecution([&] {
            result = work({.native_handle = reinterpret_cast<std::uintptr_t>(stream_), .valid = true});
            const auto status = cudaStreamSynchronize(stream_);
            if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
        });
        return stop.stop_requested() ? contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled) : result;
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
        auto operation = materialize_(settings.settings, dataset_.snapshot().inspection, model_.selection());
        if (!operation) throw contracts::InvalidIntentError(operation.error().detail);
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
                },
            .work = [this, operation = std::move(*operation)](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                contracts::ComputeTerminal terminal;
                bool failed = false;
                std::atomic_bool malformed_progress = false;
                try {
                    if (!runtime_) runtime_ = factory_();
                    if (!runtime_) throw std::runtime_error("compute runtime is unavailable");
                    terminal = runtime_->Run(std::move(operation), stop, [this, &malformed_progress](const contracts::ComputeProgress& p) {
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
                                                      const ComputeProgressSink&) {
    return impl_->resources.Run(
        [this, operation = std::move(operation)](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            auto& request = operation;
            request.device_id = impl_->resources.device();
            request.compile_cuda_device_id = impl_->resources.device();
            request = mmltk::backend::models::rfdetr::finalize_validate_request(std::move(request));
            const auto images = impl_->session.RunImageCount(request, stream);
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
    using CudaSessionRuntimeState::CudaSessionRuntimeState;
};

CudaPredictRuntime::CudaPredictRuntime(DirectComputeConfiguration configuration) : impl_(std::make_unique<Impl>(configuration)) {}
CudaPredictRuntime::~CudaPredictRuntime() = default;
CudaPredictRuntime::Product CudaPredictRuntime::Run(mmltk::backend::models::rfdetr::PredictRequest operation, const std::stop_token stop,
                                                    const ComputeProgressSink&) {
    Product product;
    mmltk::backend::models::rfdetr::PredictionRunResult predictions;
    product.terminal = impl_->resources.Run(
        [this, &predictions, operation](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            operation.device_id = impl_->resources.device();
            predictions = impl_->session.Run(operation, stream);
            mmltk::backend::models::rfdetr::write_prediction_json(operation, predictions);
            return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0, predictions.processed_images,
                                                    operation.output_path.string());
        },
        stop);
    if (product.terminal.outcome != contracts::ComputeOperationOutcome::Succeeded || predictions.records.empty()) return product;
    if (operation.source_kind != mmltk::backend::models::rfdetr::PredictSourceKind::CompiledDataset)
        throw contracts::InvalidIntentError("visual prediction requires a compiled dataset source");
    auto store = mmltk::backend::data::CompiledDataset::open_source(operation.compiled_path, 1'000'000U);
    if (!store) throw std::system_error(store.error(), "prediction visual source could not be opened");
    const auto index = predictions.records.front().dataset_index;
    if (index < 0 || static_cast<std::size_t>(index) >= store->image_entries().size())
        throw std::runtime_error("prediction visual source index is invalid");
    const auto width = store->header().image_width;
    const auto height = store->header().image_height;
    if (width == 0U || height == 0U || static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / height / 4U)
        throw std::runtime_error("prediction visual extent is invalid");
    product.extent = {width, height};
    product.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    const auto* pixels = store->image_pixels(static_cast<std::uint32_t>(index));
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    for (std::size_t pixel = 0U; pixel != plane; ++pixel) {
        for (std::size_t channel = 0U; channel != 3U; ++channel) {
            product.rgba[pixel * 4U + channel] =
                static_cast<std::uint8_t>(std::clamp(std::lround(pixels[channel * plane + pixel] * 255.0F), 0L, 255L));
        }
        product.rgba[pixel * 4U + 3U] = 255U;
    }
    product.boxes.reserve(predictions.records.front().detections.size());
    for (const auto& detection : predictions.records.front().detections)
        product.boxes.push_back({detection.bbox_xyxy[0U], detection.bbox_xyxy[1U], detection.bbox_xyxy[2U], detection.bbox_xyxy[3U]});
    return product;
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

namespace {

class PredictVisualModel final : public mmltk::frameworks::gpu::SystemImageModel {
   public:
    explicit PredictVisualModel(PredictRuntimeFactory factory) : runtime_(factory()) {
        if (!runtime_) throw std::runtime_error("prediction runtime factory returned no runtime");
    }
    [[nodiscard]] PredictRuntime::Product Run(mmltk::backend::models::rfdetr::PredictRequest request, const std::stop_token stop,
                                              const ComputeProgressSink& progress) {
        return runtime_->Run(std::move(request), stop, progress);
    }

   private:
    std::unique_ptr<PredictRuntime> runtime_;
};

[[nodiscard]] PredictVisualModel& predict_model(mmltk::frameworks::gpu::SystemImageRuntime& runtime) {
    auto* const model = dynamic_cast<PredictVisualModel*>(runtime.model());
    if (model == nullptr) throw std::runtime_error("prediction visual runtime is unavailable");
    return *model;
}

void publish_prediction_product(mmltk::frameworks::gpu::SystemImageRuntime& runtime, const PredictRuntime::Product& product) {
    runtime.Publish(
        product.extent.width, product.extent.height, [&product](const auto clean, const auto semantic, const auto stream_value) {
            auto stream = reinterpret_cast<cudaStream_t>(stream_value);
            const auto row_bytes = static_cast<std::size_t>(product.extent.width) * 4U;
            auto status = cudaMemcpy2DAsync(reinterpret_cast<void*>(clean.data), clean.descriptor.pitch_bytes, product.rgba.data(),
                                            row_bytes, row_bytes, product.extent.height, cudaMemcpyHostToDevice, stream);
            if (status == cudaSuccess)
                status = cudaMemset2DAsync(reinterpret_cast<void*>(semantic.data), semantic.descriptor.pitch_bytes, 0,
                                           semantic.descriptor.row_bytes(), semantic.descriptor.height, stream);
            if (status != cudaSuccess) throw std::runtime_error("prediction visual upload failed");
            for (const auto& box : product.boxes) {
                if (mmltk::backend::imaging::raster::raster_box_outline_rgba({
                        .overlay =
                            {
                                reinterpret_cast<std::uint8_t*>(semantic.data),
                                semantic.descriptor.pitch_bytes,
                                static_cast<int>(semantic.descriptor.width),
                                static_cast<int>(semantic.descriptor.height),
                            },
                        .box =
                            {
                                static_cast<int>(box.x1),
                                static_cast<int>(box.y1),
                                static_cast<int>(box.x2),
                                static_cast<int>(box.y2),
                            },
                        .color = {0U, 120U, 212U},
                        .thickness = 2,
                        .stream = {reinterpret_cast<void*>(stream_value)},
                    }) != 0)
                    throw std::runtime_error("prediction overlay rendering failed");
            }
            status = cudaStreamSynchronize(stream);
            if (status != cudaSuccess) throw std::runtime_error("prediction visual completion failed");
        });
}

}  // namespace

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
          worker_(
              [visual, topology = mmltk::common::system::NumaTopology::Capture(),
               execution = std::optional<mmltk::frameworks::gpu::DeviceExecution>{}, factory = std::move(factory)](auto revisions) mutable {
                  if (!execution) execution = mmltk::frameworks::gpu::resolve_device_execution(visual.device, topology, visual.numa_node);
                  mmltk::frameworks::gpu::SystemImageRuntimeConfig config{
                      .device = visual.device,
                      .model = std::make_unique<PredictVisualModel>(factory),
                      .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                      .output_buffer_count = 2U,
                      .numa_node = visual.numa_node,
                      .execution = *execution,
                      .product_revisions = std::move(revisions),
                  };
                  configure_visual_workspace_finalization(config);
                  return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(std::move(config));
              },
              [this](const std::exception_ptr failure) { Failed(failure); }) {
        if (!visual_.valid()) throw contracts::UnavailableError("prediction visual device is unavailable");
    }

    [[nodiscard]] PredictSnapshot Start() {
        {
            std::scoped_lock lock(mutex_);
            if (state_.operation.active) throw contracts::BusyError("prediction is busy");
        }
        // CLEANUP-IGNORE: Predict begins from canonical settings but then owns a distinct visual operation path.
        const auto settings = settings_.materialization_facts();
        if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
        // CLEANUP-IGNORE: Predict materialization additionally owns private visual admission and publication.
        auto operation =
            subsystems::system::ComputeIntentMaterializer::Predict(settings.settings, dataset_.snapshot().inspection, model_.selection());
        if (!operation) throw contracts::InvalidIntentError(operation.error().detail);
        PredictSnapshot admitted;
        {
            std::scoped_lock lock(mutex_);
            if (state_.operation.active) throw contracts::BusyError("prediction is busy");
            const auto prior = state_;
            const auto generation = contracts::next_compute_generation(state_.operation.generation_frontier);
            if (!generation) throw contracts::FailedError("prediction operation generation exhausted");
            state_.revision = detail::PredictRevision::Admit(state_.revision);
            state_.operation.generation_frontier = *generation;
            state_.operation.active = true;
            state_.operation.progress = {};
            state_.operation.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Running, *generation);
            state_.frame = {};
            if (!worker_.SubmitDiscrete(
                    [this, request = std::move(*operation), generation = *generation](
                        mmltk::frameworks::gpu::SystemImageRuntime& runtime,
                        const std::stop_token stop) mutable -> detail::VisualRuntimeOwner::Notification {
                        auto product = predict_model(runtime).Run(
                            std::move(request), stop, [this](const contracts::ComputeProgress& progress) { Progress(progress); });
                        if (!product.terminal.valid_worker_terminal())
                            throw std::runtime_error("prediction runtime returned an invalid terminal");
                        product.terminal.generation = generation;
                        if (product.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded) {
                            if (!product.extent.valid() || product.extent.width > visual_.maximum_width ||
                                product.extent.height > visual_.maximum_height ||
                                static_cast<std::size_t>(product.extent.width) >
                                    std::numeric_limits<std::size_t>::max() / product.extent.height / 4U)
                                throw std::runtime_error("prediction runtime returned an invalid visual product");
                            const auto expected = static_cast<std::size_t>(product.extent.width) * product.extent.height * 4U;
                            if (product.rgba.size() != expected)
                                throw std::runtime_error("prediction runtime returned an invalid visual product");
                            publish_prediction_product(runtime, product);
                        }
                        const auto frame =
                            product.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded
                                ? visual_frame({PresentationSourceKind::Predict, 1U}, product.extent, runtime.OutputFacts().revision)
                                : VisualFrame{};
                        return [this, terminal = std::move(product.terminal), frame]() mutable { Settled(std::move(terminal), frame); };
                    },
                    [this, generation = *generation] {
                        Settled(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, generation), {});
                    })) {
                state_ = prior;
                throw contracts::BusyError("prediction is busy");
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
        if (active) worker_.RequestActiveStop();
        return snapshot();
    }
    void Shutdown() noexcept { worker_.StopAndWait(); }
    [[nodiscard]] std::optional<VisualImageMetadata> ImageSnapshot(const VisualFrame& frame) const {
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
            if (state_.operation.active || state_.operation.terminal.outcome != contracts::ComputeOperationOutcome::Succeeded ||
                state_.operation.terminal.generation != state_.operation.generation_frontier || !state_.frame.valid())
                return {};
            committed = state_.frame;
        }
        return borrow_matching_visual_product(committed, worker_);
    }

   private:
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
            changed = state_;
        }
        Publish(PredictProgress{std::move(changed)});
    }
    void Settled(contracts::ComputeTerminal terminal, const VisualFrame frame) noexcept {
        PredictSnapshot changed;
        {
            std::scoped_lock lock(mutex_);
            const auto revision = detail::PredictRevision::Complete(state_.revision);
            if (!revision) return;
            state_.revision = *revision;
            state_.operation.active = false;
            state_.operation.terminal = std::move(terminal);
            state_.frame = frame;
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
            state_.frame = {};
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
    detail::VisualRuntimeOwner worker_;
};

PredictSystem::PredictSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, const VisualDeviceSettings visual,
                             PredictRuntimeFactory factory, SystemEventSink<event_type> events)
    : impl_(std::make_unique<Impl>(settings, dataset, model, visual, std::move(factory), std::move(events))) {}
PredictSystem::~PredictSystem() = default;
PredictSnapshot PredictSystem::Start(contracts::PredictWorkflowIntent) { return impl_->Start(); }
PredictSnapshot PredictSystem::Stop(contracts::PredictWorkflowIntent) noexcept { return impl_->Stop(); }
// CLEANUP-IGNORE: Predict exposes ordinary sealed lifecycle/read methods; VisualRuntimeOwner already owns shared workspace behavior.
void PredictSystem::Shutdown() noexcept { impl_->Shutdown(); }
PredictSnapshot PredictSystem::snapshot() const { return impl_->snapshot(); }
std::optional<VisualImageMetadata> PredictSystem::ImageSnapshot(const VisualFrame& frame) const { return impl_->ImageSnapshot(frame); }
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
}
