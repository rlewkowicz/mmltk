module;
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/execution_policy.h"
#include <cuda_runtime_api.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module mmltk.backend.media.capture.capture_session;

#include "detail/capture_session_impl.hpp"

namespace mmltk::backend::media::capture {

using capture_internal::CaptureSlotPhase;
using capture_internal::CaptureSlotPhaseValue;
using capture_internal::kBgr3V4l2PixelFormat;
using capture_internal::MakeErrnoStatus;
using capture_internal::MakeStatus;
using capture_internal::NowNs;

namespace {

void drain_event(const int fd) noexcept {
    std::uint64_t value = 0;
    while (fd >= 0) {
        const ssize_t result = ::read(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) continue;
        if (result < 0 && errno == EINTR) continue;
        return;
    }
}

[[nodiscard]] bool signal_event(const int fd) noexcept {
    const std::uint64_t value = 1U;
    while (fd >= 0) {
        const ssize_t result = ::write(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) return true;
        if (result < 0 && errno == EINTR) continue;
        return result < 0 && errno == EAGAIN;
    }
    return false;
}

}  // namespace

CaptureSession::Impl::CaptureReadyResult CaptureSession::Impl::WaitForCaptureReady() {
    if (camera_fault_.load(std::memory_order_acquire)) return CaptureReadyResult::kCameraError;
    pollfd descriptors[3]{
        {.fd = fd_, .events = static_cast<short>(POLLIN | POLLPRI), .revents = 0},
        {.fd = stop_event_fd_, .events = POLLIN, .revents = 0},
        {.fd = completion_event_fd_, .events = POLLIN, .revents = 0},
    };
    int result = 0;
    do {
        result = ::poll(descriptors, 3, -1);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        ReportCameraFault(MakeErrnoStatus(StatusCode::kInternalError, "poll capture descriptors").message);
        return CaptureReadyResult::kCameraError;
    }
    if ((descriptors[2].revents & POLLIN) != 0) {
        drain_event(completion_event_fd_);
        return CaptureReadyResult::kCompletionReady;
    }
    if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        drain_event(stop_event_fd_);
        return stop_requested_.load(std::memory_order_acquire) ? CaptureReadyResult::kStopRequested : CaptureReadyResult::kCameraError;
    }
    const short camera = descriptors[0].revents;
    if ((camera & (POLLHUP | POLLNVAL)) != 0) {
        ReportCameraFault("capture camera descriptor hung up");
        return CaptureReadyResult::kCameraHangup;
    }
    if ((camera & POLLERR) != 0) {
        ReportCameraFault("capture camera descriptor reported an error");
        return CaptureReadyResult::kCameraError;
    }
    return (camera & (POLLIN | POLLPRI)) != 0 ? CaptureReadyResult::kFrameReady : CaptureReadyResult::kCameraError;
}

CaptureSession::Impl::DequeueResult CaptureSession::Impl::TryDequeueBuffer(v4l2_buffer* const output, Status* const error) {
    if (output == nullptr) {
        if (error != nullptr) *error = MakeStatus(StatusCode::kInvalidArgument, "capture dequeue output is null");
        return DequeueResult::kCameraError;
    }
    *output = {};
    output->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    output->memory = V4L2_MEMORY_USERPTR;
    if (DeviceIoctl(VIDIOC_DQBUF, output) == -1) {
        if (errno == EAGAIN) return DequeueResult::kNotReady;
        const Status status = MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_DQBUF");
        if (error != nullptr) *error = status;
        ReportCameraFault(status.message);
        frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return DequeueResult::kCameraError;
    }
    dequeued_v4l2_buffers_.fetch_add(1U, std::memory_order_relaxed);
    bytes_captured_.fetch_add(output->bytesused, std::memory_order_relaxed);
    UpdateSequenceStats(output->sequence);
    return DequeueResult::kDequeued;
}

