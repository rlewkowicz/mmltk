module;
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/execution_policy.h"
#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module mmltk.backend.media.capture.capture_session;

#include "detail/capture_session_impl.hpp"

namespace mmltk::backend::media::capture {

using capture_internal::AllocateHostBuffer;
using capture_internal::CaptureSlotPhase;
using capture_internal::FreeHostBuffer;
using capture_internal::kBgr3BytesPerPixel;
using capture_internal::kBgr3V4l2PixelFormat;
using capture_internal::MakeCudaStatus;
using capture_internal::MakeErrnoStatus;
using capture_internal::MakeStatus;
using capture_internal::Xioctl;

namespace {

void retain_first_failure(Status* target, Status candidate) {
    if (target != nullptr && target->ok() && !candidate.ok()) { *target = std::move(candidate); }
}

}  // namespace

Status CaptureSession::Impl::InitializeCuda() {
    const cudaError_t set_device_status = cudaSetDevice(config.cuda_device_index);
    if (set_device_status != cudaSuccess) { return MakeCudaStatus(set_device_status, "cudaSetDevice"); }

    const cudaError_t init_status = cudaFree(nullptr);
    if (init_status != cudaSuccess) { return MakeCudaStatus(init_status, "cuda runtime initialization"); }
    return Status::Ok();
}

int CaptureSession::Impl::DeviceIoctl(const unsigned long request, void* const argument) noexcept {
    return config.device_api.ioctl != nullptr ? config.device_api.ioctl(config.device_api.context, fd_, request, argument)
                                              : Xioctl(fd_, request, argument);
}

Status CaptureSession::Impl::OpenDevice() {
    fd_ = open(config.device_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) { return MakeErrnoStatus(StatusCode::kNoDevice, "open"); }
    return Status::Ok();
}

Status CaptureSession::Impl::CloseDevice() {
    if (fd_ == -1) { return Status::Ok(); }
    const int closing_fd = fd_;
    fd_ = -1;
    if (::close(closing_fd) != 0) { return MakeErrnoStatus(StatusCode::kInternalError, "close capture device"); }
    return Status::Ok();
}

Status CaptureSession::Impl::ConfigureDevice() {
    Status status = ConfigureCaptureFormat();
    if (!status.ok()) { return status; }

    return ConfigureFrameRateAndBuffers();
}

Status CaptureSession::Impl::ConfigureCaptureFormat() {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config.width;
    format.fmt.pix.height = config.height;
    format.fmt.pix.pixelformat = kBgr3V4l2PixelFormat;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (DeviceIoctl(VIDIOC_S_FMT, &format) == -1) { return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_S_FMT"); }
    if (format.fmt.pix.pixelformat != kBgr3V4l2PixelFormat) {
        return MakeStatus(StatusCode::kUnsupported, "driver negotiated a different pixel format than requested BGR3");
    }

    const std::uint32_t capture_width = format.fmt.pix.width;
    const std::uint32_t capture_height = format.fmt.pix.height;
    const std::size_t minimum_line_bytes = static_cast<std::size_t>(capture_width) * kBgr3BytesPerPixel;
    bytes_per_line_ = std::max<std::size_t>(format.fmt.pix.bytesperline, minimum_line_bytes);
    size_image_ = std::max<std::size_t>(format.fmt.pix.sizeimage, bytes_per_line_ * static_cast<std::size_t>(capture_height));
    published_width_.store(capture_width, std::memory_order_release);
    published_height_.store(capture_height, std::memory_order_release);
    published_bytes_per_line_.store(static_cast<std::uint32_t>(bytes_per_line_), std::memory_order_release);
    return Status::Ok();
}

Status CaptureSession::Impl::ConfigureFrameRateAndBuffers() {
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = config.fps;
    if (DeviceIoctl(VIDIOC_S_PARM, &parm) == -1) { return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_S_PARM"); }

    v4l2_requestbuffers req{};
    req.count = config.v4l2_buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (DeviceIoctl(VIDIOC_REQBUFS, &req) == -1) { return MakeErrnoStatus(StatusCode::kUnsupported, "VIDIOC_REQBUFS"); }
    if (req.count == 0U) { return MakeStatus(StatusCode::kUnsupported, "device returned zero USERPTR buffers"); }
    actual_v4l2_buffer_count_ = req.count;
    return Status::Ok();
}

Status CaptureSession::Impl::StartStreaming() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (DeviceIoctl(VIDIOC_STREAMON, &type) == -1) {
        Status status = MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_STREAMON");
        SetLastError(status.message);
        return status;
    }
    streaming_.store(true, std::memory_order_release);
    return Status::Ok();
}

Status CaptureSession::Impl::StopStreaming() {
    if (!streaming_.load(std::memory_order_acquire) || fd_ == -1) { return Status::Ok(); }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    const int streamoff_rc = DeviceIoctl(VIDIOC_STREAMOFF, &type);
    streaming_.store(false, std::memory_order_release);
    if (streamoff_rc == -1) { return MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_STREAMOFF"); }
    return Status::Ok();
}

Status CaptureSession::Impl::AllocateHostSlots() {
    host_slots_.clear();
    host_slots_.reserve(actual_v4l2_buffer_count_);

    for (std::uint32_t slot_index = 0; slot_index < actual_v4l2_buffer_count_; ++slot_index) {
        auto slot = std::make_unique<HostSlotRuntime>();
        slot->slot_index = slot_index;
        host_slots_.push_back(std::move(slot));

        Status status = AllocateHostBuffer(size_image_, true, &host_slots_.back()->capture_buffer);
        if (!status.ok()) { return status; }
    }
    return Status::Ok();
}

Status CaptureSession::Impl::DestroyHostSlots() {
    Status status = Status::Ok();
    for (auto& slot : host_slots_) {
        const Status released = FreeHostBuffer(&slot->capture_buffer);
        retain_first_failure(&status, released);
        if (released.ok() && config.device_api.host_storage_released != nullptr)
            config.device_api.host_storage_released(config.device_api.context, slot->slot_index);
    }
    host_slots_.clear();
    return status;
}

Status CaptureSession::Impl::QueueAllV4l2Buffers() {
    for (std::uint32_t slot_index = 0; slot_index < host_slots_.size(); ++slot_index) {
        Status status = QueueV4l2Buffer(slot_index, false);
        if (!status.ok()) { return status; }
    }
    return Status::Ok();
}

int CaptureSession::Impl::TryQueueV4l2Buffer(const std::uint32_t slot_index) noexcept {
    if (fd_ < 0 || slot_index >= host_slots_.size()) { return EBADF; }
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = slot_index;
    buf.m.userptr = reinterpret_cast<unsigned long>(host_slots_[slot_index]->capture_buffer.data);
    const std::size_t capture_buffer_bytes = host_slots_[slot_index]->capture_buffer.bytes;
    if (capture_buffer_bytes > std::numeric_limits<decltype(buf.length)>::max()) { return EOVERFLOW; }
    buf.length = static_cast<decltype(buf.length)>(capture_buffer_bytes);
    if (DeviceIoctl(VIDIOC_QBUF, &buf) == -1) { return errno != 0 ? errno : EIO; }
    queued_v4l2_buffers_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

Status CaptureSession::Impl::QueueV4l2Buffer(const std::uint32_t slot_index, const bool record_requeue_failure) {
    const int error_number = TryQueueV4l2Buffer(slot_index);
    if (error_number != 0) {
        Status status = MakeErrnoStatus(StatusCode::kInternalError, "VIDIOC_QBUF", error_number);
        if (slot_index < host_slots_.size())
            host_slots_[slot_index]->phase.store(capture_internal::CaptureSlotPhaseValue(CaptureSlotPhase::kOwnerRetained),
                                                 std::memory_order_release);
        if (record_requeue_failure) {
            requeue_failures_.fetch_add(1, std::memory_order_relaxed);
            SetLastError(status.message);
            static_cast<void>(report_failure(active_identity(), status));
        }
        return status;
    }
    host_slots_[slot_index]->phase.store(capture_internal::CaptureSlotPhaseValue(CaptureSlotPhase::kHardwareQueued),
                                         std::memory_order_release);
    return Status::Ok();
}

}  // namespace mmltk::backend::media::capture
