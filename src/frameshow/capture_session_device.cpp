#include "capture_session_impl.hpp"

#include <algorithm>
#include <limits>

#include <fcntl.h>
#include <unistd.h>

namespace frameshow {

using capture_internal::AllocateHostBuffer;
using capture_internal::FreeHostBuffer;
using capture_internal::MakeCudaStatus;
using capture_internal::MakeErrnoStatus;
using capture_internal::MakeStatus;
using capture_internal::Xioctl;
using capture_internal::kBgr3BytesPerPixel;
using capture_internal::kBgr3V4l2PixelFormat;

template <bool CreateDisplayedEvent, typename SlotRuntime>
Status AllocateDeviceSlotResources(SlotRuntime& slot, std::size_t frame_width_bytes, std::uint32_t height) {
    void* device_allocation = nullptr;
    cudaError_t cuda_status = cudaMallocPitch(&device_allocation, &slot.pitch_bytes, frame_width_bytes, height);
    if (cuda_status != cudaSuccess) {
        return MakeCudaStatus(cuda_status, "cudaMallocPitch");
    }
    slot.device_ptr = static_cast<std::uint8_t*>(device_allocation);

    cuda_status = cudaStreamCreateWithFlags(&slot.copy_stream, cudaStreamNonBlocking);
    if (cuda_status != cudaSuccess) {
        return MakeCudaStatus(cuda_status, "cudaStreamCreateWithFlags");
    }

    cuda_status = cudaEventCreateWithFlags(&slot.ready_event, cudaEventDisableTiming);
    if (cuda_status != cudaSuccess) {
        return MakeCudaStatus(cuda_status, "cudaEventCreateWithFlags");
    }
    if constexpr (CreateDisplayedEvent) {
        cuda_status = cudaEventCreateWithFlags(&slot.displayed_event, cudaEventDisableTiming);
        if (cuda_status != cudaSuccess) {
            return MakeCudaStatus(cuda_status, "cudaEventCreateWithFlags");
        }
    }
    return Status::Ok();
}

template <bool HasDisplayedEvent, typename SlotContainer>
void DestroyDeviceSlots(SlotContainer& slots) {
    for (auto& slot : slots) {
        if (slot->ready_event != nullptr) {
            cudaEventDestroy(slot->ready_event);
            slot->ready_event = nullptr;
        }
        if constexpr (HasDisplayedEvent) {
            if (slot->displayed_event != nullptr) {
                cudaEventDestroy(slot->displayed_event);
                slot->displayed_event = nullptr;
            }
        }
        if (slot->copy_stream != nullptr) {
            cudaStreamDestroy(slot->copy_stream);
            slot->copy_stream = nullptr;
        }
        if (slot->device_ptr != nullptr) {
            cudaFree(slot->device_ptr);
            slot->device_ptr = nullptr;
        }
    }
    slots.clear();
}

Status CaptureSession::Impl::InitializeCuda() {
    const cudaError_t set_device_status = cudaSetDevice(config.cuda_device_index);
    if (set_device_status != cudaSuccess) {
        return MakeCudaStatus(set_device_status, "cudaSetDevice");
    }

    const cudaError_t init_status = cudaFree(nullptr);
    if (init_status != cudaSuccess) {
        return MakeCudaStatus(init_status, "cuda runtime initialization");
    }
    return Status::Ok();
}

Status CaptureSession::Impl::OpenDevice() {
    fd_ = open(config.device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        return MakeErrnoStatus(StatusCode::kNoDevice, "open");
    }
    return Status::Ok();
}

void CaptureSession::Impl::CloseDevice() {
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

Status CaptureSession::Impl::ConfigureDevice() {
    Status status = ConfigureCaptureFormat();
    if (!status.ok()) {
        return status;
    }

    return ConfigureFrameRateAndBuffers();
}

Status CaptureSession::Impl::ConfigureCaptureFormat() {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config.width;
    format.fmt.pix.height = config.height;
    format.fmt.pix.pixelformat = kBgr3V4l2PixelFormat;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (Xioctl(fd_, VIDIOC_S_FMT, &format) == -1) {
        return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_S_FMT");
    }
    if (format.fmt.pix.pixelformat != kBgr3V4l2PixelFormat) {
        return MakeStatus(StatusCode::kUnsupported, "driver negotiated a different pixel format than requested BGR3");
    }

    config.width = format.fmt.pix.width;
    config.height = format.fmt.pix.height;
    const std::size_t minimum_line_bytes = static_cast<std::size_t>(config.width) * kBgr3BytesPerPixel;
    bytes_per_line_ = std::max<std::size_t>(format.fmt.pix.bytesperline, minimum_line_bytes);
    size_image_ =
        std::max<std::size_t>(format.fmt.pix.sizeimage, bytes_per_line_ * static_cast<std::size_t>(config.height));
    return Status::Ok();
}

Status CaptureSession::Impl::ConfigureFrameRateAndBuffers() {
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = config.fps;
    if (Xioctl(fd_, VIDIOC_S_PARM, &parm) == -1) {
        return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_S_PARM");
    }

    v4l2_requestbuffers req{};
    req.count = config.v4l2_buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (Xioctl(fd_, VIDIOC_REQBUFS, &req) == -1) {
        return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_REQBUFS");
    }
    if (req.count == 0U) {
        return MakeStatus(StatusCode::kUnsupported, "device returned zero USERPTR buffers");
    }
    actual_v4l2_buffer_count_ = req.count;
    return Status::Ok();
}

Status CaptureSession::Impl::StartStreaming() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (Xioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
        Status status = MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_STREAMON");
        SetLastError(status.message);
        return status;
    }
    streaming_ = true;
    return Status::Ok();
}

void CaptureSession::Impl::StopStreaming() {
    if (!streaming_ || fd_ == -1) {
        return;
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
}

Status CaptureSession::Impl::AllocateHostSlots() {
    host_slots_.clear();
    host_slots_.reserve(actual_v4l2_buffer_count_);

    for (std::uint32_t slot_index = 0; slot_index < actual_v4l2_buffer_count_; ++slot_index) {
        auto slot = std::make_unique<HostSlotRuntime>();
        slot->slot_index = slot_index;
        slot->requeue_context.owner = this;
        slot->requeue_context.slot_index = slot_index;

        Status status = AllocateHostBuffer(size_image_, true, &slot->capture_buffer);
        if (!status.ok()) {
            return status;
        }

        host_slots_.push_back(std::move(slot));
    }
    return Status::Ok();
}

void CaptureSession::Impl::DestroyHostSlots() {
    for (auto& slot : host_slots_) {
        FreeHostBuffer(&slot->capture_buffer);
    }
    host_slots_.clear();
}

Status CaptureSession::Impl::AllocateInferenceSlots() {
    inference_slots_.clear();
    inference_slots_.reserve(actual_v4l2_buffer_count_);
    next_inference_slot_ = 0;

    const std::size_t frame_width_bytes = static_cast<std::size_t>(config.width) * kBgr3BytesPerPixel;
    for (std::uint32_t slot_index = 0; slot_index < actual_v4l2_buffer_count_; ++slot_index) {
        auto slot = std::make_unique<InferenceSlotRuntime>();
        slot->slot_index = slot_index;

        Status status = AllocateDeviceSlotResources<false>(*slot, frame_width_bytes, config.height);
        if (!status.ok()) {
            return status;
        }

        inference_slots_.push_back(std::move(slot));
    }
    return Status::Ok();
}

void CaptureSession::Impl::DestroyInferenceSlots() {
    DestroyDeviceSlots<false>(inference_slots_);
}

Status CaptureSession::Impl::AllocatePreviewSlots() {
    preview_slots_.clear();
    preview_slots_.reserve(config.preview_buffer_count);
    next_preview_slot_ = 0;

    if (config.preview_buffer_count == 0U) {
        return Status::Ok();
    }

    const std::size_t preview_width_bytes = static_cast<std::size_t>(config.width) * kBgr3BytesPerPixel;
    for (std::uint32_t slot_index = 0; slot_index < config.preview_buffer_count; ++slot_index) {
        auto slot = std::make_unique<PreviewSlotRuntime>();
        slot->slot_index = slot_index;

        Status status = AllocateDeviceSlotResources<true>(*slot, preview_width_bytes, config.height);
        if (!status.ok()) {
            return status;
        }

        preview_slots_.push_back(std::move(slot));
    }
    return Status::Ok();
}

void CaptureSession::Impl::DestroyPreviewSlots() {
    DestroyDeviceSlots<true>(preview_slots_);
}

Status CaptureSession::Impl::QueueAllV4l2Buffers() {
    for (std::uint32_t slot_index = 0; slot_index < host_slots_.size(); ++slot_index) {
        Status status = QueueV4l2Buffer(slot_index, false);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status CaptureSession::Impl::QueueV4l2Buffer(std::uint32_t slot_index, bool record_requeue_failure) {
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = slot_index;
    buf.m.userptr = reinterpret_cast<unsigned long>(host_slots_[slot_index]->capture_buffer.data);
    const std::size_t capture_buffer_bytes = host_slots_[slot_index]->capture_buffer.bytes;
    if (capture_buffer_bytes > std::numeric_limits<decltype(buf.length)>::max()) {
        return MakeStatus(StatusCode::kInvalidArgument, "capture buffer is too large for V4L2 USERPTR queueing");
    }
    buf.length = static_cast<decltype(buf.length)>(capture_buffer_bytes);
    if (Xioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
        if (record_requeue_failure) {
            requeue_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        return MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_QBUF");
    }

    queued_v4l2_buffers_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
}

}  // namespace frameshow
