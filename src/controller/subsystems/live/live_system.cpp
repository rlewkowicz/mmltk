#include "src/controller/presentation/workspace_input.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/live/live_analysis_configuration.h"
#include "src/controller/services/settings/settings_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/presentation/annotation_palette.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include <cmath>
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/frameworks/gpu/image/image_failure.h"
#include "src/frameworks/gpu/runtime/system_image_runtime.h"
#include <condition_variable>
#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/controller/contracts/compute.h"
namespace mmltk::controller {
namespace {
std::optional<LiveImageMetadata> project_live_metadata(const VisualFrame& frame, const mmltk::backend::media::live::LiveAnalysisProduct& analysis) {
 LiveImageMetadata metadata{.frame = frame, .analyzed = analysis.analyzed()};
 for (const auto& region : analysis.regions()) {
  const auto palette = annotation_class_palette(region.class_catalog ? region.class_catalog->size() : 0U);
  for (std::size_t index = 0; index < region.count; ++index) {
   const auto category = region.class_references[index];
   const auto& box = region.boxes_xyxy[index];
   VisualObjectLabel label{.box = {{box[0], box[1]}, {box[2], box[3]}}, .class_reference = category, .class_domain = region.class_domain, .confidence = region.confidences[index]};
   if (region.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && region.class_catalog && category >= 0 &&
       static_cast<std::size_t>(category) < region.class_catalog->size()) {
    label.name = region.class_catalog->names()[category];
    label.color = palette[category];
    if (region.keypoints_available) {
     label.skeleton.schema = region.class_catalog->keypoints(category);
     const auto count = region.keypoint_counts[index];
     if (count < 0 || static_cast<std::size_t>(count) != label.skeleton.schema.names.size()) throw contracts::InvalidIntentError("Live joint meaning differs from its class schema");
     for (int joint = 0; joint < count; ++joint) {
      const auto& point = region.keypoints[index * region.keypoint_capacity + joint];
      VisualKeypoint value{.point = {point.x, point.y}, .confidence = point.confidence};
      value.Sanitize();
      if (std::isfinite(point.covariance_xx) && std::isfinite(point.covariance_xy) && std::isfinite(point.covariance_yy))
       value.covariance = std::array{point.covariance_xx, point.covariance_xy, point.covariance_yy};
      label.skeleton.joints.push_back(value);
     }
    }
   }
   if (metadata.labels.size() == contracts::kAnnotationObjectCapacity) return std::nullopt;
   metadata.labels.push_back(std::move(label));
  }
 }
 return metadata;
}
std::shared_ptr<const VisualDocument> live_document(const VisualFrame& frame, std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct> product, const LiveImageMetadata& metadata) {
 auto document = std::make_shared<VisualDocument>();
 document->BindResource(contracts::WorkspaceResource::From("live://frame", frame.revision));
 std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> catalog;
 struct Mask final {
  mmltk::backend::ml::runtime::AnalysisRegion region;
  std::span<const std::uint8_t> pixels;
 };
 auto masks = std::make_shared<std::vector<Mask>>();
 std::size_t label_index = 0;
 for (const auto& region : product->regions()) {
  if (region.class_catalog) {
   if (catalog && !catalog->ordered_equal(*region.class_catalog)) throw contracts::InvalidIntentError("Live regions disagree on class meaning");
   if (!catalog) {
    catalog = region.class_catalog;
    document->SetCatalog(*catalog);
   }
  }
  for (std::size_t index = 0; index < region.count; ++index, ++label_index) {
   const auto& label = metadata.labels.at(label_index);
   if (!catalog || label.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground) continue;
   contracts::AnnotationObject object{.name = contracts::AnnotationText::From("object " + std::to_string(document->scene.objects.size() + 1U)), .box = label.box};
   object.category = static_cast<std::uint16_t>(label.class_reference);
   object.mask.present = region.masks_available;
   const auto joints = label.skeleton.Annotations();
   const auto& area = region.source_region;
   document->AppendSourceObject(std::move(object),
    {static_cast<float>(area.x) / static_cast<float>(frame.extent.width), static_cast<float>(area.y) / static_cast<float>(frame.extent.height),
     static_cast<float>(area.x + area.width) / static_cast<float>(frame.extent.width), static_cast<float>(area.y + area.height) / static_cast<float>(frame.extent.height)},
    joints);
   Mask mask{area, {}};
   if (region.masks_available) {
    const auto pixels = static_cast<std::size_t>(area.width) * area.height;
    const auto source = region.masks.subspan(index * pixels, pixels);
    mask.pixels = source;
   }
   masks->push_back(std::move(mask));
  }
 }
 if (!catalog) document->scene.categories.push_back({.value = "object"});
 document->scene.palette = annotation_class_palette(document->scene.categories.size());
 document->mask_contains = [masks, product = std::move(product), extent = frame.extent](std::size_t index, float x, float y) {
  if (index >= masks->size()) return false;
  const auto& mask = masks->at(index);
  const auto px = std::floor(x * static_cast<float>(extent.width)) - static_cast<float>(mask.region.x), py = std::floor(y * static_cast<float>(extent.height)) - static_cast<float>(mask.region.y);
  return px >= 0 && py >= 0 && px < static_cast<float>(mask.region.width) && py < static_cast<float>(mask.region.height) && !mask.pixels.empty() &&
         mask.pixels[static_cast<std::size_t>(py) * mask.region.width + static_cast<std::size_t>(px)] != 0;
 };
 return document;
}
}  // namespace
LiveAnalysisRead::LiveAnalysisRead(
 VisualFrame frame, mmltk::frameworks::gpu::BorrowedImageProductReadView image, std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct> analysis) noexcept
    : frame_(frame), analysis_(std::move(analysis)), image_(std::move(image)) {}
class LiveSystem::Impl final {
 WorkspaceInput input_;

public:
 Impl(const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics, SettingsSystem* application_settings, ModelSystem* model)
     : settings_(settings),
       application_settings_(application_settings),
       model_(model),
       events_(std::move(events)),
       diagnostics_(diagnostics),
       worker_(std::move(factory), [this](const std::exception_ptr failure) {
        pending_output_ = {};
        const auto reported = mmltk::frameworks::gpu::combine_image_failures(failure, worker_.FinishDeferredRetirement());
        auto detail = visual_failure_detail(reported, "Live GPU worker failed");
        LiveSnapshot settled;
        {
         std::scoped_lock lock(mutex_);
         if (!state_.running) return;
         state_.running = false;
         state_.cancellation_requested = false;
         state_.frame = {};
         if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
         settled = state_;
        }
        settled_.notify_all();
        report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Live, settings_.device, detail);
        Publish(event_type{LiveFailed{std::move(settled), std::move(detail)}});
       }) {
  if (!settings_.valid()) throw contracts::InvalidIntentError("Live device settings are invalid");
  worker_.RegisterContinuation([this](auto& runtime, const std::stop_token stop) { return Capture(runtime, stop); }, {}, true);
 }
 LiveSnapshot Start(const LiveStart request) {
  std::shared_ptr<const LiveAnalysisConfiguration> analysis;
  if (application_settings_) {
   const auto settings = application_settings_->snapshot().settings_state;
   if (settings.workflows.predict.live_analysis_enabled) {
    if (!model_) throw contracts::UnavailableError("Live model preparation is unavailable");
    auto selected = subsystems::system::ComputeIntentMaterializer::PredictionModel(settings, model_->selection());
    if (!selected) throw contracts::InvalidIntentError(selected.error().detail);
    if (selected->onnx_path.empty() && selected->tensorrt_path.empty())
     throw contracts::InvalidIntentError("Live analysis requires a prepared ONNX or TensorRT artifact; export and select it in Predict");
    if (selected->device_id != settings_.device) throw contracts::InvalidIntentError("Live analysis and capture must use the same CUDA device");
    analysis = std::make_shared<const LiveAnalysisConfiguration>(std::move(*selected), settings.workflows.predict.show_keypoints, settings.workflows.predict.show_uncertainty);
   }
  }
  if (!request.extent.valid() || request.extent.width > settings_.maximum_width || request.extent.height > settings_.maximum_height || request.frames_per_second == 0U ||
      request.frames_per_second > 240U)
   throw contracts::InvalidIntentError("Live capture settings are invalid");
  const auto cadence = std::chrono::microseconds{1'000'000U / request.frames_per_second};
  std::scoped_lock state_lock(mutex_);
  if (state_.running) throw contracts::BusyError("Live is already running");
  const auto admitted_revision = contracts::next_compute_generation(state_.revision);
  if (!admitted_revision) throw contracts::FailedError("Live observation revision exhausted");
  std::stop_source next_stop;
  if (!worker_.SubmitDiscrete(
       [this, request, cadence, analysis = std::move(analysis), stop = next_stop.get_token()](
        mmltk::frameworks::gpu::SystemImageRuntime& runtime, const std::stop_token) -> detail::VisualRuntimeOwner::Notification {
   auto* const algorithm = dynamic_cast<LiveAlgorithm*>(runtime.model());
   if (algorithm == nullptr) throw std::runtime_error("Live capture data plane is unavailable");
   algorithm->SetOutputAvailableSink([this] { static_cast<void>(worker_.NotifyContinuation()); });
   try {
    if (!stop.stop_requested()) algorithm->Start(request, analysis, stop);
   } catch (const LiveProviderAdmissionFailure&) {
    const auto failure = std::current_exception();
    static_cast<void>(algorithm->Stop());
    // A refused provider start has not replaced the receiver's completed image.
    // Settle this admitted run while keeping its runtime and retained product.
    return [this, failure = stop.stop_requested() ? std::exception_ptr{} : failure] { Settle(nullptr, failure); };
   } catch (...) {
    static_cast<void>(algorithm->Stop());
    throw;
   }
   if (stop.stop_requested()) {
    const auto stopped = algorithm->Stop();
    if (!stopped.ok()) throw std::runtime_error("Live stop failed: " + stopped.message);
    return [this] { Settle(nullptr); };
   }
   request_ = request;
   cadence_ = cadence;
   next_capture_ = {};
   static_cast<void>(worker_.NotifyContinuation());
   return {};
  },
       [this] { Settle(nullptr); })) {
   throw contracts::BusyError("Live is already running");
  }
  state_.running = true;
  run_stop_ = std::move(next_stop);
  state_.cancellation_requested = false;
  state_.revision = *admitted_revision;
  return state_;
 }
 LiveSnapshot Stop() noexcept {
  bool request_stop = false;
  std::stop_source stop{std::nostopstate};
  LiveSnapshot result;
  {
   std::scoped_lock lock(mutex_);
   if (state_.running && !state_.cancellation_requested) {
    state_.cancellation_requested = true;
    if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
    request_stop = true;
    stop = run_stop_;
   }
   result = state_;
  }
  if (request_stop) {
   static_cast<void>(stop.request_stop());
   if (!worker_.RequestActiveStop()) static_cast<void>(worker_.NotifyContinuation());
  }
  return result;
 }
 void Shutdown() noexcept {
  static_cast<void>(Stop());
  {
   std::unique_lock lock(mutex_);
   settled_.wait(lock, [this] { return !state_.running; });
  }
  worker_.StopAndWait();
 }
 bool stopped() const noexcept { return worker_.stopped(); }
 LiveSnapshot snapshot() const {
  std::scoped_lock lock(mutex_);
  return state_;
 }
 void Publish(event_type event) noexcept { publish_visual_event_noexcept(events_, std::move(event)); }
 void PublishFrame() noexcept { Publish(event_type{LiveFrameCompleted{snapshot()}}); }
 void Settle(LiveAlgorithm* algorithm, std::exception_ptr failure = {}) noexcept {
  pending_output_ = {};
  if (algorithm != nullptr) static_cast<void>(algorithm->Stop());
  LiveSnapshot settled;
  {
   std::scoped_lock lock(mutex_);
   if (!state_.running) return;
   state_.running = false;
   state_.cancellation_requested = false;
   if (const auto next = contracts::next_compute_generation(state_.revision)) state_.revision = *next;
   settled = state_;
  }
  settled_.notify_all();
  if (failure) {
   auto detail = visual_failure_detail(failure, "Live operation failed");
   report_visual_worker_failure(diagnostics_, contracts::DiagnosticOwner::Live, settings_.device, detail);
   Publish(event_type{LiveFailed{std::move(settled), std::move(detail)}});
  } else
   Publish(event_type{LiveChanged{std::move(settled)}});
 }
 void AdvanceRevision() {
  const auto next = contracts::next_compute_generation(state_.revision);
  if (!next) throw std::overflow_error("Live observation revision exhausted");
  state_.revision = *next;
 }

private:
 friend class LiveSystem;
 detail::VisualRuntimeOwner::Notification Capture(mmltk::frameworks::gpu::SystemImageRuntime& runtime, const std::stop_token worker_stop) {
  auto* const algorithm = dynamic_cast<LiveAlgorithm*>(runtime.model());
  if (!algorithm) throw std::runtime_error("Live capture data plane is unavailable");
  std::stop_token run_stop;
  std::stop_source stop_source{std::nostopstate};
  {
   std::scoped_lock lock(mutex_);
   if (!state_.running) return {};
   run_stop = run_stop_.get_token();
   stop_source = run_stop_;
  }
  std::stop_callback stopping(worker_stop, [stop_source]() mutable { static_cast<void>(stop_source.request_stop()); });
  if (run_stop.stop_requested()) {
   worker_.SetOutputRetry(false);
   const auto stopped = algorithm->Stop();
   if (!stopped.ok()) throw std::runtime_error("Live stop failed: " + stopped.message);
   return [this] { Settle(nullptr); };
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < next_capture_) {
   worker_.NotifyContinuationAt(next_capture_);
   return {};
  }
  if (!pending_output_.valid()) pending_output_ = runtime.Completed();
  auto candidate = worker_.TryAcquireOutput(runtime, pending_output_);
  if (!candidate.valid()) return {};
  if (!algorithm->AcquireOutput()) return {};
  // Every retained semantic read holds an image slot. Thus this reusable pool
  // grows only with the image slots actually owned by this runtime and readers,
  // plus the unpublished replacement; it never accumulates per-frame products.
  auto available = std::ranges::find_if(analysis_slots_, [](const auto& slot) { return slot.use_count() == 1; });
  if (available == analysis_slots_.end()) {
   analysis_slots_.push_back(std::make_shared<mmltk::backend::media::live::LiveAnalysisProduct>());
   available = std::prev(analysis_slots_.end());
  }
  auto analysis = *available;
  analysis->Reset();
  bool captured = false;
  runtime.PublishRetained(candidate, request_.extent.width, request_.extent.height, [&](const auto target, const auto semantic, const auto stream) {
   captured = algorithm->Capture(target, semantic, stream, *analysis, run_stop);
   if (captured) algorithm->RenderAnalysis(semantic, stream, *analysis);
  });
  if (!captured || run_stop.stop_requested()) {
   static_cast<void>(worker_.NotifyContinuation());
   return {};
  }
  if (!analysis->ready()) throw std::runtime_error("Live capture did not complete its semantic product");
  const auto frame = visual_frame({PresentationSourceKind::Live, 1U}, request_.extent, candidate.revision());
  auto metadata = project_live_metadata(frame, *analysis);
  std::shared_ptr<const VisualDocument> document;
  if (metadata) {
   document = live_document(frame, analysis, *metadata);
   metadata->document = document->facts();
  }
  if (!metadata || !visual_metadata_admitted(*metadata)) {
   // PublishRetained has settled both receiver planes; the selected product has
   // not changed. Stop this run, but retain its healthy receiver runtime.
   worker_.SetOutputRetry(false);
   const auto stopped = algorithm->Stop();
   if (!stopped.ok()) throw std::runtime_error("Live metadata refusal cleanup failed: " + stopped.message);
   const auto failure = std::make_exception_ptr(contracts::InvalidIntentError("Live image metadata exceeds the aggregate publication capacity"));
   return [this, failure] { Settle(nullptr, failure); };
  }
  static_cast<void>(runtime.CommitOutput(std::move(candidate)));
  {
   std::scoped_lock lock(mutex_);
   if (state_.completed_frames == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Live completed-frame counter exhausted");
   ++state_.completed_frames;
   AdvanceRevision();
   state_.frame = frame;
   state_.document = metadata->document;
   committed_metadata_ = std::move(*metadata);
   committed_document_ = std::move(document);
   committed_analysis_ = std::move(analysis);
  }
  next_capture_ = std::chrono::steady_clock::now() + cadence_;
  worker_.NotifyContinuationAt(next_capture_);
  diagnostics_.Emit([&] {
   return VisualDiagnosticFact{
    .system = contracts::DiagnosticOwner::Live, .operation = VisualDiagnosticOperation::FrameCompleted, .device = settings_.device, .generation = runtime.OutputFacts().revision
   };
  });
  return [this] { PublishFrame(); };
 }
 // CLEANUP-IGNORE: This system captures its own locked frame before calling the shared borrow_matching_visual_product; runtime borrowing must occur outside that lock.
 [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const {
  // CLEANUP-IGNORE: Each system snapshots its own locked frame before using the shared matching-product borrow API outside the lock.
  VisualFrame committed;
  {
   std::scoped_lock lock(mutex_);
   if (!state_.frame.valid()) return {};
   committed = state_.frame;
  }
  return borrow_matching_visual_product(committed, worker_);
 }
 std::vector<std::shared_ptr<mmltk::backend::media::live::LiveAnalysisProduct>> analysis_slots_;
 std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct> committed_analysis_;
 std::shared_ptr<const VisualDocument> committed_document_;
 LiveImageMetadata committed_metadata_;
 VisualDeviceSettings settings_;
 SettingsSystem* application_settings_ = nullptr;
 ModelSystem* model_ = nullptr;
 SystemEventSink<event_type> events_;
 VisualDiagnosticSink diagnostics_{};
 mutable std::mutex mutex_;
 LiveSnapshot state_;
 std::condition_variable settled_;
 std::stop_source run_stop_;
 LiveStart request_{};
 std::chrono::microseconds cadence_{};
 std::chrono::steady_clock::time_point next_capture_{};
 mmltk::frameworks::gpu::SystemImageRuntime::CompletedOutput pending_output_;
 detail::VisualRuntimeOwner worker_;
};
LiveSystem::LiveSystem(
 const VisualDeviceSettings settings, VisualRuntimeFactory factory, SystemEventSink<event_type> events, VisualDiagnosticSink diagnostics, SettingsSystem* application_settings, ModelSystem* model)
    : impl_(std::make_unique<Impl>(settings, std::move(factory), std::move(events), diagnostics, application_settings, model)) {}
LiveSystem::~LiveSystem() = default;
LiveSnapshot LiveSystem::Start(const LiveStart request) {
 // CLEANUP-IGNORE: Live facade forwarding preserves its domain-specific Start endpoint.
 return impl_->Start(request);
}
LiveSnapshot LiveSystem::Stop() noexcept {
 // CLEANUP-IGNORE: Live facade forwarding preserves its domain-specific Stop result.
 return impl_->Stop();
}
void LiveSystem::Shutdown() noexcept { impl_->Shutdown(); }
bool LiveSystem::stopped() const noexcept { return impl_->stopped(); }
LiveSnapshot LiveSystem::snapshot() const { return impl_->snapshot(); }
// CLEANUP-IGNORE: Live forwards its sealed source API to its own owner and the existing shared renderer.
std::optional<LiveImageMetadata> LiveSystem::ImageSnapshot(const VisualFrame& frame) const {
 const auto read = ReadAnalysis(frame);
 if (!read.valid()) return {};
 std::scoped_lock lock(impl_->mutex_);
 if (impl_->committed_metadata_.frame != frame) return {};
 return impl_->committed_metadata_;
}
VisualDocumentRead LiveSystem::BorrowDocument(const VisualFrame& frame) const {
 auto read = ReadAnalysis(frame);
 if (!read.valid()) return {};
 std::shared_ptr<const VisualDocument> document;
 std::optional<mmltk::frameworks::serialization::wire::Value> value;
 {
  std::scoped_lock lock(impl_->mutex_);
  if (impl_->committed_metadata_.frame != frame) return {};
  document = impl_->committed_document_;
  auto encoded = mmltk::frameworks::serialization::reflected_transport_value(impl_->committed_metadata_);
  if (!encoded) throw contracts::InvalidIntentError("Live image meaning cannot be encoded");
  value.emplace(std::move(*encoded));
 }
 if (!value || !document) throw contracts::InvalidIntentError("Live image meaning cannot be encoded");
 return {std::move(read.image_), std::move(document), std::make_shared<const mmltk::frameworks::serialization::wire::Value>(std::move(*value))};
}
mmltk::frameworks::gpu::BorrowedImageProductReadView LiveSystem::BorrowFrame() const { return impl_->BorrowFrame(); }
LiveAnalysisRead LiveSystem::ReadAnalysis(const VisualFrame& frame) const {
 std::shared_ptr<const mmltk::backend::media::live::LiveAnalysisProduct> analysis;
 {
  std::scoped_lock lock(impl_->mutex_);
  if (!frame.valid() || impl_->state_.frame != frame || !impl_->committed_analysis_) return {};
  analysis = impl_->committed_analysis_;
 }
 auto image = borrow_matching_visual_product(frame, impl_->worker_);
 if (!image.valid()) return {};
 return {frame, std::move(image), std::move(analysis)};
}
mmltk::frameworks::gpu::BorrowedImageWorkspace LiveSystem::BorrowWorkspace() const { return impl_->worker_.BorrowWorkspace(); }
mmltk::frameworks::gpu::ImageWorkspaceObservation LiveSystem::ObserveWorkspace() const { return impl_->worker_.ObserveWorkspace(); }
void LiveSystem::RequestWorkspace(VisualWorkspaceRequest request) { impl_->worker_.RequestWorkspace(std::move(request)); }
}  // namespace mmltk::controller
namespace mmltk::controller {
void LiveSystem::Input(WorkspaceMouse mouse) {
 std::scoped_lock lock(impl_->mutex_);
 if (impl_->worker_.stopped()) throw contracts::UnavailableError("Live input is unavailable");
 impl_->input_.Accept(std::move(mouse), PresentationSourceKind::Live);
}
void LiveSystem::SetInputPeer(std::uint64_t epoch) {
 std::scoped_lock lock(impl_->mutex_);
 impl_->input_.SetPeer(epoch);
}
}  // namespace mmltk::controller
