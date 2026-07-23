#include "capture_session_impl.hpp"
#include "rfdetr/cuda_utils.h"

#include <algorithm>
#include <cstring>

#include <sys/select.h>

namespace frameshow {

using capture_internal::InferenceState;
using capture_internal::MakeCudaStatus;
using capture_internal::MakeErrnoStatus;
using capture_internal::NowNs;
using capture_internal::PreviewState;
using capture_internal::ToStateValue;
using capture_internal::Xioctl;
using capture_internal::kBgr3BytesPerPixel;

bool CaptureSession::Impl::WaitForCaptureReady() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;

    const int select_rc = select(fd_ + 1, &fds, nullptr, nullptr, &timeout);
    if (select_rc == -1 && errno != EINTR) {
        SetLastError(MakeErrnoStatus(StatusCode::kInternalError, "select").message);
    }
    return select_rc > 0;
}

bool CaptureSession::Impl::TryDequeueBuffer(v4l2_buffer* out) {
    if (out == nullptr) {
        return false;
    }

    *out = {};
    out->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out->memory = V4L2_MEMORY_USERPTR;
    if (Xioctl(fd_, VIDIOC_DQBUF, out) == -1) {
        if (errno == EAGAIN) {
            return false;
        }
        SetLastError(MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_DQBUF").message);
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    dequeued_v4l2_buffers_.fetch_add(1, std::memory_order_relaxed);
    bytes_captured_.fetch_add(out->bytesused, std::memory_order_relaxed);
    UpdateSequenceStats(out->sequence);
    return true;
}

void CaptureSession::Impl::UpdateSequenceStats(std::uint32_t sequence) {
    if (last_sequence_.has_value() && sequence > (*last_sequence_ + 1U)) {
        sequence_gaps_.fetch_add(static_cast<std::uint64_t>(sequence - *last_sequence_ - 1U),
                                 std::memory_order_relaxed);
    }
    last_sequence_ = sequence;
}

std::optional<std::uint32_t> CaptureSession::Impl::ResolveHostSlotIndex(const v4l2_buffer& buf) const {
    if (buf.index < host_slots_.size()) {
        const HostSlotRuntime& slot = *host_slots_[buf.index];
        if (reinterpret_cast<unsigned long>(slot.capture_buffer.data) == buf.m.userptr &&
            slot.capture_buffer.bytes == buf.length) {
            return buf.index;
        }
    }

    for (std::uint32_t slot_index = 0; slot_index < host_slots_.size(); ++slot_index) {
        const HostSlotRuntime& slot = *host_slots_[slot_index];
        if (reinterpret_cast<unsigned long>(slot.capture_buffer.data) == buf.m.userptr &&
            slot.capture_buffer.bytes == buf.length) {
            return slot_index;
        }
    }
    return std::nullopt;
}

void CaptureSession::Impl::CaptureLoop() {
    while (!shutdown_.load(std::memory_order_acquire)) {
        RecycleDisplayedPreviewSlots();
        if (!WaitForCaptureReady()) {
            continue;
        }

        v4l2_buffer buf{};
        if (!TryDequeueBuffer(&buf)) {
            continue;
        }

        HandleDequeuedBuffer(buf);
    }
}

void CaptureSession::Impl::RecycleDisplayedPreviewSlots() {
    for (auto& slot : preview_slots_) {
        if (slot->state.load(std::memory_order_acquire) != ToStateValue(PreviewState::kDisplaying)) {
            continue;
        }
        if (!slot->displayed_event_pending.load(std::memory_order_acquire)) {
            continue;
        }
        const cudaError_t query_status = cudaEventQuery(slot->displayed_event);
        if (query_status == cudaSuccess) {
            slot->displayed_event_pending.store(false, std::memory_order_release);
            slot->state.store(ToStateValue(PreviewState::kFree), std::memory_order_release);
        } else if (query_status != cudaErrorNotReady) {
            SetLastError(MakeCudaStatus(query_status, "cudaEventQuery").message);
        }
    }
}

void CaptureSession::Impl::ReclaimStaleInferenceSlots() {
    const int latest_index = latest_inference_index_.load(std::memory_order_acquire);
    for (std::size_t slot_index = 0; slot_index < inference_slots_.size(); ++slot_index) {
        if (static_cast<int>(slot_index) == latest_index) {
            continue;
        }

        InferenceSlotRuntime& slot = *inference_slots_[slot_index];
        if (slot.state.load(std::memory_order_acquire) != ToStateValue(InferenceState::kPublished)) {
            continue;
        }

        const cudaError_t query_status = cudaStreamQuery(slot.copy_stream);
        if (query_status == cudaSuccess) {
            slot.state.store(ToStateValue(InferenceState::kFree), std::memory_order_release);
        } else if (query_status != cudaErrorNotReady) {
            SetLastError(MakeCudaStatus(query_status, "cudaStreamQuery").message);
        }
    }
}

std::optional<std::uint32_t> CaptureSession::Impl::ReserveInferenceSlot() {
    if (inference_slots_.empty()) {
        return std::nullopt;
    }

    ReclaimStaleInferenceSlots();
    const int latest_index = latest_inference_index_.load(std::memory_order_acquire);
    if (latest_index >= 0 && latest_index < static_cast<int>(inference_slots_.size())) {
        InferenceSlotRuntime& latest_slot = *inference_slots_[static_cast<std::size_t>(latest_index)];
        if (latest_slot.state.load(std::memory_order_acquire) == ToStateValue(InferenceState::kPublished)) {
            const cudaError_t latest_query = cudaStreamQuery(latest_slot.copy_stream);
            if (latest_query == cudaSuccess) {
                std::uint32_t expected = ToStateValue(InferenceState::kPublished);
                if (latest_slot.state.compare_exchange_strong(expected, ToStateValue(InferenceState::kWriting),
                                                              std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return static_cast<std::uint32_t>(latest_index);
                }
            }
            if (latest_query != cudaErrorNotReady) {
                SetLastError(MakeCudaStatus(latest_query, "cudaStreamQuery").message);
            }
        }
    }

    for (std::size_t attempt = 0; attempt < inference_slots_.size(); ++attempt) {
        const std::size_t candidate = (next_inference_slot_ + attempt) % inference_slots_.size();
        InferenceSlotRuntime& slot = *inference_slots_[candidate];
        if (slot.state.load(std::memory_order_acquire) != ToStateValue(InferenceState::kFree)) {
            continue;
        }

        const cudaError_t query_status = cudaStreamQuery(slot.copy_stream);
        if (query_status == cudaSuccess) {
            std::uint32_t expected = ToStateValue(InferenceState::kFree);
            if (slot.state.compare_exchange_strong(expected, ToStateValue(InferenceState::kWriting),
                                                   std::memory_order_acq_rel, std::memory_order_acquire)) {
                next_inference_slot_ = (candidate + 1U) % inference_slots_.size();
                return static_cast<std::uint32_t>(candidate);
            }
        }
        if (query_status != cudaErrorNotReady) {
            SetLastError(MakeCudaStatus(query_status, "cudaStreamQuery").message);
        }
    }

    return std::nullopt;
}

std::optional<std::uint32_t> CaptureSession::Impl::ReservePreviewSlot() {
    if (preview_slots_.empty()) {
        return std::nullopt;
    }

    RecycleDisplayedPreviewSlots();
    const int latest_index = latest_preview_index_.load(std::memory_order_acquire);
    for (std::size_t attempt = 0; attempt < preview_slots_.size(); ++attempt) {
        const std::size_t candidate = (next_preview_slot_ + attempt) % preview_slots_.size();
        PreviewSlotRuntime& slot = *preview_slots_[candidate];
        const std::uint32_t state = slot.state.load(std::memory_order_acquire);
        if (state == ToStateValue(PreviewState::kFree) ||
            (state == ToStateValue(PreviewState::kPublished) && latest_index != static_cast<int>(candidate))) {
            next_preview_slot_ = (candidate + 1U) % preview_slots_.size();
            return static_cast<std::uint32_t>(candidate);
        }
    }
    return std::nullopt;
}

void CaptureSession::Impl::HandleDequeuedBuffer(const v4l2_buffer& buf) {
    const auto host_slot_index = ResolveHostSlotIndex(buf);
    if (!host_slot_index.has_value()) {
        SetLastError("unable to match dequeued USERPTR buffer");
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    HostSlotRuntime& host_slot = *host_slots_[*host_slot_index];
    const std::size_t valid_bytes = std::min<std::size_t>(buf.bytesused, size_image_);
    if (valid_bytes == 0U) {
        empty_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        const Status status = QueueV4l2Buffer(*host_slot_index, true);
        if (!status.ok()) {
            SetLastError(status.message);
        }
        return;
    }

    const bool short_frame = valid_bytes < size_image_;
    if (short_frame) {
        short_frames_.fetch_add(1, std::memory_order_relaxed);
        ZeroFillShortFrame(host_slot, valid_bytes);
    }

    const auto inference_slot_index = ReserveInferenceSlot();
    if (!inference_slot_index.has_value()) {
        inference_backpressure_drops_.fetch_add(1, std::memory_order_relaxed);
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        const Status status = QueueV4l2Buffer(*host_slot_index, true);
        if (!status.ok()) {
            SetLastError(status.message);
        }
        return;
    }

    const CaptureRegion region = snapshot_capture_region();
    const std::uint64_t capture_ns = NowNs();
    InferenceSlotRuntime& inference_slot = *inference_slots_[*inference_slot_index];
    const Status status = ScheduleInferenceCopy(host_slot, inference_slot, region, capture_ns, short_frame);
    if (!status.ok()) {
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        inference_slot.state.store(ToStateValue(InferenceState::kFree), std::memory_order_release);
        SetLastError(status.message);
        const Status requeue_status = QueueV4l2Buffer(*host_slot_index, true);
        if (!requeue_status.ok()) {
            SetLastError(requeue_status.message);
        }
        return;
    }

    const Status preview_status = TrySchedulePreviewTap(inference_slot);
    if (!preview_status.ok()) {
        SetLastError(preview_status.message);
    }
}

void CaptureSession::Impl::ZeroFillShortFrame(HostSlotRuntime& host_slot, std::size_t valid_bytes) const {
    auto* capture_ptr = static_cast<std::uint8_t*>(host_slot.capture_buffer.data);
    std::memset(capture_ptr + valid_bytes, 0, size_image_ - valid_bytes);
}

Status CaptureSession::Impl::ScheduleInferenceCopy(HostSlotRuntime& host_slot, InferenceSlotRuntime& inference_slot,
                                                   const CaptureRegion& region, std::uint64_t capture_ns,
                                                   bool short_frame) {
    (void)region;
    auto* capture_ptr = static_cast<std::uint8_t*>(host_slot.capture_buffer.data);
    auto* src = capture_ptr;
    const CaptureRegion full_region{
        0U,
        0U,
        config.width,
        config.height,
    };
    const std::size_t copy_width_bytes = static_cast<std::size_t>(full_region.width) * kBgr3BytesPerPixel;

    inference_slot.frame_id = next_frame_id_++;
    inference_slot.capture_ns = capture_ns;
    inference_slot.short_frame = short_frame;
    inference_slot.region = full_region;

    const cudaError_t copy_status =
        cudaMemcpy2DAsync(inference_slot.device_ptr, inference_slot.pitch_bytes, src, bytes_per_line_, copy_width_bytes,
                          full_region.height, cudaMemcpyHostToDevice, inference_slot.copy_stream);
    if (copy_status != cudaSuccess) {
        return MakeCudaStatus(copy_status, "cudaMemcpy2DAsync");
    }

    const cudaError_t flip_status = mmltk::rfdetr::launch_bgr_vertical_flip_in_place_pitched(
        inference_slot.device_ptr, inference_slot.pitch_bytes, full_region.width, full_region.height,
        inference_slot.copy_stream);
    if (flip_status != cudaSuccess) {
        return MakeCudaStatus(flip_status, "launch_bgr_vertical_flip_in_place_pitched");
    }

    const cudaError_t ready_status = cudaEventRecord(inference_slot.ready_event, inference_slot.copy_stream);
    if (ready_status != cudaSuccess) {
        return MakeCudaStatus(ready_status, "cudaEventRecord");
    }

    const cudaError_t callback_status =
        cudaLaunchHostFunc(inference_slot.copy_stream, RequeueCapturedBufferThunk, &host_slot.requeue_context);
    if (callback_status != cudaSuccess) {
        return MakeCudaStatus(callback_status, "cudaLaunchHostFunc");
    }

    inference_slot.ready_ns = NowNs();
    inference_slot.state.store(ToStateValue(InferenceState::kPublished), std::memory_order_release);
    latest_inference_index_.store(static_cast<int>(inference_slot.slot_index), std::memory_order_release);
    inference_frames_published_.fetch_add(1, std::memory_order_relaxed);
    NotifyInferenceFramePublished();
    return Status::Ok();
}

Status CaptureSession::Impl::TrySchedulePreviewTap(const InferenceSlotRuntime& inference_slot) {
    if (preview_slots_.empty()) {
        return Status::Ok();
    }

    const auto preview_slot_index = ReservePreviewSlot();
    if (!preview_slot_index.has_value()) {
        preview_backpressure_drops_.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok();
    }

    PreviewSlotRuntime& preview_slot = *preview_slots_[*preview_slot_index];
    preview_slot.frame_id = inference_slot.frame_id;
    preview_slot.capture_ns = inference_slot.capture_ns;
    preview_slot.short_frame = inference_slot.short_frame;
    preview_slot.region = inference_slot.region;

    const cudaError_t wait_status = cudaStreamWaitEvent(preview_slot.copy_stream, inference_slot.ready_event, 0);
    if (wait_status != cudaSuccess) {
        preview_slot.state.store(ToStateValue(PreviewState::kFree), std::memory_order_release);
        return MakeCudaStatus(wait_status, "cudaStreamWaitEvent");
    }

    const std::size_t copy_width_bytes = static_cast<std::size_t>(preview_slot.region.width) * kBgr3BytesPerPixel;
    const cudaError_t copy_status = cudaMemcpy2DAsync(
        preview_slot.device_ptr, preview_slot.pitch_bytes, inference_slot.device_ptr, inference_slot.pitch_bytes,
        copy_width_bytes, preview_slot.region.height, cudaMemcpyDeviceToDevice, preview_slot.copy_stream);
    if (copy_status != cudaSuccess) {
        preview_slot.state.store(ToStateValue(PreviewState::kFree), std::memory_order_release);
        return MakeCudaStatus(copy_status, "cudaMemcpy2DAsync");
    }

    const cudaError_t ready_status = cudaEventRecord(preview_slot.ready_event, preview_slot.copy_stream);
    if (ready_status != cudaSuccess) {
        preview_slot.state.store(ToStateValue(PreviewState::kFree), std::memory_order_release);
        return MakeCudaStatus(ready_status, "cudaEventRecord");
    }

    preview_slot.ready_ns = NowNs();
    preview_slot.displayed_event_pending.store(false, std::memory_order_release);
    preview_slot.state.store(ToStateValue(PreviewState::kPublished), std::memory_order_release);
    latest_preview_index_.store(static_cast<int>(preview_slot.slot_index), std::memory_order_release);
    preview_frames_published_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
}

void CaptureSession::Impl::OnPreviewCopyComplete(std::uint32_t host_slot_index) {
    if (shutdown_.load(std::memory_order_acquire) || !streaming_ || fd_ == -1) {
        return;
    }

    const Status status = QueueV4l2Buffer(host_slot_index, true);
    if (!status.ok()) {
        SetLastError(status.message);
    }
}

void CaptureSession::Impl::NotifyInferenceFramePublished() {
    {
        std::lock_guard<std::mutex> lock(inference_publish_mutex_);
        ++inference_publish_generation_;
    }
    inference_publish_cv_.notify_all();
}

}  
