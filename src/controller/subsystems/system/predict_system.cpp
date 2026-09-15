#include "predict_system.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "detail/cuda_runtime_resources.h"
#include "detail/predict_revision.h"
#include "detail/prediction_playback.h"
#include "detail/prediction_preview.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/presentation/detail/visual_runtime_owner.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/system_image_runtime.h"

import mmltk.backend.models.rfdetr.inference.prediction;

namespace mmltk::controller {

class CudaPredictRuntime::Impl final : public detail::CudaSessionRuntimeState<mmltk::backend::models::rfdetr::PredictionSession> {
   public:
    explicit Impl(DirectComputeConfiguration configuration) : CudaSessionRuntimeState(configuration), config(std::move(configuration)) {}
    DirectComputeConfiguration config;
    bool close_failed = false;
    std::unique_ptr<detail::PredictionPreviewPool> preview;
    std::optional<mmltk::frameworks::gpu::DeviceContext> preview_context;
};

CudaPredictRuntime::CudaPredictRuntime(DirectComputeConfiguration configuration) : impl_(std::make_unique<Impl>(configuration)) {}
CudaPredictRuntime::~CudaPredictRuntime() = default;
void CudaPredictRuntime::Close() noexcept {
    try { impl_->resources.CloseSession(); }
    catch (...) { impl_->close_failed = true; }
}
bool CudaPredictRuntime::HasUnsafeCustody() const noexcept { return impl_->close_failed || impl_->session.HasUnsafeCustody() || (impl_->preview && impl_->preview->HasUnsafeCustody()); }
contracts::ComputeTerminal CudaPredictRuntime::Run(mmltk::backend::models::rfdetr::PredictRequest operation, const std::stop_token stop,
                                                    const ComputeProgressSink& progress, const ProductSink& products, const PlaybackGate& gate, VisualExtent maximum, const ContextProvider& current_context, const PreviewRetirement& retirement) {
    if (!retirement) throw std::invalid_argument("prediction preview retirement authority is unavailable");
    if (!retirement->admission_open()) throw contracts::UnavailableError("prediction receiver custody is unobservable");
    return impl_->resources.Run(
        [this, &progress, &products, &gate, stop, operation, maximum, &current_context, &retirement](const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
            operation.device_id = impl_->resources.device();
            std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> classes;
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
                        classes = summary.class_catalog;
                        class_count = summary.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground ? static_cast<int>(summary.class_catalog->size()) : summary.artifacts.config.num_classes;
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
                        if (!retirement->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
                        if (!pixels.preview_failure.empty()) throw std::runtime_error(std::string{pixels.preview_failure});
                        if (pixels.width > maximum.width || pixels.height > maximum.height)
                            throw std::runtime_error("Prediction preview exceeds the visual dimensions");
                        if (impl_->preview && impl_->preview->HasUnsafeSourceCustody())
                            throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "prediction preview source custody"};
                        if (!impl_->preview || impl_->preview_context != context) {
                            auto candidate = std::make_unique<detail::PredictionPreviewPool>(*impl_->config.execution, *context,
                                detail::PredictionPreviewPool::TransferOperations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister}, retirement);
                            impl_->preview = std::move(candidate);
                            impl_->preview_context = context;
                        }
                        auto raw = impl_->preview->Capture(pixels.chw, {pixels.width, pixels.height}, pixels.stream,
                            record.detections, annotations, classes, class_count, pixels.rgb8, pixels.custody, pixels.stop_source, pixels.source_control);
                        if (raw) products(Product{.extent = {pixels.width, pixels.height}, .raw = std::move(raw), .image_id = record.image_id, .source_index = record.dataset_index});
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
            if (!retirement_.admission_open() || !preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction has unobservable CUDA custody");
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
            if (!retirement_.admission_open() || !preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction has unobservable CUDA custody");
            const auto prior = state_;
            const auto& source = settings.settings.workflows.predict.source;
            const auto key = std::to_string(static_cast<unsigned>(source.kind)) + ":" +
                (source.kind == contracts::SourceKind::CompiledDataset ? source.compiled_path :
                 source.kind == contracts::SourceKind::SingleImage ? source.single_image_path : source.video_file_path);
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
                    .work = [this, settings = settings.settings, selection, source_key = key, video = state_.video, generation = *generation](const std::stop_token stop) mutable -> direct::LocalRun::Notification {
                        if (!preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction receiver custody is unobservable");
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
                        catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
                            if (error.terminal()) throw;
                            Product(std::unexpected{std::string{error.what()}}, source_key, video);
                        }
                        catch (const std::exception& error) { Product(std::unexpected{std::string{error.what()}}, source_key, video); }
                        auto terminal = runtime_->Run(std::move(*request), stop,
                            [this](const auto& progress) { Progress(progress); },
                            [this, &source_key, video](std::expected<PredictRuntime::Product, std::string> product) {
                                Product(std::move(product), source_key, video);
                            },
                            [this, stop](std::optional<double> timestamp, double fps) { return playback_.Wait(timestamp, fps, stop); }, {visual_.maximum_width, visual_.maximum_height}, [this] {
                                std::unique_lock lock(preview_context_mutex_, std::try_to_lock);
                                return lock.owns_lock() ? preview_context_ : std::nullopt;
                            }, preview_retirement_);
                        // Native source failures already fail RunAndWrite. Optional receiver
                        // failure seals custody without changing a completed semantic result.
                        if (runtime_->HasUnsafeCustody()) RetireRuntime();
                        if (!terminal.valid_worker_terminal()) throw std::runtime_error("prediction runtime returned an invalid terminal");
                        terminal.generation = generation;
                        return [this, terminal = std::move(terminal)]() mutable { Settled(std::move(terminal)); };
                    },
                    .failure = [this](std::exception_ptr failure) -> direct::LocalRun::Notification {
                        RetireRuntime();
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
    void Shutdown() noexcept { run_.StopAndJoin(); worker_.StopAndWait(); pending_product_.reset(); RetireRuntime(); worker_.FinishStoppedRetirement(); }
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
        if (!preview_retirement_->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
        if (!preview_context_) {
            preview_context_ = detail::CreatePredictionPreviewContext(execution, preview_retirement_);
        }
        return *preview_context_;
    }
    void RetireRuntime() noexcept {
        if (!runtime_) return;
        if (!runtime_->HasUnsafeCustody() && preview_retirement_->admission_open()) runtime_->Close();
        if (runtime_->HasUnsafeCustody() || !preview_retirement_->admission_open()) {
            std::move(retirement_lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(runtime_)), cudaErrorUnknown);
        } else runtime_.reset();
    }
    std::mutex preview_context_mutex_;
    std::optional<mmltk::frameworks::gpu::DeviceContext> preview_context_;
    struct PendingProduct final {
        std::expected<PredictRuntime::Product, std::string> product;
        std::uint64_t content_identity = 0U;
    };
    void Product(std::expected<PredictRuntime::Product, std::string> product, const std::string& source_key, bool video) {
        {
            std::scoped_lock lock(mutex_);
            preview_failure_.clear();
            if (product) {
                const auto image = video ? 0 : product->source_index;
                if (source_key_ != source_key || source_image_ != image) {
                    if (content_identity_ == std::numeric_limits<std::uint64_t>::max())
                        throw contracts::FailedError("prediction content identity exhausted");
                    ++content_identity_;
                    source_key_ = source_key;
                    source_image_ = image;
                }
            }
            pending_product_ = PendingProduct{std::move(product), content_identity_};
        }
        static_cast<void>(worker_.NotifyContinuation());
    }
    detail::VisualRuntimeOwner::Notification Render(mmltk::frameworks::gpu::SystemImageRuntime& runtime, std::stop_token stop) {
        if (stop.stop_requested()) return {};
        try {
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
            // A fallible draw may only acquire unselected replacement storage.
            mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput baseline;
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
                const auto category = detection.class_reference;
                labels.push_back({{{detection.bbox_xyxy[0], detection.bbox_xyxy[1]}, {detection.bbox_xyxy[2], detection.bbox_xyxy[3]}}, category, detection.class_domain, detection.score,
                    category >= 0 && static_cast<std::size_t>(category) < palette.size() ? palette[category] : contracts::AnnotationColor{},
                    detection.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && category >= 0 && static_cast<std::size_t>(category) < classes.size() ? classes[category] : std::string{}});
                const auto& label = labels.back();
                if (!label.box.valid() || !label.color.valid() || !std::isfinite(label.confidence) || label.name.size() > mmltk::frameworks::reflection::kMaximumNameBytes)
                    throw std::runtime_error("Prediction preview metadata exceeds the visual product limits");
            }
            product.raw->Draw(runtime, candidate);
            const auto frame = visual_frame({PresentationSourceKind::Predict, 1U}, product.extent, candidate.revision());
            detail::VisualRuntimeOwner::Notification notification = [this, frame, labels = std::move(labels), identity = product.image_id, content_identity = pending.content_identity]() mutable {
                PredictSnapshot changed;
                {
                    std::scoped_lock lock(mutex_);
                    const auto revision = detail::PredictRevision::Frame(state_.revision, state_.operation.active, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
                    if (!revision) return;
                    state_.frame = frame;
                    state_.labels = std::move(labels);
                    state_.image_id = identity;
                    state_.content_identity = content_identity;
                    state_.revision = *revision;
                    changed = state_;
                }
                Publish(PredictChanged{std::move(changed)});
            };
            static_cast<void>(runtime.CommitOutput(std::move(candidate)));
            return notification;
        } catch (...) {
            const auto failure = std::current_exception();
            if (!preview_retirement_->admission_open()) {
                static_cast<void>(runtime.Retire(failure));
                throw;
            }
            if (mmltk::frameworks::gpu::is_image_execution_failure(failure) ||
                mmltk::frameworks::gpu::find_image_failure<mmltk::backend::ml::runtime::CudaOperationError>(failure)) throw;
            try { std::rethrow_exception(failure); }
            catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
                if (error.terminal()) {
                    static_cast<void>(runtime.Retire(failure));
                    throw;
                }
            }
            catch (...) {}
            // PublishRetained settled ordinary partial writes; candidate rollback
            // leaves the selected output valid. Keep its owning runtime alive.
            return [this, message = visual_failure_detail(failure, "prediction rendering failed")]() mutable {
                PreviewFailed(std::move(message));
            };
        }
    }
    void Progress(const contracts::ComputeProgress& progress) noexcept {
        PredictProgressState changed;
        {
            std::scoped_lock lock(mutex_);
            if (!state_.operation.active || !contracts::compute_progress_follows(progress, state_.operation.progress.sequence)) return;
            const auto revision = detail::PredictRevision::Progress(
                state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
            if (!revision) return;
            state_.revision = *revision;
            state_.operation.progress = progress;
            if (state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::Running) state_.operation.terminal.detail.clear();
            changed = mmltk::frameworks::reflection::project_record<PredictProgressState>(state_);
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
    std::uint64_t content_identity_ = 0U;
    std::int64_t source_image_ = 0;
    detail::PredictionPlayback playback_;
    PredictRuntimeFactory factory_;
    mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
    mmltk::frameworks::gpu::TerminalCudaRetirementLease retirement_lease_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
    // Current pool, in-flight Draw, pending old product, and context construction.
    PredictRuntime::PreviewRetirement preview_retirement_ =
        std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity + 3U);
    std::shared_ptr<PredictRuntime> runtime_;
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
VisualSourceObservation PredictSystem::ObserveSource() const {
    std::scoped_lock lock(impl_->mutex_);
    return visual_source::Observe(impl_->state_);
}
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
