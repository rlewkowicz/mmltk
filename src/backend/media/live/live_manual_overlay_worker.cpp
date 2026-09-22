#include "detail/live_manual_overlay_worker.h"
#include "src/backend/imaging/annotation/manual_mask_mapping.h"
#include "detail/overlay_palette.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
import mmltk.backend.imaging.raster;
namespace mmltk::backend::media::live {
namespace annotation = mmltk::backend::imaging::annotation;
namespace raster = mmltk::backend::imaging::raster;
namespace {
constexpr double kTwoPi = 6.28318530717958647692;
[[nodiscard]] bool box_has_area(const ManualOverlayBox& box) noexcept { return box.x2 > box.x1 && box.y2 > box.y1; }
[[nodiscard]] const ManualOverlayInstance& instance_at(const ManualOverlayDocumentSnapshot& snapshot, const std::size_t index) {
 return index < snapshot.instances.size() ? snapshot.instances[index] : snapshot.interaction_instances[index - snapshot.instances.size()];
}
[[nodiscard]] bool add_bounded(const std::size_t count, const std::size_t limit, std::size_t* total) noexcept {
 if (total == nullptr || *total > limit || count > limit - *total) return false;
 *total += count;
 return true;
}
[[nodiscard]] std::uint8_t* offset_rgba(const CUdeviceptr rgba, const std::size_t pitch, const ManualOverlayMaskRegion region) noexcept {
 return reinterpret_cast<std::uint8_t*>(rgba) + static_cast<std::size_t>(region.capture_y) * pitch + static_cast<std::size_t>(region.capture_x) * 4U;
}
}  // namespace
LiveManualOverlayWorker::LiveManualOverlayWorker(ManualOverlayDocument& document, const std::uint32_t count, const std::uint32_t width, const std::uint32_t height,
 const std::uint32_t maximum_instances, const LiveManualOverlayUploadLimits upload_limits, LivePhysicalCudaContext cuda)
    : document_(document),
      cuda_(std::move(cuda)),
      slots_(count == 0U ? nullptr : std::make_unique<Slot[]>(count)),
      slot_count_(count),
      width_(width),
      height_(height),
      maximum_instances_(maximum_instances),
      upload_limits_(upload_limits) {
 if (count == 0U || width == 0U || height == 0U || maximum_instances == 0U || !upload_limits.valid() || !cuda_.valid()) throw std::invalid_argument("Live manual overlay requires fixed CUDA storage");
 try {
  auto scope = cuda_.scope();
  if (!scope) throw std::runtime_error("enter Live manual-overlay CUDA scope");
  for (std::uint32_t index = 0U; index < count; ++index) {
   Slot& slot = slots_[index];
   slot.index = index;
   slot.owner = this;
   slot.packed = std::make_unique<PackedInstance[]>(maximum_instances);
   if (scope.Record(cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking)) != cudaSuccess) throw std::runtime_error("create Live manual-overlay stream");
   if (scope.Record(cudaEventCreateWithFlags(&slot.ready, cudaEventDisableTiming)) != cudaSuccess) throw std::runtime_error("create Live manual-overlay event");
   if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slot.rgba), &slot.pitch, static_cast<std::size_t>(width) * 4U, height)) != cudaSuccess)
    throw std::runtime_error("allocate Live manual-overlay slot");
   allocate_upload(slot.masks, upload_limits.mask_bytes, scope);
   allocate_upload(slot.runs, upload_limits.run_values * sizeof(std::uint32_t), scope);
   allocate_upload(slot.points, upload_limits.point_values * sizeof(int), scope);
   allocate_upload(slot.edges, upload_limits.edge_values * sizeof(std::uint32_t), scope);
   allocate_upload(slot.brush, upload_limits.brush_values * sizeof(int), scope);
  }
 } catch (...) {
  destroy();
  throw;
 }
}
LiveManualOverlayWorker::~LiveManualOverlayWorker() { destroy(); }
void LiveManualOverlayWorker::start() noexcept { running_.store(true, std::memory_order_release); }
void LiveManualOverlayWorker::close_admission() noexcept { running_.store(false, std::memory_order_release); }
void LiveManualOverlayWorker::ScrubProduct(Slot& slot) noexcept {
 slot.generation = 0U;
 slot.has_content = false;
}
void LiveManualOverlayWorker::publish_slot(Slot& slot, const SlotState published) noexcept {
 // CLEANUP-IGNORE: Publication already uses the shared slot helper; the adjacent stop functions settle different product resources.
 publish_latest_live_owner_slot(latest_, slot.index, slot.state, published, [&slot] noexcept { ScrubProduct(slot); });
}
void LiveManualOverlayWorker::stop() noexcept {
 close_admission();
 if (slots_ == nullptr) return;
 latest_.store(-1, std::memory_order_release);
 published_generation_.store(0U, std::memory_order_release);
 auto scope = cuda_.scope();
 retire_live_slots(scope, slots_.get(), slot_count_, [this](Slot& slot, const SlotState published) noexcept { publish_slot(slot, published); });
}
LiveManualOverlayWorker::SlotReservation LiveManualOverlayWorker::reserve() noexcept { return {reserve_live_slot(slots_.get(), slot_count_).slot}; }
void LiveManualOverlayWorker::allocate_upload(UploadStorage& storage, const std::size_t bytes, LiveCudaCommandScope& scope) {
 if (bytes == 0U) throw std::invalid_argument("Live manual-overlay upload storage cannot be empty");
 auto host = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
 host->ensure_bytes(bytes);
 void* device = nullptr;
 cudaError_t status = scope.Record(cudaMalloc(&device, bytes));
 if (status != cudaSuccess) {
  if (device != nullptr) static_cast<void>(scope.Record(cudaFree(device)));
  throw std::runtime_error("allocate Live manual-overlay upload storage");
 }
 storage.host = host->data();
 storage.storage = std::move(host);
 storage.device = reinterpret_cast<CUdeviceptr>(device);
}
LiveManualOverlayWorker::PrepareResult LiveManualOverlayWorker::prepare_uploads(const ManualOverlayDocumentSnapshot& snapshot, Slot& slot, std::size_t* instance_count_out) {
 if (instance_count_out == nullptr || snapshot.interaction_instances.size() > std::numeric_limits<std::size_t>::max() - snapshot.instances.size()) return PrepareResult::Refused;
 const std::size_t instance_count = snapshot.instances.size() + snapshot.interaction_instances.size();
 if (instance_count > maximum_instances_) return PrepareResult::Refused;
 *instance_count_out = instance_count;
 std::size_t mask_bytes_total = 0U;
 std::size_t run_values_total = 0U;
 std::size_t point_values_total = 0U;
 std::size_t edge_values_total = 0U;
 const bool dimensions_valid = snapshot.capture_width != 0U && snapshot.capture_height != 0U && snapshot.capture_width <= width_ && snapshot.capture_height <= height_;
 if (!dimensions_valid) return PrepareResult::Refused;
 for (std::size_t index = 0U; index < instance_count; ++index) {
  const ManualOverlayInstance& instance = instance_at(snapshot, index);
  PackedInstance& packed = slot.packed[index];
  packed = {};
  packed.mask_offset = mask_bytes_total;
  packed.run_value_offset = run_values_total;
  packed.polyline_value_offset = point_values_total;
  packed.point_value_offset = point_values_total;
  packed.edge_value_offset = edge_values_total;
  if (!instance.enabled) continue;
  if ((!instance.mask.empty() && (!instance.mask_runs.empty() || instance.deferred_mask_mapping.has_value())) || instance.mask_runs.size() > upload_limits_.run_values / 2U)
   return PrepareResult::Refused;
  if (instance.deferred_mask_mapping.has_value()) {
   packed.deferred_mask_projection = validate_manual_overlay_deferred_mask(*instance.deferred_mask_mapping, instance.mask_region, instance.mask_runs, snapshot.capture_width, snapshot.capture_height);
   if (!packed.deferred_mask_projection.has_value()) return PrepareResult::Refused;
  }
  const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) * instance.mask_region.height;
  const bool has_mask_payload = !instance.mask.empty() || !instance.mask_runs.empty();
  if (has_mask_payload && !manual_overlay_mask_region_contained(instance.mask_region, snapshot.capture_width, snapshot.capture_height)) return PrepareResult::Refused;
  if (!instance.mask.empty()) {
   if (mask_bytes == 0U || instance.mask.size() != mask_bytes || !add_bounded(mask_bytes, upload_limits_.mask_bytes, &mask_bytes_total)) return PrepareResult::Refused;
  } else if (!instance.mask_runs.empty()) {
   if (!packed.deferred_mask_projection.has_value()) {
    const ManualOverlayDeferredMaskMapping local{
     instance.mask_region.width, instance.mask_region.height, 0U, 0U, instance.mask_region.width, instance.mask_region.height, snapshot.capture_width, snapshot.capture_height, 0U, 0U};
    if (!validate_manual_overlay_deferred_mask(local, instance.mask_region, instance.mask_runs, snapshot.capture_width, snapshot.capture_height).has_value()) return PrepareResult::Refused;
   }
   const std::size_t values = packed.deferred_mask_projection.has_value() ? packed.deferred_mask_projection->run_value_count : instance.mask_runs.size() * 2U;
   if (!add_bounded(values, upload_limits_.run_values, &run_values_total)) return PrepareResult::Refused;
  }
  if (snapshot.renderer_mode != SemanticRenderer::Iced) {
   if (instance.polyline_points.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || instance.points.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
       instance.skeleton_edges.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return PrepareResult::Refused;
   packed.polyline_value_offset = point_values_total;
   if (instance.polyline_points.size() > std::numeric_limits<std::size_t>::max() / 2U || !add_bounded(instance.polyline_points.size() * 2U, upload_limits_.point_values, &point_values_total))
    return PrepareResult::Refused;
   packed.point_value_offset = point_values_total;
   if (instance.points.size() > std::numeric_limits<std::size_t>::max() / 2U || !add_bounded(instance.points.size() * 2U, upload_limits_.point_values, &point_values_total))
    return PrepareResult::Refused;
   packed.edge_value_offset = edge_values_total;
   if (instance.skeleton_edges.size() > std::numeric_limits<std::size_t>::max() / 2U || !add_bounded(instance.skeleton_edges.size() * 2U, upload_limits_.edge_values, &edge_values_total))
    return PrepareResult::Refused;
   for (const ManualOverlayEdge edge : instance.skeleton_edges)
    if (edge.source_index >= instance.points.size() || edge.target_index >= instance.points.size()) return PrepareResult::Refused;
  }
 }
 if (snapshot.selected_instance.has_value() && *snapshot.selected_instance >= snapshot.instances.size()) return PrepareResult::Refused;
 auto* masks = static_cast<std::uint8_t*>(slot.masks.host);
 auto* runs = static_cast<std::uint32_t*>(slot.runs.host);
 auto* points = static_cast<int*>(slot.points.host);
 auto* edges = static_cast<std::uint32_t*>(slot.edges.host);
 for (std::size_t index = 0U; index < instance_count; ++index) {
  const ManualOverlayInstance& instance = instance_at(snapshot, index);
  const PackedInstance& packed = slot.packed[index];
  if (!instance.enabled) continue;
  const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) * instance.mask_region.height;
  if (mask_bytes != 0U && instance.mask.size() == mask_bytes) {
   std::memcpy(masks + packed.mask_offset, instance.mask.data(), mask_bytes);
  } else if (mask_bytes != 0U && !instance.mask_runs.empty()) {
   std::size_t write = packed.run_value_offset;
   for (const ManualOverlayMaskRun run : instance.mask_runs) {
    runs[write++] = run.offset;
    runs[write++] = run.length;
   }
  }
  if (snapshot.renderer_mode != SemanticRenderer::Iced) {
   std::size_t write = packed.polyline_value_offset;
   for (const ManualOverlayPoint point : instance.polyline_points) {
    points[write++] = point.x;
    points[write++] = point.y;
   }
   write = packed.point_value_offset;
   for (const ManualOverlayPoint point : instance.points) {
    points[write++] = point.x;
    points[write++] = point.y;
   }
   write = packed.edge_value_offset;
   for (const ManualOverlayEdge edge : instance.skeleton_edges) {
    edges[write++] = edge.source_index;
    edges[write++] = edge.target_index;
   }
  }
 }
 auto scope = cuda_.scope();
 if (!scope) return PrepareResult::CudaFailure;
 cudaError_t status = cudaSuccess;
 const auto upload = [&](const UploadStorage& storage, const std::size_t bytes) {
  if (status != cudaSuccess || bytes == 0U) return;
  status = scope.Record(cudaMemcpyAsync(reinterpret_cast<void*>(storage.device), storage.host, bytes, cudaMemcpyHostToDevice, slot.stream));
 };
 upload(slot.masks, mask_bytes_total);
 upload(slot.runs, run_values_total * sizeof(std::uint32_t));
 upload(slot.points, point_values_total * sizeof(int));
 upload(slot.edges, edge_values_total * sizeof(std::uint32_t));
 return status == cudaSuccess ? PrepareResult::Ready : PrepareResult::CudaFailure;
}
bool LiveManualOverlayWorker::render_snapshot(const ManualOverlayDocumentSnapshot& snapshot, Slot& slot, const std::size_t instance_count) {
 auto scope = cuda_.scope();
 if (!scope) return false;
 cudaError_t status = scope.Record(cudaMemset2DAsync(reinterpret_cast<void*>(slot.rgba), slot.pitch, 0, static_cast<std::size_t>(width_) * 4U, height_, slot.stream));
 bool has_content = false;
 const raster::MutableBytes target{reinterpret_cast<std::uint8_t*>(slot.rgba), slot.pitch, static_cast<int>(width_), static_cast<int>(height_)};
 for (std::size_t index = 0U; index < instance_count && status == cudaSuccess; ++index) {
  const ManualOverlayInstance& instance = instance_at(snapshot, index);
  const PackedInstance& packed = slot.packed[index];
  if (!instance.enabled) continue;
  const std::array<std::uint8_t, 3> color =
   instance.style.has_value() ? std::array<std::uint8_t, 3>{instance.style->r, instance.style->g, instance.style->b} : manual_overlay_category_color(instance.category_index);
  const std::uint8_t alpha = instance.style.has_value() ? instance.style->alpha : 97U;
  const int thickness = std::max(1, instance.style.has_value() ? instance.style->line_thickness : 2);
  const int radius = std::max(1, instance.style.has_value() ? instance.style->point_radius : 3);
  const std::size_t mask_bytes = static_cast<std::size_t>(instance.mask_region.width) * instance.mask_region.height;
  if (mask_bytes != 0U && instance.mask.size() == mask_bytes) {
   status = scope.Record(raster::raster_mask_rgba(
    {.overlay = {offset_rgba(slot.rgba, slot.pitch, instance.mask_region), slot.pitch, static_cast<int>(instance.mask_region.width), static_cast<int>(instance.mask_region.height)},
     .mask = reinterpret_cast<const std::uint8_t*>(slot.masks.device + packed.mask_offset),
     .color = {color[0], color[1], color[2], alpha},
     .stream = slot.stream}));
   has_content = true;
  } else if (mask_bytes != 0U && !instance.mask_runs.empty()) {
   const auto* run_pairs = reinterpret_cast<const std::uint32_t*>(slot.runs.device + packed.run_value_offset * sizeof(std::uint32_t));
   if (packed.deferred_mask_projection.has_value()) {
    const auto& projection = *packed.deferred_mask_projection;
    status = scope.Record(static_cast<cudaError_t>(annotation::draw_deferred_manual_mask_runs_rgba_pitched(projection.mapping, offset_rgba(slot.rgba, slot.pitch, instance.mask_region), slot.pitch,
     static_cast<int>(instance.mask_region.width), static_cast<int>(instance.mask_region.height), run_pairs, projection.run_count, projection.region.capture_x, projection.region.capture_y, color[0],
     color[1], color[2], alpha, reinterpret_cast<std::uintptr_t>(slot.stream))));
   } else {
    status = scope.Record(raster::raster_mask_runs_rgba(
     {.overlay = {offset_rgba(slot.rgba, slot.pitch, instance.mask_region), slot.pitch, static_cast<int>(instance.mask_region.width), static_cast<int>(instance.mask_region.height)},
      .run_pairs = run_pairs,
      .run_count = static_cast<std::uint32_t>(instance.mask_runs.size()),
      .color = {color[0], color[1], color[2], alpha},
      .stream = slot.stream}));
   }
   has_content = true;
  }
  if (snapshot.renderer_mode != SemanticRenderer::Iced && instance.polyline_points.size() >= 2U && status == cudaSuccess) {
   status = scope.Record(raster::raster_polyline_rgba({.overlay = target,
    .points = {reinterpret_cast<const int*>(slot.points.device + packed.polyline_value_offset * sizeof(int)), static_cast<int>(instance.polyline_points.size())},
    .closed = instance.polyline_closed,
    .color = {color[0], color[1], color[2]},
    .thickness = thickness,
    .stream = slot.stream}));
   has_content = true;
  }
  if (snapshot.renderer_mode != SemanticRenderer::Iced && !instance.skeleton_edges.empty() && !instance.points.empty() && status == cudaSuccess) {
   status = scope.Record(raster::raster_skeleton_rgba({.overlay = target,
    .points = {reinterpret_cast<const int*>(slot.points.device + packed.point_value_offset * sizeof(int)), static_cast<int>(instance.points.size())},
    .edges = {reinterpret_cast<const std::uint32_t*>(slot.edges.device + packed.edge_value_offset * sizeof(std::uint32_t)), static_cast<int>(instance.skeleton_edges.size())},
    .color = {color[0], color[1], color[2]},
    .thickness = thickness,
    .stream = slot.stream}));
   has_content = true;
  }
  // CLEANUP-IGNORE: Skeleton edges and standalone point glyphs are distinct raster primitives with separate
  // buffers.
  if (snapshot.renderer_mode != SemanticRenderer::Iced && !instance.points.empty() && status == cudaSuccess) {
   status = scope.Record(raster::raster_points_rgba({.overlay = target,
    .points = {reinterpret_cast<const int*>(slot.points.device + packed.point_value_offset * sizeof(int)), static_cast<int>(instance.points.size())},
    .radius = radius,
    .color = {color[0], color[1], color[2], instance.style.has_value() ? instance.style->alpha : std::uint8_t{240U}},
    .stream = slot.stream}));
   has_content = true;
  }
  if (snapshot.renderer_mode != SemanticRenderer::Iced && box_has_area(instance.box) && status == cudaSuccess) {
   status = scope.Record(raster::raster_box_outline_rgba(
    {.overlay = target, .box = {instance.box.x1, instance.box.y1, instance.box.x2, instance.box.y2}, .color = {color[0], color[1], color[2]}, .thickness = thickness, .stream = slot.stream}));
   has_content = true;
   if (status == cudaSuccess && instance.style.has_value() && instance.style->draw_handles)
    status = scope.Record(raster::raster_selection_handles_rgba({.overlay = target,
     .box = {instance.box.x1, instance.box.y1, instance.box.x2, instance.box.y2},
     .handle_radius = std::max(1, instance.style->handle_radius),
     .color = {color[0], color[1], color[2], instance.style->alpha},
     .stream = slot.stream}));
  }
 }
 if (status == cudaSuccess && snapshot.renderer_mode != SemanticRenderer::Iced && snapshot.selected_instance.has_value() && *snapshot.selected_instance < snapshot.instances.size()) {
  const ManualOverlayInstance& selected = snapshot.instances[*snapshot.selected_instance];
  if (box_has_area(selected.box)) {
   status = scope.Record(raster::raster_selection_handles_rgba(
    {.overlay = target, .box = {selected.box.x1, selected.box.y1, selected.box.x2, selected.box.y2}, .handle_radius = 4, .color = {255U, 220U, 96U, 240U}, .stream = slot.stream}));
   has_content = true;
  }
 }
 if (status == cudaSuccess && snapshot.renderer_mode != SemanticRenderer::Iced && snapshot.brush_preview.has_value()) {
  const ManualOverlayBrushPreview& brush = *snapshot.brush_preview;
  auto* host = static_cast<int*>(slot.brush.host);
  const double brush_radius = static_cast<double>(std::max(1, brush.radius));
  for (std::size_t index = 0; index < kManualOverlayBrushSegments; ++index) {
   const double theta = kTwoPi * static_cast<double>(index) / static_cast<double>(kManualOverlayBrushSegments);
   host[index * 2] = static_cast<int>(std::lround(static_cast<double>(brush.capture_x) + std::cos(theta) * brush_radius));
   host[index * 2 + 1] = static_cast<int>(std::lround(static_cast<double>(brush.capture_y) + std::sin(theta) * brush_radius));
  }
  status = scope.Record(cudaMemcpyAsync(reinterpret_cast<void*>(slot.brush.device), slot.brush.host, kManualOverlayBrushValueCount * sizeof(int), cudaMemcpyHostToDevice, slot.stream));
  if (status == cudaSuccess)
   status = scope.Record(raster::raster_polyline_rgba({.overlay = target,
    .points = {reinterpret_cast<const int*>(slot.brush.device), kManualOverlayBrushSegments},
    .closed = true,
    .color = {brush.erase ? std::uint8_t{255U} : std::uint8_t{128U}, brush.erase ? std::uint8_t{128U} : std::uint8_t{255U}, brush.erase ? std::uint8_t{128U} : std::uint8_t{170U}},
    .thickness = 2,
    .stream = slot.stream}));
  has_content = true;
 }
 slot.has_content = has_content;
 return status == cudaSuccess;
}
bool LiveManualOverlayWorker::render_pending() {
 if (!running_.load(std::memory_order_acquire)) return false;
 const auto snapshot = document_.snapshot_if_changed(consumed_generation_);
 if (snapshot == nullptr) return false;
 const SlotReservation reservation = reserve();
 if (!reservation) return false;
 Slot& slot = *reservation.slot;
 std::size_t instance_count = 0U;
 const PrepareResult prepared = prepare_uploads(*snapshot, slot, &instance_count);
 if (prepared == PrepareResult::Refused) {
  consumed_generation_ = snapshot->generation;
  publish_slot(slot, SlotState::Free);
  return true;
 }
 if (prepared == PrepareResult::CudaFailure) {
  auto scope = cuda_.scope();
  const bool synchronized = scope && scope.Record(cudaStreamSynchronize(slot.stream)) == cudaSuccess;
  consumed_generation_ = snapshot->generation;
  publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
  return false;
 }
 if (!render_snapshot(*snapshot, slot, instance_count)) {
  auto scope = cuda_.scope();
  const bool synchronized = scope && scope.Record(cudaStreamSynchronize(slot.stream)) == cudaSuccess;
  consumed_generation_ = snapshot->generation;
  publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
  return false;
 }
 auto scope = cuda_.scope();
 if (!scope) {
  consumed_generation_ = snapshot->generation;
  publish_slot(slot, SlotState::Terminal);
  return false;
 }
 slot.generation = snapshot->generation;
 cudaError_t status = scope.Record(cudaEventRecord(slot.ready, slot.stream));
 if (status == cudaSuccess) status = scope.Record(cudaLaunchHostFunc(slot.stream, RenderComplete, &slot));
 if (status != cudaSuccess) {
  const bool synchronized = scope.Record(cudaStreamSynchronize(slot.stream)) == cudaSuccess;
  consumed_generation_ = snapshot->generation;
  publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
  return false;
 }
 consumed_generation_ = snapshot->generation;
 return true;
}
void CUDART_CB LiveManualOverlayWorker::RenderComplete(void* context) noexcept {
 auto& slot = *static_cast<Slot*>(context);
 if (slot.owner->running_.load(std::memory_order_acquire)) {
  publish_live_slot_state(slot.state, SlotState::Published);
  std::uint64_t current = slot.owner->published_generation_.load(std::memory_order_acquire);
  while (slot.generation > current && !slot.owner->published_generation_.compare_exchange_weak(current, slot.generation, std::memory_order_acq_rel, std::memory_order_acquire)) {}
  if (slot.generation > current) slot.owner->latest_.store(static_cast<int>(slot.index), std::memory_order_release);
 } else {
  slot.owner->publish_slot(slot, SlotState::Free);
 }
 // CLEANUP-IGNORE: This overlay acquisition transfers custody of an overlay-specific slot and view.
 slot.owner->ready_.notify();
}
bool LiveManualOverlayWorker::try_acquire_latest(OverlayView* output) {
 if (output == nullptr) return false;
 const int index = latest_.load(std::memory_order_acquire);
 if (index < 0 || index >= static_cast<int>(slot_count_)) return false;
 Slot& slot = slots_[static_cast<std::uint32_t>(index)];
 if (!transition_slot_state(slot.state, SlotState::Published, SlotState::Acquired)) return false;
 *output = {
  .slot = slot.index, .frame = {}, .rgba = slot.rgba, .pitch_bytes = slot.pitch, .width = width_, .height = height_, .ready = slot.ready, .stream = slot.stream, .has_content = slot.has_content};
 return true;
}
void LiveManualOverlayWorker::release(const std::uint32_t index) noexcept {
 if (index >= slot_count_) {
  cuda_.Record(cudaErrorUnknown);
  return;
 }
 Slot& slot = slots_[index];
 if (running_.load(std::memory_order_acquire) && transition_slot_state(slot.state, SlotState::Acquired, SlotState::Published)) {
  ready_.notify();
  return;
 }
 if (!claim_live_slot(slot.state, SlotState::Acquired)) {
  cuda_.Record(cudaErrorUnknown);
  return;
 }
 publish_slot(slot, SlotState::Free);
 ready_.notify();
}
void LiveManualOverlayWorker::terminalize(const std::uint32_t index) noexcept {
 Slot* const slot = index < slot_count_ ? &slots_[index] : nullptr;
 if (!claim_acquired_live_slot(slot == nullptr ? nullptr : &slot->state, cuda_)) return;
 publish_slot(*slot, SlotState::Terminal);
 ready_.notify();
}
void LiveManualOverlayWorker::set_ready_listener(std::function<void()> listener) { ready_.set_listener(std::move(listener)); }
void LiveManualOverlayWorker::release_upload(UploadStorage& storage) noexcept {
 auto scope = cuda_.scope();
 if (storage.host != nullptr) {
  storage.storage.reset();
  storage.host = nullptr;
 }
 if (storage.device != 0U) {
  if (scope) free_live_cuda_allocation(scope, storage.device);
  storage.device = 0U;
 }
}
void LiveManualOverlayWorker::destroy() noexcept {
 if (slots_ == nullptr) return;
 stop();
 auto scope = cuda_.scope();
 for (std::uint32_t index = 0U; index < slot_count_; ++index) {
  Slot& slot = slots_[index];
  release_upload(slot.masks);
  release_upload(slot.runs);
  release_upload(slot.points);
  release_upload(slot.edges);
  release_upload(slot.brush);
  if (scope) {
   destroy_live_cuda_event(scope, slot.ready);
   destroy_live_cuda_stream(scope, slot.stream);
   free_live_cuda_allocation(scope, slot.rgba);
  }
  slot.ready = nullptr;
  slot.stream = nullptr;
  slot.rgba = 0U;
  slot.pitch = 0U;
  slot.packed.reset();
 }
 slots_.reset();
}
}  // namespace mmltk::backend::media::live