void CaptureSession::Impl::UpdateSequenceStats(const std::uint32_t sequence) {
    if (last_sequence_.has_value() && sequence > *last_sequence_ + 1U)
        sequence_gaps_.fetch_add(sequence - *last_sequence_ - 1U, std::memory_order_relaxed);
    last_sequence_ = sequence;
}

std::optional<std::uint32_t> CaptureSession::Impl::ResolveHostSlotIndex(const v4l2_buffer& buffer) const {
    if (buffer.index < host_slots_.size()) {
        const auto& slot = *host_slots_[buffer.index];
        if (reinterpret_cast<unsigned long>(slot.capture_buffer.data) == buffer.m.userptr && slot.capture_buffer.bytes == buffer.length)
            return buffer.index;
    }
    for (std::uint32_t index = 0; index < host_slots_.size(); ++index) {
        const auto& slot = *host_slots_[index];
        if (reinterpret_cast<unsigned long>(slot.capture_buffer.data) == buffer.m.userptr && slot.capture_buffer.bytes == buffer.length)
            return index;
    }
    return std::nullopt;
}

CaptureSession::Impl::CaptureLoopResult CaptureSession::Impl::CaptureLoop() {
    for (;;) {
        const CaptureReadyResult ready = WaitForCaptureReady();
        if (ready == CaptureReadyResult::kCompletionReady) {
            CaptureTeardownDisposition teardown = CaptureTeardownDisposition::kRequeueThenStreamOff;
            const Status requeued = RequeueCompletedSlots(&teardown);
            if (!requeued.ok()) return {.kind = CaptureStopKind::kOwnerFailure, .status = requeued, .teardown = teardown};
            continue;
        }
        if (ready == CaptureReadyResult::kStopRequested) return StopRequestedResult();
        if (ready == CaptureReadyResult::kCameraHangup)
            return {.kind = CaptureStopKind::kCameraHangup,
                    .status = MakeStatus(StatusCode::kNoDevice, last_error()),
                    .teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost};
        if (ready == CaptureReadyResult::kCameraError)
            return {.kind = CaptureStopKind::kCameraError,
                    .status = MakeStatus(StatusCode::kInternalError, last_error()),
                    .teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost};
        v4l2_buffer buffer{};
        Status error = Status::Ok();
        const DequeueResult dequeued = TryDequeueBuffer(&buffer, &error);
        if (dequeued == DequeueResult::kNotReady) continue;
        if (dequeued == DequeueResult::kCameraError)
            return {.kind = CaptureStopKind::kCameraError,
                    .status = std::move(error),
                    .teardown = CaptureTeardownDisposition::kNoStreamOrDeviceLost};
        const Status handled = HandleDequeuedBuffer(buffer);
        if (!handled.ok())
            return {.kind = camera_fault_.load(std::memory_order_acquire) ? CaptureStopKind::kCameraError : CaptureStopKind::kOwnerFailure,
                    .status = handled,
                    .teardown = camera_fault_.load(std::memory_order_acquire) ? CaptureTeardownDisposition::kNoStreamOrDeviceLost
                                                                              : CaptureTeardownDisposition::kOwnerRetainThenStreamOff};
    }
}

