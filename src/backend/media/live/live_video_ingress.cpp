// CLEANUP-IGNORE: This independent module implementation declares the exact global-fragment and module dependencies it
// consumes.
module;
#include <cuda_runtime_api.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "detail/live_module_dependencies.h"  // IWYU pragma: keep
#include "src/common/system/time_utils.h"

module mmltk.backend.media.live.live_session_controller;

import mmltk.backend.media.live.live_capture_region;
import mmltk.backend.media.live.live_frame_id;
import mmltk.backend.media.capture.capture_session;
import mmltk.backend.media.capture.capture_types;
import mmltk.backend.media.capture.live_video_source;
import mmltk.backend.media.capture.status;

#include "detail/live_video_ingress.h"
namespace mmltk::backend::media::live {
namespace system = mmltk::common::system;

LiveVideoIngress::LiveVideoIngress(capture::CaptureConfig config, const std::uint32_t count, LivePhysicalCudaContext cuda)
    : config_(std::move(config)),
      cuda_(std::move(cuda)),
      capture_(config_),
      slots_(count == 0U ? nullptr : std::make_unique<DeviceSlot[]>(count)),
      slot_count_(count) {
    if (count == 0U || !cuda_.valid()) throw std::invalid_argument("Live ingress requires fixed slots and a CUDA owner");
    try {
        auto scope = cuda_.scope();
        if (!scope) throw std::runtime_error("enter Live ingress CUDA scope");
        for (std::uint32_t index = 0; index < count; ++index) {
            DeviceSlot& slot = slots_[index];
            slot.owner = this;
            slot.index = index;
            if (scope.Record(cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking)) != cudaSuccess)
                throw std::runtime_error("create Live ingress stream");
            if (scope.Record(cudaEventCreateWithFlags(&slot.ready, cudaEventDisableTiming)) != cudaSuccess)
                throw std::runtime_error("create Live ingress event");
            if (scope.Record(cudaMallocPitch(reinterpret_cast<void**>(&slot.pixels), &slot.pitch_bytes,
                                             static_cast<std::size_t>(config_.width) * 3U, config_.height)) != cudaSuccess)
                throw std::runtime_error("allocate Live ingress slot");
        }
    } catch (...) {
        release_resources();
        throw;
    }
    capture_.set_filled_frame_listener([this] { ingestion_signal_.notify(); });
    capture_.set_state_listener([this] { ingestion_signal_.notify(); });
}

LiveVideoIngress::~LiveVideoIngress() { release_resources(); }

void LiveVideoIngress::ScrubProduct(DeviceSlot& slot) noexcept {
    slot.lease = {};
    slot.metadata = {};
}

void LiveVideoIngress::publish_slot(DeviceSlot& slot, const SlotState published) noexcept {
    publish_latest_live_owner_slot(latest_, slot.index, slot.state, published, [&slot] noexcept { ScrubProduct(slot); });
}

capture::CaptureSessionStartResult LiveVideoIngress::start() {
    std::lock_guard lock(lifecycle_);
    auto result = capture_.start();
    if (result.has_custody()) identity_ = result.identity;
    running_.store(result.running(), std::memory_order_release);
    return result;
}

capture::Status LiveVideoIngress::request_stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (!identity_.valid()) return {capture::StatusCode::kNotRunning, "Live ingress has no Capture custody"};
    return capture_.request_stop(identity_);
}

std::shared_ptr<const capture::CaptureStopTerminal> LiveVideoIngress::try_take_terminal() noexcept {
    return capture_.try_take_stop_terminal();
}

capture::Status LiveVideoIngress::finalize_terminal(const std::shared_ptr<const capture::CaptureStopTerminal>& terminal) {
    return capture_.finalize_stop(terminal);
}

capture::Status LiveVideoIngress::report_failure(capture::Status failure) noexcept {
    running_.store(false, std::memory_order_release);
    if (!identity_.valid()) return failure;
    return capture_.report_failure(identity_, std::move(failure));
}

