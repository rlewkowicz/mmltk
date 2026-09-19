#include "detail/live_compositor_owner.h"
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/common/system/time_utils.h"
#include "src/frameworks/gpu/resource_owner_command_authority.h"
import mmltk.backend.imaging.raster;
namespace mmltk::backend::media::live {
namespace raster = mmltk::backend::imaging::raster;
namespace system = mmltk::common::system;
namespace gpu = mmltk::frameworks::gpu;
LiveCompositor::LiveCompositor(LiveFrameFanout& fanout, LiveAnalyzerWorker* analyzer, LiveManualOverlayWorker* manual_overlay,
                               LiveCompletedFramePublication& publication, const std::uint32_t count, const std::uint32_t width, const std::uint32_t height,
                               LivePhysicalCudaContext cuda)
    : fanout_(fanout),
      analyzer_(analyzer),
      manual_overlay_(manual_overlay),
      publication_(publication),
      cuda_(std::move(cuda)),
      slots_(count == 0U ? nullptr : std::make_unique<CompositeSlot[]>(count)),
      slot_count_(count),
      width_(width),
      height_(height) {
    if (count == 0U || width == 0U || height == 0U || !cuda_.valid()) throw std::invalid_argument("Live compositor requires fixed CUDA storage");
    try {
        auto scope = cuda_.scope();
        if (!scope) throw std::runtime_error("enter Live compositor CUDA scope");
        for (std::uint32_t index = 0; index < count; ++index) {
            CompositeSlot& slot = slots_[index];
            slot.owner = this;
            slot.index = index;
            if (scope.Record(cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking)) != cudaSuccess)
                throw std::runtime_error("create Live compositor stream");
            if (scope.Record(cudaEventCreateWithFlags(&slot.ready, cudaEventDisableTiming)) != cudaSuccess)
                throw std::runtime_error("create Live compositor event");
            if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slot.rgba), &slot.rgba_pitch, static_cast<std::size_t>(width) * 4U, height)) !=
                cudaSuccess)
                throw std::runtime_error("allocate Live composite slot");
            if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slot.overlay), &slot.overlay_pitch, static_cast<std::size_t>(width) * 4U, height)) !=
                cudaSuccess)
                // CLEANUP-IGNORE: This compositor retains disjoint physical slots and owns its own admission and
                // teardown lifecycle.
                throw std::runtime_error("allocate Live overlay slot");
        }
    } catch (...) {
        destroy();
        throw;
    }
}
LiveCompositor::~LiveCompositor() { destroy(); }
void LiveCompositor::start() noexcept { running_.store(true, std::memory_order_release); }
void LiveCompositor::close_admission() noexcept { running_.store(false, std::memory_order_release); }
void LiveCompositor::ScrubLogicalProduct(CompositeSlot& slot) noexcept {
    slot.metadata = {};
    slot.revision = 0U;
    slot.source_slot = 0U;
    slot.analysis_slot.reset();
    slot.manual_overlay_slot.reset();
    slot.producer_complete.store(false, std::memory_order_relaxed);
}
void LiveCompositor::publish_slot(CompositeSlot& slot, const SlotState published) noexcept {
    publish_live_owner_slot(slot.state, published, [&slot] noexcept { ScrubLogicalProduct(slot); });
}
void LiveCompositor::stop() noexcept {
    close_admission();
    if (slots_ == nullptr) return;
    auto scope = cuda_.scope();
    if (scope) static_cast<void>(drain_completions());
    for (std::uint32_t index = 0U; index < slot_count_; ++index) {
        CompositeSlot& slot = slots_[index];
        bool synchronized = static_cast<bool>(scope);
        if (scope && slot.stream != nullptr) synchronized = scope.Record(cudaStreamSynchronize(slot.stream)) == cudaSuccess;
        const SlotState current = static_cast<SlotState>(slot.state.load(std::memory_order_acquire));
        if (current == SlotState::Acquired || current == SlotState::Free || current == SlotState::Terminal) continue;
        const bool owned = current == SlotState::Completing || claim_live_slot(slot.state, current);
        if (owned) publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
    }
    publication_.clear();
}
LiveCompositor::CompositeSlot* LiveCompositor::reserve() noexcept { return reserve_live_slot(slots_.get(), slot_count_).slot; }
bool LiveCompositor::process_latest() {
    if (!running_.load(std::memory_order_acquire)) return false;
    DeviceFrameView source{};
    if (!fanout_.try_acquire_composite(&source)) return false;
    CompositeSlot* slot = reserve();
    if (slot == nullptr) {
        fanout_.release_composite(source.slot, source.ready, source.stream);
        if (analyzer_ != nullptr) static_cast<void>(analyzer_->discard(source.frame));
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    LiveAnalysisOverlayProjection analysis{};
    OverlayView manual_overlay{};
    auto scope = cuda_.scope();
    if (!scope) {
        fanout_.release_composite(source.slot, source.ready, source.stream);
        publish_slot(*slot, SlotState::Terminal);
        return false;
    }
    cudaError_t status = scope.Record(cudaStreamWaitEvent(slot->stream, source.ready, 0U));
    if (status == cudaSuccess) {
        const raster::CopyBgrToRgbaWork copy{.source_bgr = {reinterpret_cast<const std::uint8_t*>(source.pixels), source.pitch_bytes,
                                                            static_cast<int>(source.width), static_cast<int>(source.height)},
                                             .target_rgba = raster::pitched_rgba_target(reinterpret_cast<std::uint8_t*>(slot->rgba), slot->rgba_pitch,
                                                                                        static_cast<int>(source.width), static_cast<int>(source.height)),
                                             .stream = slot->stream};
        status = scope.Record(static_cast<cudaError_t>(raster::copy_bgr_to_rgba(copy)));
    }
    if (status == cudaSuccess && analyzer_ != nullptr && analyzer_->try_acquire(source.frame, &analysis)) {
        slot->analysis_slot = analysis.slot;
        status = scope.Record(cudaStreamWaitEvent(slot->stream, reinterpret_cast<cudaEvent_t>(analysis.completion.event), 0U));
        if (status == cudaSuccess)
            status = scope.Record(cudaMemset2DAsync(reinterpret_cast<void*>(slot->overlay), slot->overlay_pitch, 0, static_cast<std::size_t>(source.width) * 4U,
                                                    source.height, slot->stream));
        for (const auto& annotation : analysis.annotations) {
            const auto capacity = annotation.count.device_view() ? annotation.value_capacity : annotation.count.value();
            if (status != cudaSuccess || capacity == 0U) continue;
            const raster::InstanceOverlayRgbaWork overlay{
                .overlay = {reinterpret_cast<std::uint8_t*>(slot->overlay), slot->overlay_pitch, static_cast<int>(source.width),
                            static_cast<int>(source.height)},
                .instances = {reinterpret_cast<const float*>(annotation.boxes_xyxy.address),
                              reinterpret_cast<const std::uint8_t*>(annotation.colors_rgb.address),
                              reinterpret_cast<const int*>(annotation.class_references.address), static_cast<int>(capacity), annotation.count.device_view()},
                .masks = !annotation.masks_available ? nullptr : reinterpret_cast<const bool*>(annotation.masks.address),
                .mask_alpha = 115U,
                .box_thickness = 2,
                .stream = slot->stream};
            status = scope.Record(static_cast<cudaError_t>(raster::raster_instance_overlay_rgba(overlay)));
        }
        if (status == cudaSuccess) {
            const raster::CompositeRgbaWork composite{.base_rgba = raster::pitched_rgba_target(reinterpret_cast<std::uint8_t*>(slot->rgba), slot->rgba_pitch,
                                                                                               static_cast<int>(source.width), static_cast<int>(source.height)),
                                                      .overlay_rgba = {reinterpret_cast<const std::uint8_t*>(slot->overlay), slot->overlay_pitch,
                                                                       static_cast<int>(source.width), static_cast<int>(source.height)},
                                                      .stream = slot->stream};
            status = scope.Record(static_cast<cudaError_t>(raster::composite_rgba(composite)));
        }
    }
    if (status == cudaSuccess && manual_overlay_ != nullptr && manual_overlay_->try_acquire_latest(&manual_overlay)) {
        if (manual_overlay.has_content) {
            const std::uint64_t right = static_cast<std::uint64_t>(source.region.x) + source.width;
            const std::uint64_t bottom = static_cast<std::uint64_t>(source.region.y) + source.height;
            if (right > manual_overlay.width || bottom > manual_overlay.height) {
                status = scope.Record(cudaErrorInvalidValue);
            } else {
                status = scope.Record(cudaStreamWaitEvent(slot->stream, manual_overlay.ready, 0U));
            }
            if (status == cudaSuccess) {
                const CUdeviceptr manual_region = manual_overlay.rgba + static_cast<std::size_t>(source.region.y) * manual_overlay.pitch_bytes +
                                                  static_cast<std::size_t>(source.region.x) * 4U;
                const raster::CompositeRgbaWork composite{
                    .base_rgba = raster::pitched_rgba_target(reinterpret_cast<std::uint8_t*>(slot->rgba), slot->rgba_pitch, static_cast<int>(source.width),
                                                             static_cast<int>(source.height)),
                    .overlay_rgba = {reinterpret_cast<const std::uint8_t*>(manual_region), manual_overlay.pitch_bytes, static_cast<int>(source.width),
                                     static_cast<int>(source.height)},
                    .stream = slot->stream};
                status = scope.Record(static_cast<cudaError_t>(raster::composite_rgba(composite)));
            }
            slot->manual_overlay_slot = manual_overlay.slot;
        } else {
            manual_overlay_->release(manual_overlay.slot);
            manual_overlay = {};
        }
    }
    if (status == cudaSuccess) {
        slot->source_slot = source.slot;
        slot->metadata = DeviceFrameMetadata::From(source);
        status = scope.Record(cudaEventRecord(slot->ready, slot->stream));
    }
    if (status == cudaSuccess) status = scope.Record(cudaLaunchHostFunc(slot->stream, ProducerComplete, slot));
    if (status == cudaSuccess) return true;
    const bool synchronized = scope.Record(cudaStreamSynchronize(slot->stream)) == cudaSuccess;
    if (slot->analysis_slot.has_value() && analyzer_ != nullptr) {
        if (synchronized)
            analyzer_->release(*slot->analysis_slot);
        else
            analyzer_->terminalize(*slot->analysis_slot);
        slot->analysis_slot.reset();
    }
    if (slot->manual_overlay_slot.has_value() && manual_overlay_ != nullptr) {
        if (synchronized)
            manual_overlay_->release(*slot->manual_overlay_slot);
        else
            manual_overlay_->terminalize(*slot->manual_overlay_slot);
        slot->manual_overlay_slot.reset();
    }
    fanout_.release_composite(source.slot, synchronized ? nullptr : source.ready, nullptr);
    publish_slot(*slot, synchronized ? SlotState::Free : SlotState::Terminal);
    dropped_.fetch_add(1U, std::memory_order_relaxed);
    return false;
}
void CUDART_CB LiveCompositor::ProducerComplete(void* context) noexcept {
    auto& slot = *static_cast<CompositeSlot*>(context);
    slot.producer_complete.store(true, std::memory_order_release);
    slot.owner->completion_signal_.notify();
}
bool LiveCompositor::drain_completions() {
    bool progressed = false;
    for (std::uint32_t index = 0U; index < slot_count_; ++index) {
        CompositeSlot& slot = slots_[index];
        if (!slot.producer_complete.exchange(false, std::memory_order_acq_rel)) continue;
        complete(slot);
        progressed = true;
    }
    return progressed;
}
void LiveCompositor::complete(CompositeSlot& slot) noexcept {
    fanout_.release_composite(slot.source_slot, nullptr, nullptr);
    if (slot.analysis_slot.has_value() && analyzer_ != nullptr) {
        analyzer_->release(*slot.analysis_slot);
        slot.analysis_slot.reset();
    }
    if (slot.manual_overlay_slot.has_value() && manual_overlay_ != nullptr) {
        manual_overlay_->release(*slot.manual_overlay_slot);
        slot.manual_overlay_slot.reset();
    }
    if (!running_.load(std::memory_order_acquire)) {
        publish_slot(slot, SlotState::Free);
        return;
    }
    slot.metadata.ready_ns = system::steady_clock_now_ns();
    slot.revision = revision_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    publish_live_slot_state(slot.state, SlotState::Published);
    publication_.publish(PhysicalFrameRevision{slot.revision, slot.metadata.frame, slot.index, reinterpret_cast<std::uintptr_t>(slot.ready)});
    frames_.fetch_add(1U, std::memory_order_relaxed);
    revision_signal_.notify();
}
bool LiveCompositor::try_acquire(const PhysicalFrameRevision revision, LiveCompositeOutputLease* output, void* callback_owner,
                                 const LiveCompositeOutputLease::CompleteCallback on_complete, const LiveCompositeOutputLease::AbandonCallback abandon) {
    if (output == nullptr || static_cast<bool>(*output) || callback_owner == nullptr || on_complete == nullptr || abandon == nullptr || !revision.valid() ||
        revision.slot >= slot_count_)
        return false;
    CompositeSlot& slot = slots_[revision.slot];
    if (!transition_slot_state(slot.state, SlotState::Published, SlotState::Acquired)) return false;
    if (slot.revision != revision.revision || slot.metadata.frame != revision.frame || reinterpret_cast<std::uintptr_t>(slot.ready) != revision.ready_event) {
        publish_live_slot_state(slot.state, SlotState::Published);
        return false;
    }
    const DeviceFrameView view = slot.metadata.View(slot.index, slot.rgba, slot.rgba_pitch, slot.ready, slot.stream);
    *output = LiveCompositeOutputLease::Create(callback_owner, on_complete, abandon,
                                               {.pixels = static_cast<std::uintptr_t>(view.pixels),
                                                .pitch_bytes = view.pitch_bytes,
                                                .width = view.width,
                                                .height = view.height,
                                                .ready_event = reinterpret_cast<std::uintptr_t>(view.ready)},
                                               revision);
    return true;
}
void LiveCompositor::complete_output(const PhysicalFrameRevision frame_revision) noexcept { finish_output(frame_revision, true); }
void LiveCompositor::abandon_output(const PhysicalFrameRevision frame_revision) noexcept { finish_output(frame_revision, false); }
void LiveCompositor::finish_output(const PhysicalFrameRevision frame_revision, const bool completed) noexcept {
    if (!completed) close_admission();
    if (frame_revision.slot >= slot_count_) {
        cuda_.Record(cudaErrorInvalidResourceHandle);
        close_admission();
        completion_signal_.notify();
        return;
    }
    CompositeSlot& slot = slots_[frame_revision.slot];
    const bool identity_matches = slot.revision == frame_revision.revision && slot.metadata.frame == frame_revision.frame &&
                                  reinterpret_cast<std::uintptr_t>(slot.ready) == frame_revision.ready_event;
    if (!identity_matches) {
        cuda_.Record(cudaErrorInvalidResourceHandle);
        close_admission();
        completion_signal_.notify();
        if (claim_live_slot(slot.state, SlotState::Acquired)) publish_slot(slot, SlotState::Terminal);
        return;
    }
    if (!claim_live_slot(slot.state, SlotState::Acquired)) {
        cuda_.Record(cudaErrorInvalidResourceHandle);
        close_admission();
        completion_signal_.notify();
        return;
    }
    if (completed) {
        publish_slot(slot, SlotState::Free);
        completion_signal_.notify();
    } else {
        cuda_.Record(cudaErrorUnknown);
        completion_signal_.notify();
        publish_slot(slot, SlotState::Terminal);
    }
}
bool LiveCompositor::settled() const noexcept {
    for (std::uint32_t index = 0U; index < slot_count_; ++index)
        if (!slot_state_is(slots_[index].state, SlotState::Free) && !slot_state_is(slots_[index].state, SlotState::Terminal)) return false;
    return true;
}
void LiveCompositor::set_revision_listener(std::function<void()> listener) { revision_signal_.set_listener(std::move(listener)); }
void LiveCompositor::set_completion_listener(std::function<void()> listener) { completion_signal_.set_listener(std::move(listener)); }
std::optional<PhysicalFrameRevision> LiveCompositor::newest_revision() const noexcept {
    const PhysicalFrameRevision revision = publication_.snapshot();
    if (!revision.valid()) return std::nullopt;
    return revision;
}
LiveCompositorTelemetry LiveCompositor::status() const noexcept {
    return {.running = running_.load(std::memory_order_acquire),
            .frames_composited = frames_.load(std::memory_order_relaxed),
            .frames_dropped = dropped_.load(std::memory_order_relaxed),
            .front_revision = revision_.load(std::memory_order_relaxed)};
}
void LiveCompositor::destroy() noexcept {
    if (slots_ == nullptr) return;
    close_admission();
    auto scope = cuda_.scope();
    if (scope) {
        for (std::uint32_t index = 0; index < slot_count_; ++index) {
            CompositeSlot& slot = slots_[index];
            synchronize_live_cuda_stream(scope, slot.stream);
            destroy_live_cuda_event(scope, slot.ready);
            destroy_live_cuda_stream(scope, slot.stream);
            free_live_cuda_allocation(scope, slot.rgba);
            free_live_cuda_allocation(scope, slot.overlay);
        }
    }
    for (std::uint32_t index = 0; index < slot_count_; ++index) {
        CompositeSlot& slot = slots_[index];
        slot.ready = nullptr;
        slot.stream = nullptr;
        slot.rgba = 0U;
        slot.overlay = 0U;
        slot.rgba_pitch = 0U;
        slot.overlay_pitch = 0U;
    }
    slots_.reset();
}
}  // namespace mmltk::backend::media::live