Status CaptureSession::Impl::HandleDequeuedBuffer(const v4l2_buffer& buffer) {
    const auto index = ResolveHostSlotIndex(buffer);
    if (!index.has_value()) {
        const Status failure = MakeStatus(StatusCode::kInternalError, "unable to match dequeued USERPTR buffer");
        ReportCameraFault(failure.message);
        frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return failure;
    }
    HostSlotRuntime& slot = *host_slots_[*index];
    if (slot.phase.load(std::memory_order_acquire) != CaptureSlotPhaseValue(CaptureSlotPhase::kHardwareQueued)) {
        const Status failure = MakeStatus(StatusCode::kInternalError, "dequeued USERPTR slot has conflicting custody");
        ReportCameraFault(failure.message);
        frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return failure;
    }
    slot.phase.store(CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending), std::memory_order_release);
    const std::size_t valid = std::min<std::size_t>(buffer.bytesused, size_image_);
    if (valid == 0U) {
        empty_frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return QueueV4l2Buffer(*index, true);
    }
    const bool short_frame = valid < size_image_;
    if (short_frame) {
        short_frames_.fetch_add(1U, std::memory_order_relaxed);
        ZeroFillShortFrame(slot, valid);
    }

    std::unique_lock lock(filled_mutex_);
    std::size_t active_slots = 0U;
    for (const auto& candidate : host_slots_) {
        const std::uint32_t phase = candidate->phase.load(std::memory_order_acquire);
        if (phase == CaptureSlotPhaseValue(CaptureSlotPhase::kH2dActive) ||
            phase == CaptureSlotPhaseValue(CaptureSlotPhase::kCompletionPending))
            ++active_slots;
    }
    if (replaceable_filled_index_ < 0 && active_slots != 0U && active_slots + 1U == host_slots_.size()) {
        lock.unlock();
        active_occupancy_drops_.fetch_add(1U, std::memory_order_relaxed);
        frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return QueueV4l2Buffer(*index, true);
    }
    if (replaceable_filled_index_ >= 0) {
        HostSlotRuntime& displaced = *host_slots_[static_cast<std::size_t>(replaceable_filled_index_)];
        if (displaced.phase.load(std::memory_order_acquire) == CaptureSlotPhaseValue(CaptureSlotPhase::kReplaceableFilled)) {
            displaced.phase.store(CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending), std::memory_order_release);
            const std::uint32_t displaced_index = displaced.slot_index;
            replaceable_filled_index_ = -1;
            lock.unlock();
            const Status queued = QueueV4l2Buffer(displaced_index, true);
            lock.lock();
            if (!queued.ok()) return queued;
            replaced_filled_frames_.fetch_add(1U, std::memory_order_relaxed);
            frames_dropped_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    slot.sequence = next_frame_id_++;
    slot.capture_ns = NowNs();
    slot.short_frame = short_frame;
    slot.region = CaptureRegion{.width = published_width_.load(std::memory_order_acquire),
                                .height = published_height_.load(std::memory_order_acquire)};
    slot.phase.store(CaptureSlotPhaseValue(CaptureSlotPhase::kReplaceableFilled), std::memory_order_release);
    replaceable_filled_index_ = static_cast<int>(*index);
    filled_frames_published_.fetch_add(1U, std::memory_order_relaxed);
    lock.unlock();
    NotifyFilledFramePublished();
    return Status::Ok();
}

void CaptureSession::Impl::ZeroFillShortFrame(HostSlotRuntime& slot, const std::size_t valid) const {
    auto* bytes = static_cast<std::uint8_t*>(slot.capture_buffer.data);
    std::memset(bytes + valid, 0, size_image_ - valid);
}

Status CaptureSession::Impl::RequeueCompletedSlots(CaptureTeardownDisposition* const teardown) {
    if (teardown == nullptr) return MakeStatus(StatusCode::kInvalidArgument, "capture teardown disposition is null");
    Status first_failure = Status::Ok();
    for (auto& slot : host_slots_) {
        if (slot->phase.load(std::memory_order_acquire) != CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending)) continue;
        if (*teardown != CaptureTeardownDisposition::kRequeueThenStreamOff) {
            slot->phase.store(CaptureSlotPhaseValue(CaptureSlotPhase::kOwnerRetained), std::memory_order_release);
            continue;
        }
        const Status status = QueueV4l2Buffer(slot->slot_index, true);
        if (!status.ok()) {
            first_failure = status;
            *teardown = CaptureTeardownDisposition::kOwnerRetainThenStreamOff;
        }
    }
    return first_failure;
}

