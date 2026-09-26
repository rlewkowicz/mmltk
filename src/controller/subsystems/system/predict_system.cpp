#include "src/controller/presentation/annotation_palette.h"
#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "predict_system.h"
#include <cuda_runtime_api.h>
#include <cmath>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "detail/cuda_runtime_resources.h"
#include "detail/predict_revision.h"
#include "detail/prediction_playback.h"
#include "detail/prediction_preview.h"
#include "detail/prediction_output.h"
#include "detail/prediction_sampling.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/common/system/execution_policy.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/services/run_output.h"
#include "src/controller/runtime/local_run.h"
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
CudaPredictRuntime::CudaPredictRuntime(DirectComputeConfiguration configuration, services::RuntimeDiagnosticTarget diagnostics)
    : diagnostics_(std::move(diagnostics)), impl_(std::make_unique<Impl>(configuration)) {}
CudaPredictRuntime::~CudaPredictRuntime() = default;
void CudaPredictRuntime::Close() noexcept {
 try {
  impl_->resources.Retire();
  impl_->preview.reset();
  impl_->preview_context.reset();
 } catch (...) { impl_->close_failed = true; }
}
bool CudaPredictRuntime::HasUnsafeCustody() const noexcept {
 return impl_->resources.HasUnsafeCustody() || impl_->close_failed || impl_->session.HasUnsafeCustody() || (impl_->preview && impl_->preview->HasUnsafeCustody());
}
contracts::ComputeTerminal CudaPredictRuntime::Run(mmltk::backend::models::rfdetr::PredictRequest operation, const std::stop_token stop, const ComputeProgressSink& progress,
 const ProductSink& products, const PlaybackGate& gate, VisualExtent maximum, const ContextProvider& current_context, const PreviewRetirement& retirement, const ComputeArtifactSink& published,
 const PredictionRunOutput& output, std::uint64_t generation, const ExecutionSink& admitted) {
 if (!retirement) throw std::invalid_argument("prediction preview retirement authority is unavailable");
 if (!retirement->admission_open()) throw contracts::UnavailableError("prediction receiver custody is unobservable");
 return impl_->resources.Run(
  [this, generation, &progress, &products, &gate, stop, operation, maximum, &current_context, &retirement, &published, &output, &admitted](
   const mmltk::backend::ml::runtime::BorrowedCommandStream stream) mutable {
   if (operation.device_id != impl_->resources.device()) throw std::invalid_argument("prediction device disagrees with admitted execution");
   diagnostics_.Emit([&] {
    return services::RuntimeDiagnosticFact{.owner = contracts::DiagnosticOwner::Prediction,
     .event = "workflow.gpu_execution",
     .participant = "predict",
     .sequence = generation,
     .value = 0U,
     .detail = 1U,
     .device = impl_->resources.device()};
   });
   std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> classes;
   int class_count = 0;
   std::uint64_t sequence = 0U;
   detail::PredictionOutput media(impl_->config, operation.source_kind, output, current_context ? current_context() : std::nullopt);
   const bool full_video =
    operation.source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::VideoFile && media.Enabled() && output.saving.video_mode == contracts::PredictionVideoSaving::Full;
   using MediaBegin = std::function<void(const mmltk::backend::media::video::VideoMediaInfo&)>;
   using AudioDelivery = std::function<void(const mmltk::backend::media::video::VideoAudioPacket&)>;
   std::unique_ptr<mmltk::backend::models::rfdetr::PredictionJsonWriter> json;
   if (operation.source_kind != mmltk::backend::models::rfdetr::PredictSourceKind::VideoFile && !operation.output_path.empty())
    json = std::make_unique<mmltk::backend::models::rfdetr::PredictionJsonWriter>(operation);
   mmltk::backend::models::rfdetr::PredictionRunResult result;
   try {
    result = impl_->session.Run(operation, stream,
     {
      .stop = stop,
      .source_pixels = static_cast<bool>(products),
      .encoded_masks = static_cast<bool>(json),
      .demand =
       [&](std::int64_t index) {
        const bool save = media.Wants(index);
        return mmltk::backend::models::rfdetr::PredictionDemand{.native_pixels = save, .preview_masks = save && output.preview.masks};
       },
      .maximum_pixel_width = maximum.width,
      .maximum_pixel_height = maximum.height,
      .before_source = gate.admission,
      .before_frame = gate.frame,
      .media_begin = full_video ? MediaBegin{[&](const auto& info) { media.Media(info); }} : MediaBegin{},
      .audio = full_video ? AudioDelivery{[&](const auto& packet) { media.Audio(packet); }} : AudioDelivery{},
      .begin =
       [&](const auto& summary) {
        media.Begin(summary);
        if (json) json->Begin(summary);
        if (!products) return;
        try {
         classes = summary.class_catalog;
         class_count = summary.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground ? static_cast<int>(summary.class_catalog->size()) : summary.artifacts.config.num_classes;
        } catch (const std::exception& error) {
         if (products) products(std::unexpected{std::string{error.what()}});
        }
       },
      .completed =
       [&](const auto& record, const auto pixels, const auto& annotations) {
        auto saved_raw = media.Capture(record, pixels, annotations);
        if (json) json->Append(record);
        if (!products || !classes) return;
        try {
         const auto context = current_context ? current_context() : std::nullopt;
         if (!context) {
          products(std::unexpected{std::string{"Prediction preview context is recovering"}});
          return;
         }
         if (!retirement->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
         if (!pixels.preview_failure.empty()) throw std::runtime_error(std::string{pixels.preview_failure});
         if (pixels.width > maximum.width || pixels.height > maximum.height) throw std::runtime_error("Prediction preview exceeds the visual dimensions");
         if (saved_raw) {
          products(Product{.extent = {pixels.width, pixels.height}, .raw = std::move(saved_raw), .image_id = record.image_id, .source_index = record.dataset_index});
          return;
         }
         if (impl_->preview && impl_->preview->HasUnsafeSourceCustody()) throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "prediction preview source custody"};
         if (!impl_->preview || impl_->preview_context != context) {
          auto candidate = std::make_unique<detail::PredictionPreviewPool>(
           *context->execution(), *context, detail::PredictionPreviewPool::TransferOperations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister}, retirement);
          impl_->preview = std::move(candidate);
          impl_->preview_context = context;
         }
         auto raw = impl_->preview->Capture(pixels.chw, {pixels.width, pixels.height}, pixels.stream, record.detections, annotations, classes, class_count, pixels.rgb8, pixels.custody,
          pixels.stop_source, pixels.source_control, {}, false, pixels.device);
         if (raw) products(Product{.extent = {pixels.width, pixels.height}, .raw = std::move(raw), .image_id = record.image_id, .source_index = record.dataset_index});
        } catch (const mmltk::backend::ml::runtime::CudaOperationError&) { throw; } catch (const std::exception& error) {
         products(std::unexpected{std::string{error.what()}});
        } catch (...) { products(std::unexpected{std::string{"Prediction preview custody transfer failed"}}); }
       },
      .progress =
       [&](std::size_t completed, std::size_t total) {
        if (progress) progress({++sequence, completed, total, "Processed"});
       },
      .decoded =
       [&](std::size_t decoded, std::size_t total) {
        if (progress) progress({++sequence, decoded, total, "Decoded"});
       },
      .admitted = admitted,
      .retirement = retirement,
     });
   } catch (...) {
    const auto failure = std::current_exception();
    media.Finish(false);
    std::rethrow_exception(failure);
   }
   result.cancelled = result.cancelled || stop.stop_requested();
   media.Finish(!result.cancelled);
   if (!result.cancelled && json) {
    json->Complete();
    if (published) published(operation.output_path);
   }
   return contracts::make_compute_terminal(result.cancelled ? contracts::ComputeOperationOutcome::Cancelled : contracts::ComputeOperationOutcome::Succeeded, 0U, result.processed_images,
    result.cancelled || !json ? std::string{} : operation.output_path.string());
  },
  {}, false);  // PredictionSession owns the atomic output completion boundary.
}
class PredictSystem::Impl final {
 friend class PredictSystem;
 WorkspaceInput input_;

public:
 Impl(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, const VisualDeviceSettings visual, PredictRuntimeFactory factory, SystemEventSink<event_type> events,
  DirectComputeResolver resolver)
     : settings_(settings),
       dataset_(dataset),
       model_(model),
       visual_(visual),
       events_(std::move(events)),
       factory_(std::move(factory)),
       resolver_(std::move(resolver)),
       worker_(
        [this, visual, topology = mmltk::common::system::NumaTopology::Capture(), execution = std::optional<mmltk::frameworks::gpu::DeviceExecution>{}](auto revisions) mutable {
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
         return make_visual_runtime(std::move(config));
        },
        [this](const std::exception_ptr failure) { RenderingFailed(failure); }) {
  if (!visual_.valid()) throw contracts::UnavailableError("prediction visual device is unavailable");
  if (!factory_ || !resolver_) throw contracts::UnavailableError("prediction runtime factory or placement resolver is unavailable");
  worker_.RegisterContinuation([this](auto& runtime, auto stop) { return Render(runtime, stop); }, {}, true);
 }
 ~Impl() { Shutdown(); }
 [[nodiscard]] PredictSnapshot Start(contracts::PredictWorkflowIntent intent) {
  {
   std::scoped_lock lock(mutex_);
   RequireStartAdmissionLocked();
  }
  // CLEANUP-IGNORE: Predict begins from canonical settings but then owns a distinct visual operation path.
  const auto settings = settings_.materialization_facts();
  if (!settings.loaded) throw contracts::UnavailableError("settings are unavailable");
  // CLEANUP-IGNORE: Predict materialization additionally owns private visual admission and publication.
  const auto selection = model_.selection();
  const auto& configured_request = settings.settings.workflows.predict.request;
  const auto configuration = resolver_(configured_request.device_id, configured_request.numa_node);
  const auto receiver_execution = resolve_visual_device_execution(visual_);
  PredictSnapshot admitted;
  {
   std::scoped_lock admission_lock(mutex_);
   RequireStartAdmissionLocked();
   const auto prior = state_;
   const auto& source = settings.settings.workflows.predict.source;
   const auto key = std::to_string(static_cast<unsigned>(source.kind)) + ":" +
                    (source.kind == contracts::SourceKind::CompiledDataset ? source.compiled_path
                     : source.kind == contracts::SourceKind::SingleImage   ? source.single_image_path
                                                                           : source.video_file_path);
   const auto generation = contracts::next_compute_generation(state_.operation.generation_frontier);
   if (!generation) throw contracts::FailedError("prediction operation generation exhausted");
   state_.revision = detail::PredictRevision::Admit(state_.revision);
   contracts::begin_compute(state_.operation, *generation, "Preparing selected prediction inputs");
   state_.execution = mmltk::backend::models::rfdetr::derive_execution_facts(configured_request, settings.revision);
   state_.execution.operation_generation = *generation;
   state_.paused = false;
   preview_failure_.clear();
   state_.video = source.kind == contracts::SourceKind::VideoFile;
   playback_.Reset();
   try {
    run_.Start({
     .policy = configuration.worker_policy(),
     .work = [this, settings = settings.settings, intent, selection, source_key = key, video = state_.video, generation = *generation, configuration, receiver_execution](
              const std::stop_token stop) mutable -> direct::LocalRun::Notification {
      if (!preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction receiver custody is unobservable");
      contracts::ArtifactInspection inspection;
      if (!stop.stop_requested() && settings.workflows.predict.source.kind == contracts::SourceKind::CompiledDataset)
       inspection = dataset_.Inspect({settings.workflows.predict.source.compiled_path, {}, {}}, selection.key.preset, selection.key.resolution, stop);
      if (stop.stop_requested()) return [this, generation] { Settled(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, generation)); };
      settings.workflows.predict.saving = intent.saving;
      auto request = subsystems::system::ComputeIntentMaterializer::Predict(settings, inspection, selection);
      if (!request) throw contracts::InvalidIntentError(request.error().detail);
      if (stop.stop_requested()) return [this, generation] { Settled(contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled, generation)); };
      if (request->source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::ImageFiles && !std::filesystem::is_regular_file(request->image_inputs.front().image_path))
       throw contracts::InvalidIntentError("prediction image is not a readable regular file");
      if (request->source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::VideoFile && !std::filesystem::is_regular_file(request->video_path))
       throw contracts::InvalidIntentError("prediction video is not a readable regular file");
      PredictionRunOutput media_output;
      media_output.saving = intent.saving;
      media_output.preview = intent.preview;
      if (request->source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::CompiledDataset) {
       if (inspection.splits.empty()) throw contracts::InvalidIntentError("prediction dataset is empty");
       media_output.population = inspection.splits.front().image_count;
       if (intent.compiled_population && intent.compiled_population != media_output.population) throw contracts::InvalidIntentError("prediction dataset count changed after inspection");
       if (!intent.compiled_population) media_output.saving.compiled_total = std::min(media_output.saving.compiled_total, media_output.population);
       if (intent.saving.compiled_enabled && intent.saving.compiled_mode == contracts::PredictionCompiledSampling::Total && media_output.saving.compiled_total > media_output.population)
        throw contracts::InvalidIntentError("prediction Total exceeds the admitted dataset count");
       if (media_output.saving.compiled_enabled) {
        const auto count = media_output.saving.compiled_mode == contracts::PredictionCompiledSampling::Percent
                            ? detail::PredictionSelection::Percent(media_output.population, media_output.saving.compiled_percent)
                            : media_output.saving.compiled_total;
        media_output.processing_population = detail::PredictionSelection::Eligible(media_output.population, request->limit_images, count);
       }
      }
      const auto& output = settings.workflows.predict.output;
      const auto directory = services::reserve_run_output(contracts::workflow_output_root(output, contracts::FeatureId::Predict), !output.automatic);
      media_output.directory = directory;
      media_output.progress = [this](const auto& facts) {
       {
        std::scoped_lock lock(mutex_);
        auto artifacts = std::move(state_.operation.output.artifacts);
        state_.operation.output = facts;
        if (const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested))
         state_.revision = *revision;
        for (const auto& path : artifacts)
         if (std::find(state_.operation.output.artifacts.begin(), state_.operation.output.artifacts.end(), path) == state_.operation.output.artifacts.end() &&
             state_.operation.output.artifacts.size() < contracts::kWorkflowArtifactCapacity)
          state_.operation.output.artifacts.push_back(path);
       }
       Publish(PredictChanged{snapshot()});
      };
      if (!request->output_path.empty()) request->output_path = directory / request->output_path.filename();
      {
       std::scoped_lock lock(mutex_);
       state_.operation.output.directory = directory.string();
       if (const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested))
        state_.revision = *revision;
      }
      Publish(PredictChanged{snapshot()});
      if (runtime_ && runtime_configuration_ != configuration) RetireRuntime();
      if (!retirement_.admission_open() || !preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction runtime retirement failed");
      const bool initial_runtime = !runtime_;
      if (!runtime_) {
       runtime_ = construct_compute_runtime(factory_, configuration, construction_failed_);
       runtime_configuration_ = configuration;
      }
      if (!runtime_) throw std::runtime_error("prediction runtime factory returned no runtime");
      try {
       if (initial_runtime) static_cast<void>(PreviewContext(receiver_execution));
      } catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
       if (error.terminal()) throw;
       Product(std::unexpected{std::string{error.what()}}, source_key, video);
      } catch (const std::exception& error) { Product(std::unexpected{std::string{error.what()}}, source_key, video); }
      auto terminal = runtime_->Run(
       std::move(*request), stop, [this](const auto& progress) { Progress(progress); },
       [this, &source_key, video](std::expected<PredictRuntime::Product, std::string> product) { Product(std::move(product), source_key, video); },
       {.admission = [this, stop] { return playback_.WaitAdmission(stop); }, .frame = [this, stop](std::optional<double> timestamp, double fps) { return playback_.Wait(timestamp, fps, stop); }},
       {visual_.maximum_width, visual_.maximum_height},
       [this] {
        std::unique_lock context_lock(preview_context_mutex_, std::try_to_lock);
        return context_lock.owns_lock() ? preview_context_ : std::nullopt;
       },
       preview_retirement_, [this](const auto& path) { Artifact(path); }, media_output, generation,
       [this, generation](const auto& admitted_execution) {
        {
         std::scoped_lock lock(mutex_);
         if (state_.operation.generation_frontier != generation || !state_.operation.active) return;
         auto facts = admitted_execution;
         facts.settings_revision = state_.execution.settings_revision;
         facts.operation_generation = generation;
         if (state_.execution.admitted_capacity) {
          if (state_.execution != facts) throw std::logic_error("prediction capacity changed during an admitted operation");
          return;
         }
         state_.execution = facts;
         if (const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested))
          state_.revision = *revision;
        }
        Publish(PredictChanged{snapshot()});
       });
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
    contracts::cancel_compute(state_.operation);
   }
  }
  if (active) static_cast<void>(run_.Stop());
  return snapshot();
 }
 void Shutdown() noexcept {
  inspection_run_.StopAndJoin();
  run_.StopAndJoin();
  worker_.StopAndWait();
  pending_product_.reset();
  RetireRuntime();
  worker_.FinishStoppedRetirement();
 }
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
   if (!state_.frame.valid()) return {};
   committed = state_.frame;
  }
  return borrow_matching_visual_product(committed, worker_);
 }

