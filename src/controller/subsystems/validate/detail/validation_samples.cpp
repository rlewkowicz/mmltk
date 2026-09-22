#include "src/backend/imaging/sampling.h"
#include "src/backend/imaging/raster/class_palette.h"
#include "src/backend/imaging/raster/image_containment.h"
#include "src/frameworks/serialization/reflected_cbor.h"
#include "src/controller/presentation/annotation_palette.h"
#include "validation_samples.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>
#include <limits>
#include <iterator>
#include <utility>
#include <cuda_runtime_api.h>
namespace mmltk::controller::detail {
namespace gpu = mmltk::frameworks::gpu;
namespace rfdetr = mmltk::backend::models::rfdetr;
class ValidationSamples::Impl final {
public:
 struct Sample final {
  ValidationSampleMetadata metadata;
  std::optional<mmltk::backend::imaging::resample::ImageResizeMode> resize_mode{};
  std::uint64_t content_identity = 0U;
  std::uint64_t clean_revision = 0U;
  std::shared_ptr<const PredictionPreviewFrame> raw;
  std::shared_ptr<const VisualDocument> document;
 };
 struct Set final {
  std::array<std::shared_ptr<const Sample>, rfdetr::kValidationSampleCapacity> samples;
  std::uint64_t clean_revision = 0;
  std::uint64_t generation = 0;
  std::uint64_t atlas_identity = 0;
 };
 struct Composition final {
  std::shared_ptr<Set> atlas;
  std::optional<ValidationSampleIdentity> detail;
  ValidationOverlays overlays;
 };
 Impl(VisualDeviceSettings visual, std::function<void()> changed, PredictionPreviewPool::TransferOperations transfers)
     : visual_(visual),
       changed_(std::move(changed)),
       transfers_(transfers),
       worker_(
        [this](auto revisions) {
         EnsurePool();
         gpu::SystemImageRuntimeConfig config{.device = visual_.device,
          .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
          .output_buffer_count = 2U,
          .numa_node = visual_.numa_node,
          .execution = execution_,
          .product_revisions = std::move(revisions),
          .adopted_context = context_};
         configure_visual_workspace_finalization(config);
         return std::make_unique<gpu::SystemImageRuntime>(std::move(config));
        },
        [this](std::exception_ptr) {
         {
          std::scoped_lock lock(mutex_);
          SelectOverlays(image_.overlays);
          if (current_) RestoreIncumbent();
          render_requested_ = false;
         }
         Notify();
        }) {
  if (visual_.valid() && (visual_.maximum_width < 8U || visual_.maximum_height < 9U)) throw contracts::InvalidIntentError("validation requires at least an 8 by 9 image envelope");
  worker_.RegisterContinuation([this](auto& runtime, auto stop) { return Render(runtime, stop); }, {}, true);
 }
 ~Impl() { Shutdown(); }
 void EnsurePool() {
  std::scoped_lock lock(pool_mutex_);
  if (!retirement_->admission_open()) throw contracts::UnavailableError("validation sample custody is unobservable");
  if (pool_) return;
  if (!visual_.valid()) throw contracts::UnavailableError("validation visual device is unavailable");
  execution_ = resolve_visual_device_execution(visual_);
  context_ = CreatePredictionPreviewContext(*execution_, retirement_);
  pool_ = std::make_unique<PredictionPreviewPool>(*execution_, *context_, transfers_, retirement_, kRawCapacity);
 }
 void Notify() noexcept {
  if (changed_) {
   try {
    changed_();
   } catch (...) {}
  }
 }
 void Begin(std::uint64_t generation, std::span<const std::uint32_t> indices) {
  if (indices.size() > rfdetr::kValidationSampleCapacity) throw contracts::InvalidIntentError("validation sample count exceeds six");
  auto next = std::make_shared<Set>();
  next->generation = generation;
  for (std::size_t index = 0; index < indices.size(); ++index) {
   auto sample = std::make_shared<Sample>();
   sample->metadata.identity = {generation, indices[index]};
   next->samples[index] = std::move(sample);
  }
  {
   std::scoped_lock lock(mutex_);
   if (generation == 0U || generation <= generation_frontier_) return;
   if (current_ && !settled_success_)
    RestoreIncumbent();
   else
    current_.reset();
   if (content_frontier_ > std::numeric_limits<std::uint64_t>::max() - (rfdetr::kValidationSampleCapacity + 2U)) throw contracts::FailedError("validation content identities exhausted");
   if (!requested_.atlas) {
    requested_.atlas = std::make_shared<Set>();
    requested_.atlas->atlas_identity = ++content_frontier_;
    requested_.atlas->clean_revision = NextCleanRevision();
    RequestRender();
   }
   rollback_ = requested_;
   next->atlas_identity = ++content_frontier_;
   for (auto& sample : next->samples)
    if (sample) {
     auto identified = std::make_shared<Sample>(*sample);
     identified->content_identity = ++content_frontier_;
     sample = std::move(identified);
    }
   next->clean_revision = NextCleanRevision();
   generation_frontier_ = generation;
   current_ = std::move(next);
   settled_success_ = false;
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 // Caller holds mutex_. Restore source custody, never an old physical output.
 void RestoreIncumbent() {
  if (rollback_.atlas) {
   requested_.atlas = rollback_.atlas;
   requested_.detail = rollback_.detail;
   RequestRender();
  }
  current_.reset();
  rollback_ = {};
 }
 void Settle(std::uint64_t generation, bool succeeded) {
  {
   std::scoped_lock lock(mutex_);
   if (!current_ || current_->generation != generation || settled_success_) return;
   if (!succeeded)
    RestoreIncumbent();
   else {
    // Keep an incumbent detail open. Atlas previews already point
    // at the complete mutable set, whose capture phase ends here.
    if (!requested_.detail && std::ranges::any_of(current_->samples, [](const auto& sample) { return sample && bool(sample->raw); })) {
     requested_.atlas = current_;
     RequestRender();
    }
    if (requested_.atlas == current_)
     settled_success_ = true;
    else {
     current_.reset();
     rollback_ = {};
    }
   }
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 void Capture(std::uint64_t generation, rfdetr::ValidationSampleView sample) {
  {
   std::scoped_lock lock(mutex_);
   if (!current_ || current_->generation != generation || settled_success_) return;
  }
  if (!sample.pixels.preview_failure.empty() || (!sample.pixels.chw && !sample.pixels.rgb8) || sample.pixels.width > visual_.maximum_width || sample.pixels.height > visual_.maximum_height) {
   Settle(generation, false);
   return;
  }
  EnsurePool();
  std::shared_ptr<const PredictionPreviewFrame> raw;
  {
   std::scoped_lock lock(pool_mutex_);
   raw = pool_->Capture(sample.pixels.chw, {sample.pixels.width, sample.pixels.height}, sample.pixels.stream, sample.prediction.detections, sample.annotations, sample.annotations.class_catalog,
    static_cast<int>(sample.annotations.class_catalog->size()), sample.pixels.rgb8, std::move(sample.pixels.custody), sample.pixels.stop_source, sample.pixels.source_control, sample.ground_truth,
    true);
  }
  if (!raw) {
   Settle(generation, false);
   return;
  }
  ValidationSampleMetadata metadata;
  metadata.available = true;
  metadata.pixel_extent = {sample.pixels.width, sample.pixels.height};
  metadata.source_extent = {sample.source_width, sample.source_height};
  metadata.content = {sample.content.offset_x, sample.content.offset_y, sample.content.resized_width, sample.content.resized_height};
  if (!metadata.content.valid()) metadata.content = {0U, 0U, sample.pixels.width, sample.pixels.height};
  const auto palette = mmltk::controller::annotation_class_palette(raw->classes().size());
  const auto labels = [&](std::span<const rfdetr::Prediction> predictions, bool ground_truth) {
   for (const auto& prediction : predictions) {
    const auto category = static_cast<std::size_t>(prediction.class_reference);
    if (category >= raw->classes().size()) throw std::invalid_argument("validation sample category is absent");
    std::array<std::uint8_t, 3> rgb{};
    mmltk::backend::imaging::raster::color::class_color(static_cast<int>(category), static_cast<int>(raw->classes().size()), rgb[0], rgb[1], rgb[2]);
    if (ground_truth)
     for (auto& channel : rgb) channel = 255U - channel;
    metadata.labels.push_back({{{prediction.bbox_xyxy[0], prediction.bbox_xyxy[1]}, {prediction.bbox_xyxy[2], prediction.bbox_xyxy[3]}}, palette[category], rgb, static_cast<std::uint32_t>(category),
     ground_truth, prediction.score, raw->classes()[category]});
   }
  };
  labels(raw->predictions(), false);
  labels(raw->ground_truth(), true);
  {
   std::scoped_lock lock(mutex_);
   if (!current_ || current_->generation != generation || settled_success_) return;
   auto found = std::ranges::find_if(
    current_->samples, [&](const auto& slot) { return slot && slot->metadata.identity.generation != 0U && slot->metadata.identity.dataset_index == sample.prediction.dataset_index; });
   if (found == current_->samples.end()) throw std::logic_error("validation captured an unselected sample");
   if ((*found)->raw) return;  // A delivery identity owns one immutable sample.
   metadata.identity = (*found)->metadata.identity;
   auto document = std::make_shared<VisualDocument>();
   document->scene.document = contracts::WorkspaceResource::From("validation://sample", (*found)->content_identity);
   document->scene.frame_width = static_cast<std::uint16_t>(metadata.pixel_extent.width);
   document->scene.frame_height = static_cast<std::uint16_t>(metadata.pixel_extent.height);
   document->scene.frame_ready = true;
   document->scene.frame_index = metadata.identity.dataset_index;
   document->scene.palette = palette;
   for (const auto& name : raw->classes()) document->scene.categories.push_back({.value = name});
   for (const auto& gt : raw->ground_truth()) {
    contracts::AnnotationObject object;
    object.name = contracts::AnnotationText::From("object " + std::to_string(document->scene.objects.size() + 1U));
    object.category = static_cast<std::uint16_t>(gt.class_reference);
    object.box = {{gt.bbox_xyxy[0], gt.bbox_xyxy[1]}, {gt.bbox_xyxy[2], gt.bbox_xyxy[3]}};
    object.mask.present = gt.has_mask;
    document->mask_bounds.push_back(gt.has_mask ? mmltk::backend::imaging::sampling::rle_support_bounds(std::span{gt.mask.runs}, gt.mask.width, gt.mask.height) : std::array<float, 4>{});
    object.shape = object.mask.present ? contracts::AnnotationShape::Mask : contracts::AnnotationShape::Box;
    document->scene.objects.push_back(std::move(object));
   }
   auto masks = std::make_shared<std::vector<rfdetr::EncodedMask>>();
   masks->reserve(raw->ground_truth().size());
   for (const auto& gt : raw->ground_truth()) masks->push_back(gt.mask);
   document->mask_contains = [masks](std::size_t index, float x, float y) {
    if (index >= masks->size()) return false;
    const auto& mask = (*masks)[index];
    if (!(x >= 0.0F && x < 1.0F && y >= 0.0F && y < 1.0F)) return false;
    const auto offset = static_cast<std::uint32_t>(y * static_cast<float>(mask.height)) * mask.width + static_cast<std::uint32_t>(x * static_cast<float>(mask.width));
    const auto after = std::ranges::upper_bound(mask.runs, offset, {}, &std::pair<std::uint32_t, std::uint32_t>::first);
    if (after == mask.runs.begin()) return false;
    const auto& run = *std::prev(after);
    return offset - run.first < run.second;
   };
   auto captured = std::make_shared<Sample>();
   captured->metadata = std::move(metadata);
   captured->resize_mode = sample.resize_mode;
   captured->content_identity = (*found)->content_identity;
   captured->clean_revision = NextCleanRevision();
   captured->document = std::move(document);
   captured->raw = std::move(raw);
   *found = std::move(captured);
   current_->clean_revision = NextCleanRevision();
   if (!requested_.detail) {
    requested_.atlas = current_;
    RequestRender();
   }
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 std::uint64_t NextCleanRevision() {
  if (clean_frontier_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("validation clean revisions exhausted");
  return ++clean_frontier_;
 }
 void RequestRender() {
  if (request_revision_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("validation composition revision exhausted");
  ++request_revision_;
  dirty_ = render_requested_ = true;
  retry_remaining_ = 1U;
 }
 void Select(ValidationSampleIdentity identity) {
  {
   std::scoped_lock lock(mutex_);
   if (!displayed_) throw contracts::InvalidIntentError("validation sample is not displayed");
   const auto found = std::ranges::find_if(displayed_->samples, [&](const auto& sample) { return sample && sample->raw && sample->metadata.identity == identity; });
   if (found == displayed_->samples.end()) throw contracts::InvalidIntentError("validation sample identity is stale");
   requested_.atlas = displayed_;
   requested_.detail = identity;
   if (rollback_.atlas && rollback_.atlas->generation == identity.generation) rollback_.detail = identity;
   RequestRender();
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 void CloseDetail() {
  {
   std::scoped_lock lock(mutex_);
   // Keep the atlas retained by a pending/committed detail selection.
   // Begin and partial captures never substitute their current set.
   if (!requested_.detail) requested_.atlas = displayed_;
   requested_.detail.reset();
   rollback_.detail.reset();
   if (!requested_.atlas) return;
   RequestRender();
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 void SelectOverlays(ValidationOverlays overlays) {
  if (requested_.overlays == overlays) return;
  if (overlay_revision_ == std::numeric_limits<std::uint64_t>::max()) throw contracts::FailedError("validation overlay revision exhausted");
  ++overlay_revision_;
  requested_.overlays = overlays;
 }
 void SetOverlays(ValidationOverlays overlays) {
  {
   std::scoped_lock lock(mutex_);
   if (!dirty_ && image_.overlays == overlays) return;
   SelectOverlays(overlays);
   if (displayed_) {
    requested_.atlas = displayed_;
    requested_.detail.reset();
    if (image_.detail) { requested_.detail = image_.selected; }
   }
   if (requested_.atlas) RequestRender();
  }
  static_cast<void>(worker_.NotifyContinuation());
 }
 VisualRuntimeOwner::Notification Render(gpu::SystemImageRuntime& runtime, std::stop_token stop) {
  if (stop.stop_requested()) return {};
  std::uint64_t attempt = 0U;
  try {
   std::shared_ptr<Set> set;
   std::optional<ValidationSampleIdentity> selected;
   ValidationOverlays overlays;
   {
    std::scoped_lock lock(mutex_);
    if (!dirty_ || !render_requested_) return {};
   }
   gpu::SystemImageRuntime::CompletedOutput baseline;
   auto candidate = worker_.TryAcquireOutput(runtime, baseline);
   if (!candidate.valid()) return {};
   // Copy only six immutable sample handles/metadata under the owner lock.
   auto drawing = std::make_shared<Set>();
   {
    std::scoped_lock lock(mutex_);
    set = requested_.atlas;
    if (!set || !render_requested_) return {};
    selected = requested_.detail;
    overlays = requested_.overlays;
    *drawing = *set;
    attempt = request_revision_;
    render_requested_ = false;
   }
   const auto unit = std::min({64U, visual_.maximum_width / 8U, visual_.maximum_height / 9U});
   const auto cell_width = unit * 4U, cell_height = unit * 3U;
   VisualExtent extent{cell_width * 2U, cell_height * 3U};
   ValidationImageMetadata image;
   image.content_identity = drawing->atlas_identity;
   image.detail = selected.has_value();
   image.selected = selected;
   image.overlays = overlays;
   auto clean_revision = drawing->clean_revision;
   if (selected) {
    const auto found = std::ranges::find_if(drawing->samples, [&](const auto& slot) { return slot && slot->raw && slot->metadata.identity == *selected; });
    if (found == drawing->samples.end()) return {};
    extent = (*found)->metadata.pixel_extent;
    image.content_identity = (*found)->content_identity;
    image.document = (*found)->document->facts();
    clean_revision = (*found)->clean_revision;
   }
   std::array<PredictionPreviewComposition::Region, rfdetr::kValidationSampleCapacity> regions;
   std::size_t region_count = 0U;
   for (std::size_t index = 0; index < drawing->samples.size(); ++index) {
    const auto& sample = drawing->samples[index];
    if (!sample) continue;
    auto& metadata = image.samples[index];
    metadata = sample->metadata;
    if (!sample->raw || (selected && sample->metadata.identity != *selected)) continue;
    const auto contained = mmltk::backend::imaging::raster::contain_image(metadata.pixel_extent.width, metadata.pixel_extent.height, cell_width, cell_height);
    metadata.crop =
     selected ? VisualRegion{0U, 0U, extent.width, extent.height}
              : VisualRegion{static_cast<std::uint32_t>(index % 2U) * cell_width + contained.x, static_cast<std::uint32_t>(index / 2U) * cell_height + contained.y, contained.width, contained.height};
    regions[region_count++] = {sample->raw, metadata.crop};
   }
   PredictionPreviewComposition::Draw(runtime, candidate, extent, std::span(regions).first(region_count),
    {overlays.prediction_layer && overlays.prediction_boxes, overlays.prediction_layer && overlays.prediction_masks, overlays.ground_truth_layer && overlays.ground_truth_boxes,
     overlays.ground_truth_layer && overlays.ground_truth_masks, true, !selected},
    &preparation_);
   image.frame = visual_frame({PresentationSourceKind::Validation, 1U}, extent, candidate.revision());
   image.frame.content = {0U, 0U, extent.width, extent.height};
   image.frame.clean_revision = clean_revision;
   if (selected) {
    const auto found = std::ranges::find_if(drawing->samples, [&](const auto& slot) { return slot && slot->metadata.identity == *selected; });
    image.frame.content = (*found)->metadata.content;
    image.frame.source_extent = (*found)->metadata.source_extent;
    image.frame.resize_mode = (*found)->resize_mode;
   }
   {
    std::scoped_lock lock(mutex_);
    if (attempt != request_revision_) return {};
   }
   static_cast<void>(runtime.CommitOutput(std::move(candidate)));
   return [this, attempt, drawing = std::move(drawing), image = std::move(image)]() mutable {
    {
     std::scoped_lock lock(mutex_);
     if (attempt != request_revision_) return;
     displayed_ = std::move(drawing);
     image_ = std::move(image);
     dirty_ = false;
     if (current_ && settled_success_ && displayed_->generation == current_->generation) {
      current_.reset();
      rollback_ = {};
     }
    }
    Notify();
   };
  } catch (...) {
   const auto failure = std::current_exception();
   const auto context_failure = gpu::find_image_failure<gpu::CudaContextFailure>(failure);
   bool terminal_context = false;
   if (context_failure) {
    try {
     std::rethrow_exception(context_failure);
    } catch (const gpu::CudaContextFailure& error) { terminal_context = error.terminal(); }
   }
   if (terminal_context || !retirement_->admission_open() || gpu::is_image_execution_failure(failure)) {
    static_cast<void>(runtime.Retire(failure));
    throw;
   }
   // Proved ordinary draw failure leaves candidate rollback and the last
   // selected image with its original metadata in the healthy runtime.
   bool retry = false;
   {
    std::scoped_lock lock(mutex_);
    if ((attempt == 0U || attempt == request_revision_) && dirty_) {
     retry = retry_remaining_ != 0U;
     retry_remaining_ = 0U;
     render_requested_ = retry;
     if (!retry) {
      SelectOverlays(image_.overlays);
      // A refused preview is not a successful replacement.
      if (current_ && requested_.atlas && requested_.atlas->generation == current_->generation) {
       RestoreIncumbent();
       retry = true;
      }
     }
    }
   }
   // One automatic retry per request. Refused overlay selection returns
   // to applied state; refused progressive capture schedules rollback.
   return [this, retry] {
    if (retry) static_cast<void>(worker_.NotifyContinuation());
    Notify();
   };
  }
 }
 void Shutdown() noexcept {
  worker_.StopAndWait();
  {
   std::scoped_lock lock(mutex_);
   current_.reset();
   displayed_.reset();
   requested_.atlas.reset();
   rollback_ = {};
  }
  {
   std::scoped_lock lock(pool_mutex_);
   pool_.reset();
   context_.reset();
  }
  worker_.FinishStoppedRetirement();
 }
 mutable std::mutex mutex_, pool_mutex_;
 VisualDeviceSettings visual_;
 std::function<void()> changed_;
 PredictionPreviewPool::TransferOperations transfers_;
 std::optional<gpu::DeviceExecution> execution_;
 std::optional<gpu::DeviceContext> context_;
 // Incumbent rollback, current capture and an in-flight/displayed generation
 // share immutable raw handles. Admission stays bounded by the original pool;
 // rollback retains no completed output slot and republishes a new revision.
 // Each raw slot reserves its own lease; composition and context construction
 // each reserve one additional exact aggregate before doing device work.
 static_assert(rfdetr::kValidationSampleCapacity == 3U * 2U);
 static constexpr std::size_t kRawCapacity = 3U * rfdetr::kValidationSampleCapacity;
 std::shared_ptr<gpu::TerminalCudaRetirementOwner> retirement_ = std::make_shared<gpu::TerminalCudaRetirementOwner>(kRawCapacity + 2U);
 std::unique_ptr<PredictionPreviewPool> pool_;
 std::shared_ptr<Set> current_, displayed_;
 Composition requested_, rollback_;
 ValidationImageMetadata image_;
 std::uint64_t overlay_revision_ = 0U;
 std::uint64_t generation_frontier_ = 0U;
 std::uint64_t content_frontier_ = 0U, clean_frontier_ = 0U;
 PredictionPreviewComposition preparation_;
 std::uint64_t request_revision_ = 0U;
 unsigned retry_remaining_ = 0U;
 bool dirty_ = false, render_requested_ = false, settled_success_ = false;
 VisualRuntimeOwner worker_;
};
ValidationSamples::ValidationSamples(VisualDeviceSettings visual, std::function<void()> changed, PredictionPreviewPool::TransferOperations transfers)
    : impl_(std::make_unique<Impl>(visual, std::move(changed), transfers)) {}
ValidationSamples::~ValidationSamples() = default;
void ValidationSamples::Begin(std::uint64_t generation, std::span<const std::uint32_t> indices) { impl_->Begin(generation, indices); }
void ValidationSamples::Capture(std::uint64_t generation, rfdetr::ValidationSampleView sample) {
 try {
  impl_->Capture(generation, std::move(sample));
 } catch (...) {
  impl_->Settle(generation, false);
  throw;
 }
}
void ValidationSamples::Settle(std::uint64_t generation, bool succeeded) { impl_->Settle(generation, succeeded); }
void ValidationSamples::Select(ValidationSampleIdentity identity) { impl_->Select(identity); }
void ValidationSamples::CloseDetail() { impl_->CloseDetail(); }
void ValidationSamples::SetOverlays(ValidationOverlays overlays) { impl_->SetOverlays(overlays); }
ValidationSnapshot ValidationSamples::snapshot() const {
 std::scoped_lock lock(impl_->mutex_);
 ValidationSnapshot result;
 result.frame = impl_->image_.frame;
 result.content_identity = impl_->image_.content_identity;
 result.detail = impl_->image_.detail;
 result.selected = impl_->image_.selected;
 result.document = impl_->image_.document;
 result.overlays = impl_->image_.overlays;
 result.overlay_selection = {impl_->overlay_revision_, impl_->requested_.overlays};
 for (std::size_t index = 0; index < impl_->image_.samples.size(); ++index) {
  if (impl_->displayed_ && impl_->displayed_->samples[index]) {
   result.sample_identities[index] = impl_->displayed_->samples[index]->metadata.identity;
   result.sample_available[index] = impl_->displayed_->samples[index]->metadata.available;
  }
 }
 return result;
}
std::optional<ValidationImageMetadata> ValidationSamples::ImageSnapshot(const VisualFrame& frame) const {
 std::scoped_lock lock(impl_->mutex_);
 return impl_->image_.frame == frame ? std::optional{impl_->image_} : std::nullopt;
}
gpu::BorrowedImageProductReadView ValidationSamples::BorrowFrame() const {
 const auto frame = snapshot().frame;
 return borrow_matching_visual_product(frame, impl_->worker_);
}
VisualDocumentRead ValidationSamples::BorrowDocument(const VisualFrame& frame) const {
 std::shared_ptr<const VisualDocument> document;
 ValidationImageMetadata metadata;
 {
  std::scoped_lock lock(impl_->mutex_);
  if (!impl_->image_.detail || impl_->image_.frame != frame || !impl_->displayed_) return {};
  metadata = impl_->image_;
  for (const auto& sample : impl_->displayed_->samples)
   if (sample && sample->document && sample->document->facts() == metadata.document) document = sample->document;
 }
 if (!document) return {};
 auto encoded = mmltk::frameworks::serialization::reflected_transport_value(metadata);
 if (!encoded) throw std::runtime_error("validation document metadata cannot be projected");
 return {borrow_matching_visual_product(frame, impl_->worker_), std::move(document), std::make_shared<const mmltk::frameworks::serialization::wire::Value>(std::move(*encoded))};
}
gpu::BorrowedImageWorkspace ValidationSamples::BorrowWorkspace() const { return impl_->worker_.BorrowWorkspace(); }
gpu::ImageWorkspaceObservation ValidationSamples::ObserveWorkspace() const { return impl_->worker_.ObserveWorkspace(); }
void ValidationSamples::RequestWorkspace(VisualWorkspaceRequest request) { impl_->worker_.RequestWorkspace(std::move(request)); }
void ValidationSamples::Shutdown() noexcept { impl_->Shutdown(); }
}  // namespace mmltk::controller::detail