Status CaptureSession::Impl::SettleSlotsBeforeTeardown(CaptureTeardownDisposition* const teardown) {
    if (teardown == nullptr) return MakeStatus(StatusCode::kInvalidArgument, "capture teardown disposition is null");
    Status first_failure = Status::Ok();
    {
        std::lock_guard lock(filled_mutex_);
        if (replaceable_filled_index_ >= 0) {
            const std::uint32_t index = static_cast<std::uint32_t>(replaceable_filled_index_);
            replaceable_filled_index_ = -1;
            host_slots_[index]->phase.store(CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending), std::memory_order_release);
        }
    }
    for (;;) {
        const Status requeued = RequeueCompletedSlots(teardown);
        if (first_failure.ok() && !requeued.ok()) first_failure = requeued;
        bool settled = true;
        for (const auto& slot : host_slots_) {
            const std::uint32_t phase = slot->phase.load(std::memory_order_acquire);
            if (phase != CaptureSlotPhaseValue(CaptureSlotPhase::kHardwareQueued) &&
                phase != CaptureSlotPhaseValue(CaptureSlotPhase::kOwnerRetained)) {
                settled = false;
                break;
            }
        }
        if (settled) return first_failure;
        pollfd completion{.fd = completion_event_fd_, .events = POLLIN, .revents = 0};
        int result = 0;
        do {
            result = ::poll(&completion, 1, -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0) return MakeErrnoStatus(StatusCode::kInternalError, "poll capture completion");
        drain_event(completion_event_fd_);
    }
}

