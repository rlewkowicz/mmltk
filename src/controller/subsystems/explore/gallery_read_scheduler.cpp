#include "src/controller/subsystems/explore/detail/gallery_read_scheduler.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/backend/models/rfdetr/augmentation/sampling.h"
#include "src/backend/imaging/resample/image_resize.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>
import mmltk.backend.imaging.explore.explore_render_core;
namespace mmltk::controller::explore_detail {
namespace data = mmltk::backend::data;
namespace explore = mmltk::backend::imaging::explore;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace {
[[nodiscard]] float normalized(const float coordinate, const std::uint32_t extent) noexcept {
 return extent == 0U ? 0.0F : std::clamp(static_cast<float>(coordinate) / static_cast<float>(extent), 0.0F, 1.0F);
}
void pack_rle_mask(const std::span<const data::RLEPair> runs, const std::span<std::uint64_t> words, const std::size_t pixel_count) noexcept {
 std::ranges::fill(words, 0U);
 for (const auto run : runs) {
  const std::size_t begin = std::min<std::size_t>(run.start, pixel_count);
  const std::size_t end = std::min<std::size_t>(begin + run.length, pixel_count);
  for (std::size_t word = begin / 64U; word <= (end == 0U ? 0U : (end - 1U) / 64U) && begin < end; ++word) {
   const auto word_begin = word * 64U;
   const auto low = std::max(begin, word_begin) - word_begin;
   const auto high = std::min(end, word_begin + 64U) - word_begin;
   const auto below_high = high == 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << high) - 1U;
   const auto below_low = low == 0U ? 0U : (std::uint64_t{1} << low) - 1U;
   words[word] |= below_high & ~below_low;
  }
 }
}
}  // namespace
GalleryReadScheduler::GalleryReadScheduler(std::size_t nproc, const mmltk::frameworks::gpu::DeviceExecution& execution, const ExploreNativeConfiguration& configuration)
    : acceptance_(configuration.acceptance),
      diagnostics_(configuration.diagnostics),
      device_(execution.device),
      image_stream_({.slots = nproc + 1U, .workers = nproc, .device = execution.device, .loading = configuration.loading, .execution = execution}) {
 if (image_stream_.workers().size() != nproc) throw contracts::InvalidIntentError("Explore nproc exceeds the current Linux CPU affinity");
 lanes_.reserve(nproc);
 for (std::size_t index = 0U; index != nproc; ++index) lanes_.push_back(std::make_unique<Lane>(index, image_stream_.metadata_storage(index)));
 detail_lane_ = std::make_unique<Lane>(nproc, image_stream_.metadata_storage(nproc));
}
bool GalleryReadScheduler::BeginReadLane(const std::size_t lane_index) {
 Lane& lane = LaneAt(lane_index);
 std::uint64_t demand = 0U;
 {
  std::scoped_lock lock(lanes_mutex_);
  demand = lane.DemandGeneration();
  if (lane.state != LaneState::Queued || !Current(demand)) return false;
  lane.state = LaneState::Reading;
  if (diagnostics_.valid()) diagnostics_.Emit([&] { return LaneDiagnostic(VisualDiagnosticOperation::GalleryReadStarted, lane); });
 }
 if (acceptance_ && !lane.prefetch) {
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceLaneStarted, lane, lane.compiled_index);
  if (acceptance_->AwaitInitialRelease(demand) == ExploreAcceptanceGate::WaitResult::Stale) {
   if (acceptance_->ClaimTerminalReport()) AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceGateTerminal, lane, lane.compiled_index);
   return false;
  }
 }
 if (acceptance_) acceptance_->ObserveRead(demand, lane.compiled_index);
 {
  std::scoped_lock lock(lanes_mutex_);
  if (!Current(lane.DemandGeneration())) return false;
 }
 AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompiledRead, lane, lane.compiled_index);
 return true;
}
void GalleryReadScheduler::FinishReadLane(const std::size_t lane_index, std::exception_ptr failure, const bool read) noexcept {
 Lane& lane = LaneAt(lane_index);
 std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
 if (diagnostics_.valid()) {
  std::scoped_lock lock(lanes_mutex_);
  diagnostics_.Emit([&] {
   auto fact = LaneDiagnostic(VisualDiagnosticOperation::GalleryReadCompleted, lane);
   fact.context.capacity_width = read ? 1U : 0U;
   fact.context.capacity_height = failure ? 1U : 0U;
   return fact;
  });
 }
 try {
  if (failure) std::rethrow_exception(failure);
  if (!read) {
   {
    std::scoped_lock lock(lanes_mutex_);
    lane.state = lane.transfer_ready ? LaneState::StaleReady : LaneState::AwaitingTransfer;
    sink = ready_sink_;
   }
   if (sink) (*sink)();
   return;
  }
  ReadLanePayload(lane);
  {
   std::scoped_lock lock(lanes_mutex_);
   lane.read_valid = true;
  }
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompiledReadCompleted, lane, lane.compiled_index);
  if (acceptance_ && !lane.prefetch && lane.first_row != 0U && acceptance_->ClaimHeldCompletion()) {
   AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompletionHeld, lane, lane.compiled_index);
   const auto released = acceptance_->AwaitHeldCompletion(lane.DemandGeneration(), lane.destination_slot, lane.compiled_index, lane.pinned.capacity_bytes());
   if (released == ExploreAcceptanceGate::WaitResult::Proceed)
    AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceCompletionReleased, lane, lane.compiled_index);
   else if (acceptance_->ClaimTerminalReport())
    AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceGateTerminal, lane, lane.compiled_index);
   if (released == ExploreAcceptanceGate::WaitResult::Stale) {
    {
     std::scoped_lock lock(lanes_mutex_);
     lane.state = lane.transfer_ready ? LaneState::StaleReady : LaneState::AwaitingTransfer;
     sink = ready_sink_;
    }
    if (sink) (*sink)();
    return;
   }
  }
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadyStateStarted, lane, lane.compiled_index);
  {
   std::scoped_lock lock(lanes_mutex_);
   lane.read_valid = true;
   PublishInput(lane);
   sink = ready_sink_;
  }
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadyStateCompleted, lane, lane.compiled_index);
 } catch (...) {
  {
   std::scoped_lock lock(lanes_mutex_);
   lane.failure = std::current_exception();
   lane.state = !lane.transfer_ready ? LaneState::AwaitingTransfer : Current(lane.DemandGeneration()) ? LaneState::Failed : LaneState::StaleReady;
   sink = ready_sink_;
  }
 }
 if (sink) {
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadySinkStarted, lane, lane.compiled_index);
  (*sink)();
  AcceptanceDiagnostic(VisualDiagnosticOperation::AcceptanceReadySinkCompleted, lane, lane.compiled_index);
 }
}
void GalleryReadScheduler::PublishInput(Lane& lane) {
 if (!lane.transfer_ready)
  lane.state = LaneState::AwaitingTransfer;
 else if (!Current(lane.DemandGeneration()))
  lane.state = LaneState::StaleReady;
 else
  lane.state = lane.failure ? LaneState::Failed : lane.read_valid ? LaneState::InputReady : LaneState::StaleReady;
 if (lane.prefetch && lane.state == LaneState::InputReady && diagnostics_.valid())
  diagnostics_.Emit([&] {
   return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
    .operation = VisualDiagnosticOperation::ExplorePrefetchReady,
    .generation = lane.generation,
    .value = lane.compiled_index,
    .detail = lanes_.size(),
    .context = {.staging_bytes = lane.pinned.capacity_bytes() + image_stream_.host_storage(lane.index).capacity_bytes()}};
  });
}
void GalleryReadScheduler::FinishTransfer(std::size_t index, std::exception_ptr failure) noexcept {
 std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
 {
  std::scoped_lock lock(lanes_mutex_);
  auto& lane = LaneAt(index);
  lane.transfer_ready = true;
  if (failure) lane.failure = failure;
  if (diagnostics_.valid())
   diagnostics_.Emit([&] {
    auto fact = LaneDiagnostic(VisualDiagnosticOperation::GalleryTransferCompleted, lane);
    fact.context.capacity_height = failure ? 1U : 0U;
    return fact;
   });
  if (lane.state == LaneState::AwaitingTransfer) {
   PublishInput(lane);
   sink = ready_sink_;
  }
 }
 if (sink) (*sink)();
}
void GalleryReadScheduler::AcceptanceDiagnostic(const VisualDiagnosticOperation operation, const Lane& lane, const std::uint64_t detail) const noexcept {
 if (!acceptance_ || !diagnostics_.valid()) return;
 std::scoped_lock lock(lanes_mutex_);
 if (lane.prefetch) return;
 diagnostics_.Emit([&] {
  return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
   .operation = operation,
   .generation = lane.generation,
   .value = lane.destination_slot,
   .detail = detail,
   .context = {.staging_bytes = lane.pinned.capacity_bytes()}};
 });
}
void GalleryReadScheduler::ReadLanePayload(Lane& lane) {
 const auto labels = lane.store->image_labels(lane.compiled_index);
 std::size_t rle_cursor = 0U;
 for (std::size_t label_index = 0U; label_index != labels.size(); ++label_index) {
  const auto& label = labels[label_index];
  const auto runs = lane.store->instance_rle(label);
  store_payload(lane.pinned.data(), lane.layout.rle.offset + rle_cursor * sizeof(data::RLEPair), runs);
  const explore::ExploreRenderAnnotationDescriptor annotation{
   .mask_present = label.has_mask(),
   .rle_offset = static_cast<std::uint32_t>(rle_cursor),
   .rle_count = static_cast<std::uint32_t>(runs.size()),
   .card_index = lane.destination_slot,
   .class_id = label.class_id,
  };
  store_payload(lane.pinned.data(), lane.layout.annotations.offset + label_index * sizeof(explore::ExploreRenderAnnotationDescriptor), annotation);
  rle_cursor += runs.size();
 }
 const auto& entry = lane.store->image_entry(lane.compiled_index);
 const bool source_known = entry.original_width != 0U && entry.original_height != 0U;
 const auto content =
  source_known ? lane.store->geometry(lane.compiled_index) : mmltk::backend::imaging::resample::ImageResizeGeometry{lane.store->header().image_width, lane.store->header().image_height, 0U, 0U};
 const auto image = explore::make_explore_contain_rect(
  source_known ? entry.original_width : lane.store->header().image_width, source_known ? entry.original_height : lane.store->header().image_height, lane.card_extent);
 const explore::ExploreRenderCardDescriptor card{
  .source_width = lane.store->header().image_width,
  .source_height = lane.store->header().image_height,
  .crop_x = content.offset_x,
  .crop_y = content.offset_y,
  .crop_width = content.resized_width,
  .crop_height = content.resized_height,
  .image_x = image.x,
  .image_y = image.y,
  .image_width = image.width,
  .image_height = image.height,
  .annotation_count = static_cast<std::uint32_t>(labels.size()),
  .compiled_index = lane.compiled_index,
 };
 store_payload(lane.pinned.data(), lane.layout.card, card);
 if (lane.donor_instance) {
  store_payload(lane.pinned.data(), lane.layout.donor_instance, *lane.donor_instance);
  const std::array donor_box{
   normalized(lane.donor_instance->bbox_x1, lane.store->header().image_width),
   normalized(lane.donor_instance->bbox_y1, lane.store->header().image_height),
   normalized(lane.donor_instance->bbox_x2, lane.store->header().image_width),
   normalized(lane.donor_instance->bbox_y2, lane.store->header().image_height),
  };
  store_payload(lane.pinned.data(), lane.layout.donor_box, std::span{donor_box});
  const auto donor_runs = lane.store->instance_rle(*lane.donor_instance);
  store_payload(lane.pinned.data(), lane.layout.donor_rle.offset, donor_runs);
  auto* const donor_mask = reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(lane.pinned.data()) + lane.layout.donor_mask);
  pack_rle_mask(donor_runs, std::span{donor_mask, lane.layout.donor_mask_words}, static_cast<std::size_t>(lane.store->header().image_width) * lane.store->header().image_height);
 }
}
void GalleryReadScheduler::CompleteLane(const std::size_t lane_index, std::exception_ptr failure) noexcept {
 std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
 VisualDiagnosticFact fact;
 const bool observed = diagnostics_.valid();
 {
  std::scoped_lock lock(lanes_mutex_);
  Lane& lane = LaneAt(lane_index);
  if (failure) {
   lane.failure = failure;
   lane.state = LaneState::Failed;
  } else if (lane.state == LaneState::GpuPending) {
   lane.state = LaneState::GpuComplete;
  }
  if (observed)
   fact = {.system = contracts::DiagnosticOwner::Explore,
    .operation = failure ? VisualDiagnosticOperation::GalleryGpuFailed : VisualDiagnosticOperation::GalleryGpuCompleted,
    .device = device_,
    .generation = lane.generation,
    .value = lane.destination_slot,
    .detail = lane.compiled_index,
    .context = {.demand = {.demand_generation = lane.DemandGeneration()}, .transfer = {.transfer_sequence = lane.tile_generation}, .link = lane.diagnostic_link}};
  sink = ready_sink_;
 }
 if (observed) diagnostics_(fact);
 if (sink) (*sink)();
}
data::CompiledImageStream::CompletionObserver GalleryReadScheduler::LaneCompletion() noexcept {
 return {.context = this, .complete = [](void* context, std::size_t index, std::exception_ptr failure) noexcept { static_cast<GalleryReadScheduler*>(context)->CompleteLane(index, failure); }};
}
void GalleryReadScheduler::SubmitRead(Lane& lane, const bool observed) {
 const std::array reads{data::CompiledImageRead{lane.compiled_index, 0U}, data::CompiledImageRead{lane.donor_index, lane.layout.pixel_bytes}};
 data::CompiledImageStream::ReadObserver observer;
 if (observed)
  observer = {.context = this,
   .before = [](void* context, std::size_t index) { return static_cast<GalleryReadScheduler*>(context)->BeginReadLane(index); },
   .complete = [](void* context, std::size_t index, std::exception_ptr failure, bool read) noexcept { static_cast<GalleryReadScheduler*>(context)->FinishReadLane(index, failure, read); }};
 else
  observer = {.context = this, .before = [](void* context, std::size_t index) {
               auto& owner = *static_cast<GalleryReadScheduler*>(context);
               const auto& candidate_lane = owner.LaneAt(index);
               if (!owner.current_demand_(candidate_lane.DemandGeneration())) return false;
               owner.diagnostics_.Emit([&] {
                return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                 .operation = VisualDiagnosticOperation::GalleryReadStarted,
                 .device = owner.device_,
                 .generation = candidate_lane.generation,
                 .detail = candidate_lane.compiled_index};
               });
               return true;
              }};
 image_stream_.submit(lane.index, *lane.store, std::span{reads}.first(lane.donor_instance ? 2U : 1U), observer,
  {.context = this, .complete = [](void* context, std::size_t index, std::exception_ptr error) noexcept { static_cast<GalleryReadScheduler*>(context)->FinishTransfer(index, error); }});
}
GalleryReadScheduler::PayloadLayout GalleryReadScheduler::LayoutFor(
 const GalleryProductState& product, const std::uint32_t compiled_index, const bool has_donor, const std::size_t donor_rle_count) const {
 const auto& header = product.store->header();
 PayloadLayout result;
 constexpr std::size_t channels_bytes = 3U * sizeof(float);
 if (header.image_height != 0U && static_cast<std::size_t>(header.image_width) > std::numeric_limits<std::size_t>::max() / header.image_height)
  throw std::overflow_error("Explore thumbnail pixel count exceeds addressable storage");
 const auto pixels = static_cast<std::size_t>(header.image_width) * header.image_height;
 if (pixels > std::numeric_limits<std::size_t>::max() / channels_bytes) throw std::overflow_error("Explore thumbnail payload exceeds addressable storage");
 result.pixel_bytes = pixels * channels_bytes;
 const auto labels = product.store->image_labels(compiled_index);
 result.annotations.count = labels.size();
 if (result.annotations.count > explore::kExploreRenderAnnotationCapacity) throw contracts::BusyError("Explore thumbnail annotations exceed renderer capacity");
 for (const auto& label : labels) {
  const auto run_count = product.store->instance_rle(label).size();
  if (run_count > explore::kExploreRenderRleCapacity - result.rle.count) throw contracts::BusyError("Explore thumbnail annotations exceed renderer capacity");
  result.rle.count += run_count;
 }
 result.card = 0U;
 result.annotations.offset = align_up(result.card + sizeof(explore::ExploreRenderCardDescriptor), alignof(explore::ExploreRenderAnnotationDescriptor));
 result.rle.offset = align_up(result.annotations.offset + result.annotations.count * sizeof(explore::ExploreRenderAnnotationDescriptor), alignof(data::RLEPair));
 result.tile = align_up(result.rle.offset + result.rle.count * sizeof(data::RLEPair), alignof(explore::ExploreRenderTileDescriptor));
 result.donor_rle.count = donor_rle_count;
 result.donor_mask_words = has_donor ? (pixels + 63U) / 64U : 0U;
 result.donor_pixels = align_up(result.tile + sizeof(explore::ExploreRenderTileDescriptor), alignof(float));
 result.donor_instance = align_up(result.donor_pixels, alignof(data::PackedInstance));
 result.donor_box = align_up(result.donor_instance + (has_donor ? sizeof(data::PackedInstance) : 0U), alignof(float));
 result.donor_rle.offset = align_up(result.donor_box + (has_donor ? 4U * sizeof(float) : 0U), alignof(data::RLEPair));
 result.donor_mask = align_up(result.donor_rle.offset + donor_rle_count * sizeof(data::RLEPair), alignof(std::uint64_t));
 result.bytes = result.donor_mask + result.donor_mask_words * sizeof(std::uint64_t);
 return result;
}
void GalleryReadScheduler::PrepareLaneStorage(const GalleryProductState& product, Lane& lane, const std::uint32_t compiled_index, const std::uint32_t slot, const std::uint64_t generation) {
 image_stream_.bind_current_context();
 lane.store = product.store;
 lane.identity = {.incarnation = lane.store.get(),
  .dataset = product.plan.dataset_identity,
  .seed = product.plan.augmentation.seed,
  .augmentation = product.plan.augmentation_config,
  .extent = explore_atlas_card_extent(product.viewport),
  .augmented = product.plan.augmentation.enabled};
 lane.transfer_ready = false;
 lane.read_valid = false;
 lane.pending_meaning.reset();
 lane.preview_key = rfdetr::augmentation_preview_image_key(product.plan.dataset_identity, product.plan.augmentation.seed, compiled_index);
 lane.donor_instance.reset();
 const bool copy_paste_requested = product.plan.augmentation.enabled && product.plan.augmentation_config.enabled &&
                                   rfdetr::augmentation_paste_admitted(product.plan.augmentation_config, lane.preview_key) && !product.annotated_indices.empty();
 if (copy_paste_requested) {
  lane.donor_index = rfdetr::select_augmentation_preview_donor_image(product.annotated_indices, compiled_index, lane.preview_key);
  if (lane.donor_index != compiled_index) {
   const auto donor_instances = product.store->image_labels(lane.donor_index);
   const auto count = static_cast<std::size_t>(std::ranges::count_if(donor_instances, [](const auto& instance) { return !instance.is_crowd(); }));
   if (count != 0U) {
    auto remaining = rfdetr::select_augmentation_preview_donor_instance(count, lane.preview_key);
    for (const auto& instance : donor_instances) {
     if (instance.is_crowd()) continue;
     if (remaining == 0U) {
      lane.donor_instance = instance;
      break;
     }
     --remaining;
    }
   }
  }
 }
 lane.layout = LayoutFor(product, compiled_index, lane.donor_instance.has_value(), lane.donor_instance ? lane.donor_instance->mask_rle_pairs : 0U);
 image_stream_.prepare_metadata(lane.index, lane.layout.bytes);
 image_stream_.prepare_images(lane.index, 2U * lane.layout.pixel_bytes);
 lane.generation = generation;
 lane.reserved_generation = generation;
 lane.tile_generation = ++next_tile_generation_;
 lane.diagnostic_link = diagnostics_.valid() ? services::DiagnosticSpanIds::Next() : contracts::DiagnosticLink{};
 lane.compiled_index = compiled_index;
 lane.destination_slot = slot;
 lane.card_extent = explore_atlas_card_extent(product.viewport);
 lane.columns = product.viewport.columns;
 lane.first_row = product.viewport.first_row;
 lane.failure = {};
 const explore::ExploreRenderTileDescriptor tile{
  .card_index = slot,
  .destination_x = slot % lane.columns * lane.card_extent,
  .destination_y = slot / lane.columns * lane.card_extent,
  .destination_width = lane.card_extent,
  .destination_height = lane.card_extent,
  .generation = {.viewport = generation, .tile = lane.tile_generation},
 };
 store_payload(lane.pinned.data(), lane.layout.tile, tile);
}
void GalleryReadScheduler::Prioritize(GalleryProductState& product) {
 next_priority_ = 0U;
 product.priority_slots.clear();
 if (product.window_indices.empty()) return;
 product.priority_slots.reserve(product.window_indices.size());
 const auto first = std::min(static_cast<std::size_t>(product.viewport.first_row) * product.viewport.columns, product.window_first + product.window_indices.size());
 const auto visible_offset = first - product.window_first;
 for (std::size_t slot = 0U; slot != product.visible_indices.size(); ++slot) product.priority_slots.push_back(static_cast<std::uint32_t>(visible_offset + slot));
 const auto end = visible_offset + product.visible_indices.size();
 const auto columns = product.viewport.columns;
 const auto ahead = [&] {
  for (auto offset = end; offset < product.window_indices.size(); ++offset) product.priority_slots.push_back(static_cast<std::uint32_t>(offset));
 };
 const auto behind = [&] {
  for (auto row_end = visible_offset; row_end != 0U;) {
   const auto row_begin = row_end > columns ? row_end - columns : 0U;
   for (auto offset = row_begin; offset < row_end; ++offset) product.priority_slots.push_back(static_cast<std::uint32_t>(offset));
   row_end = row_begin;
  }
 };
 if (product.plan.scroll_direction == ExploreScrollDirection::Forward) {
  ahead();
  behind();
 } else {
  behind();
  ahead();
 }
 priority_rank_.resize(product.cache.size());
 for (std::size_t rank = 0U; rank < product.priority_slots.size(); ++rank) priority_rank_[product.cache.Slot(product.window_first + product.priority_slots[rank])] = rank;
}
bool GalleryReadScheduler::ReserveInput(const GalleryProductState& product, Lane& lane) {
 if (product.plan.mode != ExploreMode::Gallery || lane.store.get() != product.store.get() || lane.identity != product.cache.identity()) return false;
 const auto position = product.cache.Position(lane.compiled_index);
 if (position == std::numeric_limits<std::size_t>::max() || product.cache.Find(lane.compiled_index)) return false;
 auto& scheduled = scheduled_slots_[product.cache.Slot(position)];
 if (scheduled == product.plan.generation && lane.reserved_generation != product.plan.generation) return false;
 scheduled = product.plan.generation;
 lane.reserved_generation = product.plan.generation;
 lane.reserved_position = position;
 return true;
}
void GalleryReadScheduler::DiscardSettledInput(const GalleryProductState& product, Lane& lane) {
 if (lane.reserved_generation != 0U && lane.DemandGeneration() == product.plan.generation && product.cache.size() != 0U) {
  scheduled_slots_[product.cache.Slot(lane.reserved_position)] = 0U;
  // The priority cursor may already have skipped this reservation while
  // its old callback was pending. An obsolete failure releases the claim,
  // not the newly committed demand for an as-yet-unfilled position.
  if (!product.cache.Find(lane.compiled_index)) next_priority_ = 0U;
 }
 lane.reserved_generation = 0U;
 lane.state = LaneState::Idle;
 lane.store.reset();
 lane.pending_meaning.reset();
 lane.failure = {};
 lane.read_valid = false;
}
void GalleryReadScheduler::ReconcileInitializationLane(const GalleryProductState& product, const GalleryThumbnailCache* incumbent, Lane& lane) {
 switch (lane.state) {
  case LaneState::Idle:
  case LaneState::Preparing: DiscardSettledInput(product, lane); break;
  case LaneState::Queued:
  case LaneState::Reading:
  case LaneState::AwaitingTransfer:
  case LaneState::GpuPending:
   // No physical metadata is rewritten before completion. Claiming
   // its destination now prevents another free lane duplicating it.
   static_cast<void>(ReserveInput(product, lane));
   break;
  case LaneState::InputReady:
  case LaneState::StaleReady:
  case LaneState::GpuComplete: RebindInput(product, incumbent, lane); break;
  case LaneState::Failed:
   if (lane.DemandGeneration() != product.plan.generation) DiscardSettledInput(product, lane);
   break;
 }
}
void GalleryReadScheduler::RebindInput(const GalleryProductState& product, const GalleryThumbnailCache* incumbent, Lane& lane) {
 if (!lane.read_valid || !lane.transfer_ready || lane.failure || !ReserveInput(product, lane)) {
  DiscardSettledInput(product, lane);
  return;
 }
 lane.position = lane.reserved_position;
 const auto generation = product.plan.generation;
 const auto first = static_cast<std::size_t>(product.viewport.first_row) * product.viewport.columns;
 lane.prefetch = lane.position < first || lane.position - first >= product.visible_indices.size();
 lane.destination_slot = static_cast<std::uint32_t>(lane.prefetch ? product.cache.Slot(lane.position) : lane.position - first);
 lane.generation = generation;
 lane.columns = product.viewport.columns;
 lane.first_row = product.viewport.first_row;
 lane.cache_bank = product.cache.WritableBank(lane.position, incumbent);
 lane.semantic_bank = product.cache.WritableBank(lane.position, incumbent, true);
 lane.state = LaneState::InputReady;
 scheduled_slots_[product.cache.Slot(lane.position)] = generation;
 const explore::ExploreRenderTileDescriptor tile{.card_index = lane.destination_slot,
  .destination_x = lane.prefetch ? 0U : lane.destination_slot % lane.columns * lane.card_extent,
  .destination_y = lane.prefetch ? static_cast<std::uint32_t>(product.cache.PhysicalRow(product.cache.Slot(lane.position), lane.cache_bank)) : lane.destination_slot / lane.columns * lane.card_extent,
  .destination_width = lane.card_extent,
  .destination_height = lane.card_extent,
  .generation = {.viewport = generation, .tile = ++next_tile_generation_}};
 store_payload(lane.pinned.data(), lane.layout.tile, tile);
}
void GalleryReadScheduler::StartIdleLanes(const GalleryProductState& product, const GalleryThumbnailCache* incumbent) {
 const auto generation = desired_generation_.load(std::memory_order_acquire);
 if (!Current(generation)) return;
 // Rebind the acceptance gate to restored demand. In-flight reads keep
 // their physical lanes through cancellation and view changes.
 if (acceptance_) acceptance_->AdvanceGeneration(generation);
 for (std::size_t lane_index = 0U; lane_index != lanes_.size(); ++lane_index) {
  if (!Current(generation)) return;
  std::uint32_t slot = 0U;
  std::uint32_t compiled_index = 0U;
  std::size_t position = 0U;
  bool prefetch = false;
  {
   std::scoped_lock lock(lanes_mutex_);
   while (next_priority_ < product.priority_slots.size()) {
    const auto offset = product.priority_slots[next_priority_];
    const auto demand_position = product.window_first + offset;
    if (!product.cache.Find(product.window_indices[offset]) && scheduled_slots_[product.cache.Slot(demand_position)] != generation) break;
    ++next_priority_;
   }
   if (next_priority_ == product.priority_slots.size()) return;
   Lane& lane = *lanes_[lane_index];
   const auto first = static_cast<std::size_t>(product.viewport.first_row) * product.viewport.columns;
   const auto next_position = product.window_first + product.priority_slots[next_priority_];
   const bool next_visible = next_position >= first && next_position - first < product.visible_indices.size();
   if (next_visible && lane.state == LaneState::InputReady && lane.prefetch) {
    const auto immediate_cursor = next_priority_;
    DiscardSettledInput(product, lane);
    // The released reservation is in a later tier. Finish the
    // current immediate admission before revisiting speculation.
    next_priority_ = immediate_cursor;
   }
   if (lane.state != LaneState::Idle) continue;
   const auto speculative = std::ranges::count_if(lanes_, [](const auto& candidate) { return candidate->state != LaneState::Idle && candidate->prefetch; });
   if (!next_visible && speculative >= static_cast<std::ptrdiff_t>(std::max<std::size_t>(1U, lanes_.size() - 1U))) return;
   const auto offset = product.priority_slots[next_priority_++];
   position = product.window_first + offset;
   compiled_index = product.window_indices[offset];
   prefetch = position < first || position - first >= product.visible_indices.size();
   slot = static_cast<std::uint32_t>(prefetch ? product.cache.Slot(position) : position - first);
   scheduled_slots_[product.cache.Slot(position)] = generation;
   lane.position = position;
   lane.reserved_position = position;
   lane.reserved_generation = generation;
   lane.state = LaneState::Preparing;
  }
  Lane& lane = *lanes_[lane_index];
  try {
   PrepareLaneStorage(product, lane, compiled_index, slot, generation);
   lane.position = position;
   lane.cache_bank = product.cache.WritableBank(position, incumbent);
   lane.prefetch = prefetch;
   if (prefetch) {
    auto tile = load_payload<explore::ExploreRenderTileDescriptor>(lane.pinned.data(), lane.layout.tile);
    tile.destination_x = 0U;
    tile.destination_y = static_cast<std::uint32_t>(product.cache.PhysicalRow(product.cache.Slot(position), lane.cache_bank));
    store_payload(lane.pinned.data(), lane.layout.tile, tile);
   }
   {
    std::scoped_lock lock(lanes_mutex_);
    if (!Current(generation)) {
     lane.state = LaneState::Idle;
     return;
    }
    lane.state = LaneState::Queued;
   }
   diagnostics_.Emit([&] {
    std::scoped_lock lock(lanes_mutex_);
    contracts::DiagnosticExploreAdmission admission{.admission_position = position,
     .admission_first_row = product.viewport.first_row,
     .admission_row_count = product.viewport.row_count,
     .admission_columns = product.viewport.columns,
     .admission_tier = !prefetch ? 0U : ((position / product.viewport.columns >= product.viewport.first_row) == (product.plan.scroll_direction == ExploreScrollDirection::Forward) ? 1U : 2U),
     .admission_forward = product.plan.scroll_direction == ExploreScrollDirection::Forward};
    for (std::size_t offset = 0U; offset < product.window_indices.size(); ++offset) {
     const auto candidate = product.window_first + offset;
     if (product.cache.Find(product.window_indices[offset]) || scheduled_slots_[product.cache.Slot(candidate)] == generation) continue;
     const auto row = candidate / admission.admission_columns;
     if (row < admission.admission_first_row)
      ++admission.admission_backward_eligible;
     else if (row >= admission.admission_first_row + admission.admission_row_count)
      ++admission.admission_forward_eligible;
     else
      ++admission.admission_immediate_eligible;
    }
    return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
     .operation = VisualDiagnosticOperation::GalleryReadScheduled,
     .device = device_,
     .generation = generation,
     .value = lane_index,
     .detail = compiled_index,
     .context = {.admission = admission}};
   });
   SubmitRead(lane, true);
  } catch (...) {
   std::shared_ptr<const ExploreAlgorithm::GalleryReadySink> sink;
   {
    std::scoped_lock lock(lanes_mutex_);
    lane.failure = std::current_exception();
    lane.state = LaneState::Failed;
    sink = ready_sink_;
   }
   if (sink) (*sink)();
   if (Current(generation)) throw;
   return;
  }
 }
}
bool GalleryReadScheduler::HasReadyTiles() const {
 const auto generation = desired_generation_.load(std::memory_order_acquire);
 std::scoped_lock lock(lanes_mutex_);
 return std::ranges::any_of(lanes_, [generation](const auto& lane) { return lane->state == LaneState::InputReady && lane->generation == generation && !lane->prefetch; });
}
void GalleryReadScheduler::SetReadySink(ExploreAlgorithm::GalleryReadySink sink) {
 auto retained = sink ? std::make_shared<const ExploreAlgorithm::GalleryReadySink>(std::move(sink)) : nullptr;
 std::scoped_lock lock(lanes_mutex_);
 ready_sink_ = std::move(retained);
}
void GalleryReadScheduler::StopIngress() noexcept {
 desired_generation_.store(0U, std::memory_order_release);
 image_stream_.cancel_reads();
 // Runtime retirement also occurs while a staged replacement is promoted.
 // Invalidate this gallery's waiters without terminalizing the acceptance
 // gate shared by the reusable runtime factory and its replacement.
 if (acceptance_) acceptance_->AdvanceGeneration(0U);
 {
  std::scoped_lock lock(lanes_mutex_);
  ready_sink_ = {};
 }
}
void GalleryReadScheduler::SettleConsumers() {
 image_stream_.wait_consumers();
 image_stream_.synchronize(detail_lane_->index);
}
void GalleryReadScheduler::RestoreSettledLanes() {
 std::scoped_lock lock(lanes_mutex_);
 for (auto& lane : lanes_) {
  if (lane->state != LaneState::GpuPending && lane->state != LaneState::GpuComplete) continue;
  if (lane->failure)
   lane->state = LaneState::Failed;
  else
   lane->state = lane->read_valid && lane->transfer_ready ? LaneState::InputReady : LaneState::StaleReady;
 }
 detail_lane_->state = LaneState::Idle;
 detail_lane_->store.reset();
 detail_lane_->pending_meaning.reset();
}
void GalleryReadScheduler::QuiesceReads(std::size_t priority_count) {
 image_stream_.cancel_reads();
 image_stream_.synchronize();
 std::scoped_lock lock(lanes_mutex_);
 for (auto& lane : lanes_) {
  lane->state = LaneState::Idle;
  lane->store.reset();
  lane->pending_meaning.reset();
  lane->failure = {};
 }
 next_priority_ = priority_count;
}
void GalleryReadScheduler::ResetLogical() {
 next_tile_generation_ = 0U;
 scheduled_slots_.clear();
 stale_discarded_.store(0U, std::memory_order_release);
 desired_generation_.store(0U, std::memory_order_release);
 std::scoped_lock lock(lanes_mutex_);
 for (auto& lane : lanes_) {
  lane->layout = {};
  lane->state = LaneState::Idle;
  lane->generation = 0U;
  lane->reserved_generation = 0U;
  lane->tile_generation = 0U;
  lane->compiled_index = 0U;
  lane->destination_slot = 0U;
  lane->card_extent = 0U;
  lane->columns = 1U;
  lane->first_row = 0U;
  lane->preview_key = 0U;
  lane->donor_index = 0U;
  lane->donor_instance.reset();
  lane->pending_meaning.reset();
  lane->failure = {};
 }
}
void GalleryReadScheduler::ReleaseLane(Lane& lane, cudaStream_t stream) { image_stream_.release(lane.index, stream, LaneCompletion()); }
}  // namespace mmltk::controller::explore_detail