private:
 void RequireStartAdmissionLocked() const {
  if (state_.operation.active || state_.inspection.active) throw contracts::BusyError("prediction is busy");
  if (construction_failed_ || !retirement_.admission_open() || !preview_retirement_->admission_open()) throw contracts::UnavailableError("prediction has unobservable CUDA custody");
 }
 mmltk::frameworks::gpu::DeviceContext PreviewContext(const mmltk::frameworks::gpu::DeviceExecution& execution) {
  std::scoped_lock lock(preview_context_mutex_);
  if (!preview_retirement_->admission_open()) throw std::runtime_error("prediction preview retirement admission is closed");
  if (!preview_context_) { preview_context_ = detail::CreatePredictionPreviewContext(execution, preview_retirement_); }
  return *preview_context_;
 }
 void RetireRuntime() noexcept {
  if (!runtime_) return;
  if (!runtime_->HasUnsafeCustody() && preview_retirement_->admission_open()) runtime_->Close();
  if (runtime_->HasUnsafeCustody() || !preview_retirement_->admission_open()) {
   std::move(retirement_lease_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(runtime_)), cudaErrorUnknown);
  } else
   runtime_.reset();
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
     if (content_identity_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("prediction content identity exhausted");
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
    preview_palette_ = mmltk::controller::annotation_class_palette(static_cast<std::size_t>(product.raw->class_count()));
    preview_class_count_ = product.raw->class_count();
   }
   const auto& palette = preview_palette_;
   const auto& classes = product.raw->classes();
   labels.reserve(product.raw->predictions().size());
   for (const auto& detection : product.raw->predictions()) {
    const auto category = detection.class_reference;
    labels.push_back({{{detection.bbox_xyxy[0], detection.bbox_xyxy[1]}, {detection.bbox_xyxy[2], detection.bbox_xyxy[3]}}, category, detection.class_domain, detection.score,
     category >= 0 && static_cast<std::size_t>(category) < palette.size() ? palette[category] : contracts::AnnotationColor{},
     detection.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && category >= 0 && static_cast<std::size_t>(category) < classes.size() ? classes[category]
                                                                                                                                                                       : std::string{}});
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
   if (mmltk::frameworks::gpu::is_image_execution_failure(failure) || mmltk::frameworks::gpu::find_image_failure<mmltk::backend::ml::runtime::CudaOperationError>(failure)) throw;
   try {
    std::rethrow_exception(failure);
   } catch (const mmltk::frameworks::gpu::CudaContextFailure& error) {
    if (error.terminal()) {
     static_cast<void>(runtime.Retire(failure));
     throw;
    }
   } catch (...) {}
   // PublishRetained settled ordinary partial writes; candidate rollback
   // leaves the selected output valid. Keep its owning runtime alive.
   return [this, message = visual_failure_detail(failure, "prediction rendering failed")]() mutable { PreviewFailed(std::move(message)); };
  }
 }
 void Progress(const contracts::ComputeProgress& progress) noexcept {
  PredictProgressState changed;
  {
   std::scoped_lock lock(mutex_);
   if (!state_.operation.active || !contracts::compute_progress_follows(progress, state_.operation.progress.sequence)) return;
   const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
   if (!revision) return;
   state_.revision = *revision;
   static_cast<void>(contracts::advance_compute(state_.operation, progress));
   changed = mmltk::frameworks::reflection::project_record<PredictProgressState>(state_);
  }
  Publish(PredictProgress{std::move(changed)});
 }
 void PreviewFailed(std::string detail) noexcept {
  PredictSnapshot changed;
  {
   std::scoped_lock lock(mutex_);
   if (preview_failure_ == detail) return;
   const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested);
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
 void Artifact(const std::filesystem::path& path) {
  {
   std::scoped_lock lock(mutex_);
   if (state_.operation.output.artifacts.size() == contracts::kWorkflowArtifactCapacity) throw std::runtime_error("prediction artifact publication capacity exhausted");
   state_.operation.output.artifacts.push_back(path);
   if (const auto revision = detail::PredictRevision::Progress(state_.revision, state_.operation.terminal.outcome == contracts::ComputeOperationOutcome::CancellationRequested))
    state_.revision = *revision;
  }
  Publish(PredictChanged{snapshot()});
 }
 void Settled(contracts::ComputeTerminal terminal) noexcept {
  PredictSnapshot changed;
  {
   std::scoped_lock lock(mutex_);
   const auto revision = detail::PredictRevision::Complete(state_.revision);
   if (!revision) return;
   state_.revision = *revision;
   contracts::complete_compute(state_.operation, std::move(terminal));
   state_.paused = false;
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
   state_.operation.terminal = contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Failed, state_.operation.generation_frontier, 0U, {}, detail);
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
 std::atomic_bool construction_failed_ = false;
 DirectComputeResolver resolver_;
 std::optional<DirectComputeConfiguration> runtime_configuration_;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease retirement_lease_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
 // Frames and borrowed index buffers (2 * slots), six session host buffers,
 // two source claims, four receiver/context claims and two growth claims.
 PredictRuntime::PreviewRetirement preview_retirement_ = std::make_shared<mmltk::frameworks::gpu::TerminalCudaRetirementOwner>(2U * detail::PredictionPreviewPool::kSlotCapacity + 14U);
 std::shared_ptr<PredictRuntime> runtime_;
 direct::LocalRun run_;
 direct::LocalRun inspection_run_;
 detail::VisualRuntimeOwner worker_;
};
PredictSystem::PredictSystem(SettingsSystem& settings, DatasetSystem& dataset, ModelSystem& model, const VisualDeviceSettings visual, PredictRuntimeFactory factory, SystemEventSink<event_type> events,
 DirectComputeResolver resolver)
    : impl_(std::make_unique<Impl>(settings, dataset, model, visual, std::move(factory), std::move(events), std::move(resolver))) {}
PredictSystem::~PredictSystem() = default;
PredictSnapshot PredictSystem::Inspect(PredictionSourceQuery query) {
 if (query.path.empty()) throw contracts::InvalidIntentError("prediction dataset path is empty");
 const auto selected = impl_->model_.selection();
 {
  std::scoped_lock lock(impl_->mutex_);
  if (impl_->state_.operation.active || impl_->state_.inspection.active) throw contracts::BusyError("prediction dataset inspection is busy");
  impl_->state_.revision = detail::PredictRevision::Admit(impl_->state_.revision);
  impl_->state_.inspection = {query.path, 0U, true, {}};
 }
 try {
  impl_->inspection_run_.Start({.work = [this, path = query.path, selected](std::stop_token stop) -> direct::LocalRun::Notification {
                                 auto result = impl_->dataset_.Inspect({path, {}, {}}, selected.key.preset, selected.key.resolution, stop);
                                 return [this, path, result = std::move(result)] {
                                  {
                                   std::scoped_lock lock(impl_->mutex_);
                                   if (impl_->state_.inspection.path != path) return;
                                   impl_->state_.revision = detail::PredictRevision::Admit(impl_->state_.revision);
                                   auto& facts = impl_->state_.inspection;
                                   facts.active = false;
                                   if (result.available() && result.splits.size() == 1U)
                                    facts.count = result.splits.front().image_count;
                                   else
                                    facts.error = result.detail.empty() ? "prediction dataset is unavailable" : result.detail;
                                  }
                                  impl_->Publish(PredictChanged{snapshot()});
                                 };
                                },
   .failure = [this, path = query.path](std::exception_ptr error) -> direct::LocalRun::Notification {
    return [this, path, error] {
     {
      std::scoped_lock lock(impl_->mutex_);
      if (impl_->state_.inspection.path != path) return;
      impl_->state_.revision = detail::PredictRevision::Admit(impl_->state_.revision);
      impl_->state_.inspection.active = false;
      impl_->state_.inspection.error = visual_failure_detail(error, "prediction dataset inspection failed");
     }
     impl_->Publish(PredictChanged{snapshot()});
    };
   }});
 } catch (...) {
  std::scoped_lock lock(impl_->mutex_);
  impl_->state_.inspection.active = false;
  throw;
 }
 return snapshot();
}
PredictSnapshot PredictSystem::Start(contracts::PredictWorkflowIntent intent) { return impl_->Start(intent); }
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
 // CLEANUP-IGNORE: Sealed source forwarding retains Predict ownership; VisualRuntimeOwner shares the implementation.
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