void LiveVideoIngress::settle() noexcept {
    running_.store(false, std::memory_order_release);
    latest_.store(-1, std::memory_order_release);
    auto scope = cuda_.scope();
    retire_live_slots(scope, slots_.get(), slot_count_,
                      [this](DeviceSlot& slot, const SlotState published) noexcept { publish_slot(slot, published); });
}

LiveVideoIngress::DeviceSlot* LiveVideoIngress::reserve() noexcept { return reserve_live_slot(slots_.get(), slot_count_, &latest_).slot; }

bool LiveVideoIngress::ingest_next() {
    if (!running()) return false;
    capture::FilledCaptureSlotLease lease;
    const capture::Status status = capture_.try_take_filled(&lease);
    if (!status.ok()) {
        if (status.code != capture::StatusCode::kNotReady) publish_failure(status);
        return false;
    }
    DeviceSlot* slot = reserve();
    if (slot == nullptr) {
        const capture::Status returned = capture_.return_after_h2d(std::move(lease));
        if (!returned.ok()) publish_failure(returned);
        return true;
    }

    slot->metadata = DeviceFrameMetadata::Captured(lease);
    slot->lease = std::move(lease);

    auto scope = cuda_.scope();
    if (!scope) {
        fail_upload(*slot, cudaErrorNotPermitted);
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(slot->metadata.region.width) * 3U;
    const std::uint8_t* const source = slot->lease.data() + static_cast<std::size_t>(slot->metadata.region.y) * slot->lease.stride_bytes() +
                                       static_cast<std::size_t>(slot->metadata.region.x) * 3U;
    cudaError_t failure =
        scope.Record(cudaMemcpy2DAsync(reinterpret_cast<void*>(slot->pixels), slot->pitch_bytes, source, slot->lease.stride_bytes(),
                                       row_bytes, slot->metadata.region.height, cudaMemcpyHostToDevice, slot->stream));
    if (failure == cudaSuccess) failure = scope.Record(cudaEventRecord(slot->ready, slot->stream));
    capture::Status capture_failure = capture::Status::Ok();
    if (failure == cudaSuccess) capture_failure = capture_.mark_h2d_completion_pending(slot->lease);
    if (failure == cudaSuccess && capture_failure.ok()) failure = scope.Record(cudaLaunchHostFunc(slot->stream, CompleteSlot, slot));
    if (failure != cudaSuccess || !capture_failure.ok()) {
        fail_upload(*slot, failure, std::move(capture_failure));
        return false;
    }
    return true;
}

void CUDART_CB LiveVideoIngress::CompleteSlot(void* context) noexcept {
    auto& slot = *static_cast<DeviceSlot*>(context);
    slot.owner->complete(slot);
}

void LiveVideoIngress::complete(DeviceSlot& slot) noexcept {
    slot.metadata.ready_ns = system::steady_clock_now_ns();
    const capture::Status returned = capture_.return_after_h2d(std::move(slot.lease));
    if (!returned.ok()) publish_failure(returned);
    if (returned.ok() && running_.load(std::memory_order_acquire)) {
        publish_live_slot_state(slot.state, SlotState::Published);
        latest_.store(static_cast<int>(slot.index), std::memory_order_release);
        ready_signal_.notify();
        return;
    }
    publish_slot(slot, SlotState::Free);
}

void LiveVideoIngress::fail_upload(DeviceSlot& slot, const cudaError_t failure, capture::Status capture_failure) noexcept {
    auto scope = cuda_.scope();
    bool synchronized = static_cast<bool>(scope);
    if (scope) {
        if (failure != cudaSuccess) static_cast<void>(scope.Record(failure));
        synchronized = scope.Record(cudaStreamSynchronize(slot.stream)) == cudaSuccess;
    }
    const capture::Status returned = capture_.return_after_h2d(std::move(slot.lease));
    if (!capture_failure.ok()) publish_failure(std::move(capture_failure));
    if (!returned.ok()) publish_failure(returned);
    publish_slot(slot, synchronized ? SlotState::Free : SlotState::Terminal);
}

bool LiveVideoIngress::try_acquire_latest(DeviceFrameView* output) {
    if (output == nullptr) return false;
    const int index = latest_.load(std::memory_order_acquire);
    if (index < 0 || index >= static_cast<int>(slot_count_)) return false;
    DeviceSlot& slot = slots_[static_cast<std::uint32_t>(index)];
    if (!transition_slot_state(slot.state, SlotState::Published, SlotState::Acquired)) return false;
    *output = slot.metadata.View(slot.index, slot.pixels, slot.pitch_bytes, slot.ready, slot.stream);
    return true;
}

void LiveVideoIngress::release(const std::uint32_t index) noexcept {
    if (index >= slot_count_) {
        publish_failure({capture::StatusCode::kInternalError, "Live ingress release slot is out of range"});
        return;
    }
    DeviceSlot& slot = slots_[index];
    if (!claim_live_slot(slot.state, SlotState::Acquired)) {
        publish_failure({capture::StatusCode::kInternalError, "Live ingress release lost exact-slot custody"});
        return;
    }
    publish_slot(slot, SlotState::Free);
}

void LiveVideoIngress::terminalize(const std::uint32_t index) noexcept {
    if (index >= slot_count_) {
        publish_failure({capture::StatusCode::kInternalError, "Live ingress terminal slot is out of range"});
        return;
    }
    DeviceSlot& slot = slots_[index];
    if (!claim_live_slot(slot.state, SlotState::Acquired)) {
        publish_failure({capture::StatusCode::kInternalError, "Live ingress terminalization lost exact-slot custody"});
        return;
    }
    publish_slot(slot, SlotState::Terminal);
}

void LiveVideoIngress::set_ready_listener(std::function<void()> listener) { ready_signal_.set_listener(std::move(listener)); }

void LiveVideoIngress::set_ingestion_wake(std::function<void()> listener) { ingestion_signal_.set_listener(std::move(listener)); }

void LiveVideoIngress::set_failure_listener(FailureListener listener) {
    if (!listener) {
        failure_listener_.store(nullptr, std::memory_order_release);
        return;
    }
    failure_listener_.store(std::make_shared<const FailureListener>(std::move(listener)), std::memory_order_release);
}

void LiveVideoIngress::publish_failure(capture::Status failure) const noexcept {
    if (failure.ok()) return;
    const auto listener = failure_listener_.load(std::memory_order_acquire);
    if (listener == nullptr || !*listener) return;
    try {
        (*listener)(std::move(failure));
    } catch (...) {}
}

bool LiveVideoIngress::running() const noexcept { return running_.load(std::memory_order_acquire); }

capture::CaptureStats LiveVideoIngress::snapshot_stats() const { return capture_.snapshot_stats(); }

void LiveVideoIngress::release_resources() noexcept {
    running_.store(false, std::memory_order_release);
    if (slots_ == nullptr) return;
    auto scope = cuda_.scope();
    for (std::uint32_t index = 0; index < slot_count_; ++index) {
        DeviceSlot& slot = slots_[index];
        if (scope) synchronize_live_cuda_stream(scope, slot.stream);
        if (slot.lease.valid()) {
            const capture::Status returned = capture_.return_after_h2d(std::move(slot.lease));
            if (!returned.ok()) publish_failure(returned);
        }
        if (scope) {
            destroy_live_cuda_event(scope, slot.ready);
            destroy_live_cuda_stream(scope, slot.stream);
            free_live_cuda_allocation(scope, slot.pixels);
        }
        slot.ready = nullptr;
        slot.stream = nullptr;
        slot.pixels = 0U;
        slot.pitch_bytes = 0U;
        slot.metadata = {};
    }
    slots_.reset();
    latest_.store(-1, std::memory_order_release);
}
}  // namespace mmltk::backend::media::live