Status CaptureSession::Impl::try_take_filled(FilledCaptureSlotLease* const output) {
    if (output == nullptr) return MakeStatus(StatusCode::kInvalidArgument, "output lease is null");
    if (output->valid()) return MakeStatus(StatusCode::kInvalidArgument, "output lease still owns a capture slot");
    std::lock_guard lock(filled_mutex_);
    if (replaceable_filled_index_ < 0) return MakeStatus(StatusCode::kNotReady, "no filled capture slot");
    HostSlotRuntime& slot = *host_slots_[static_cast<std::size_t>(replaceable_filled_index_)];
    std::uint32_t expected = CaptureSlotPhaseValue(CaptureSlotPhase::kReplaceableFilled);
    if (!slot.phase.compare_exchange_strong(expected, CaptureSlotPhaseValue(CaptureSlotPhase::kH2dActive), std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        return MakeStatus(StatusCode::kNotReady, "filled capture slot was replaced");
    replaceable_filled_index_ = -1;
    *output = CaptureSession::MakeFilledSlotLease(active_identity(), slot.slot_index, slot.sequence,
                                                  static_cast<const std::uint8_t*>(slot.capture_buffer.data), size_image_, bytes_per_line_,
                                                  kBgr3V4l2PixelFormat, slot.region, slot.capture_ns, slot.short_frame);
    h2d_frames_admitted_.fetch_add(1U, std::memory_order_relaxed);
    return Status::Ok();
}

Status CaptureSession::Impl::mark_h2d_completion_pending(const FilledCaptureSlotLease& lease) {
    if (!lease.valid() || lease.identity() != active_identity() || lease.slot() >= host_slots_.size())
        return {StatusCode::kInvalidArgument, {}};
    HostSlotRuntime& slot = *host_slots_[lease.slot()];
    if (slot.sequence != lease.sequence()) return {StatusCode::kInvalidArgument, {}};
    std::uint32_t expected = CaptureSlotPhaseValue(CaptureSlotPhase::kH2dActive);
    if (!slot.phase.compare_exchange_strong(expected, CaptureSlotPhaseValue(CaptureSlotPhase::kCompletionPending),
                                            std::memory_order_acq_rel, std::memory_order_acquire))
        return MakeStatus(StatusCode::kNotReady, "capture slot is not H2D active");
    return Status::Ok();
}

Status CaptureSession::Impl::return_after_h2d(FilledCaptureSlotLease&& lease) noexcept {
    if (!lease.valid() || lease.identity() != active_identity() || lease.slot() >= host_slots_.size())
        return MakeStatus(StatusCode::kInvalidArgument, "capture lease does not name the active slot");
    HostSlotRuntime& slot = *host_slots_[lease.slot()];
    if (slot.sequence != lease.sequence()) return MakeStatus(StatusCode::kInvalidArgument, "capture lease sequence is stale");
    std::uint32_t phase = slot.phase.load(std::memory_order_acquire);
    bool newly_completed = false;
    for (;;) {
        if (phase == CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending)) break;
        if (phase != CaptureSlotPhaseValue(CaptureSlotPhase::kH2dActive) &&
            phase != CaptureSlotPhaseValue(CaptureSlotPhase::kCompletionPending))
            return {StatusCode::kNotReady, {}};
        if (slot.phase.compare_exchange_weak(phase, CaptureSlotPhaseValue(CaptureSlotPhase::kRequeuePending), std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            newly_completed = true;
            break;
        }
    }
    CaptureSession::ConsumeFilledSlotLease(lease);
    if (newly_completed) h2d_frames_completed_.fetch_add(1U, std::memory_order_relaxed);
    if (!signal_event(completion_event_fd_)) {
        Status failure{StatusCode::kInternalError, "failed to signal completed capture slot"};
        static_cast<void>(report_failure(active_identity(), failure));
        return failure;
    }
    return Status::Ok();
}

void CaptureSession::Impl::RetainFirstFailure(const CaptureSessionIdentity identity, Status failure) noexcept {
    if (failure.ok()) return;
    std::lock_guard lock(failure_mutex_);
    if (!first_failure_.has_value()) first_failure_.emplace(PhysicalFailure{.identity = identity, .status = std::move(failure)});
}

Status CaptureSession::Impl::FirstFailure() const {
    std::lock_guard lock(failure_mutex_);
    return first_failure_.has_value() ? first_failure_->status : Status::Ok();
}

Status CaptureSession::Impl::report_failure(const CaptureSessionIdentity identity, Status failure) noexcept {
    if (!identity.valid() || identity != active_identity() || finalized_.load(std::memory_order_acquire))
        return MakeStatus(StatusCode::kInvalidArgument, "physical failure does not name the active capture owner");
    if (terminal_published_.load(std::memory_order_acquire))
        return MakeStatus(StatusCode::kNotReady, "capture terminal is already immutable");
    if (failure.ok()) failure = MakeStatus(StatusCode::kInternalError, "empty physical capture failure");
    RetainFirstFailure(identity, std::move(failure));
    stop_requested_.store(true, std::memory_order_release);
    CloseFilledAdmission();
    const bool stop_signaled = signal_event(stop_event_fd_);
    const bool completion_signaled = signal_event(completion_event_fd_);
    if (!stop_signaled && !completion_signaled) {
        Status wake_failure = MakeStatus(StatusCode::kInternalError, "failed to signal capture owner after physical failure");
        RetainFirstFailure(identity, wake_failure);
        return wake_failure;
    }
    return Status::Ok();
}

CaptureSession::Impl::CaptureLoopResult CaptureSession::Impl::StopRequestedResult() const {
    Status failure = FirstFailure();
    return !failure.ok() ? CaptureLoopResult{.kind = CaptureStopKind::kOwnerFailure,
                                             .status = std::move(failure),
                                             .teardown = CaptureTeardownDisposition::kRequeueThenStreamOff}
                         : CaptureLoopResult{.kind = CaptureStopKind::kRequested,
                                             .status = Status::Ok(),
                                             .teardown = CaptureTeardownDisposition::kRequeueThenStreamOff};
}

void CaptureSession::Impl::NotifyFilledFramePublished() {
    const auto listener = filled_frame_listener_.load(std::memory_order_acquire);
    if (listener != nullptr && *listener) {
        try {
            (*listener)();
        } catch (...) {}
    }
}

}  // namespace mmltk::backend::media::capture
