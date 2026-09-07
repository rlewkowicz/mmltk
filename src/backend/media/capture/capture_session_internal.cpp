module;
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/execution_policy.h"
#include <cuda_runtime_api.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

module mmltk.backend.media.capture.capture_session;

#include "detail/capture_session_impl.hpp"

namespace mmltk::backend::media::capture::capture_internal {

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

}  // namespace mmltk::backend::media::capture::capture_internal

namespace mmltk::backend::media::capture::capture_internal {

Status MakeStatus(StatusCode code, std::string message) { return Status{code, std::move(message)}; }

Status MakeErrnoStatus(StatusCode code, const char* label) { return MakeErrnoStatus(code, label, errno); }

Status MakeErrnoStatus(StatusCode code, const char* label, const int error_number) {
    std::ostringstream oss;
    oss << label << " failed: " << std::strerror(error_number);
    return {code, oss.str()};
}

Status MakeCudaStatus(cudaError_t code, const char* label) {
    std::ostringstream oss;
    oss << label << " failed: " << cudaGetErrorString(code);
    return {StatusCode::kCudaError, oss.str()};
}

int Xioctl(int fd, unsigned long request, void* arg) noexcept {
    int rc = 0;
    do {
        rc = ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

std::size_t RoundUpToPage(std::size_t bytes, std::size_t page_size) {
    const std::size_t remainder = bytes % page_size;
    return remainder == 0 ? bytes : bytes + (page_size - remainder);
}

Status AllocateHostBuffer(std::size_t bytes, bool pinned, HostBuffer* out) {
    if (out == nullptr || bytes == 0U) { return MakeStatus(StatusCode::kInvalidArgument, "invalid host buffer request"); }

    try {
        HostBuffer candidate;
        if (pinned) {
            candidate.registered_storage = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
            candidate.registered_storage->ensure_bytes(bytes);
            candidate.data = candidate.registered_storage->data();
            candidate.bytes = candidate.registered_storage->capacity_bytes();
            candidate.pinned = true;
        } else {
            const int node = mmltk::common::system::bound_memory_node(mmltk::common::system::capture_memory_policy());
            candidate.storage = std::make_unique<mmltk::common::system::NumaMemory>(node);
            candidate.storage->ensure_bytes(bytes);
            candidate.data = candidate.storage->data();
            candidate.bytes = candidate.storage->capacity_bytes();
        }
        *out = std::move(candidate);
        return Status::Ok();
    } catch (const std::exception& error) { return MakeStatus(StatusCode::kInternalError, error.what()); }
}

Status FreeHostBuffer(HostBuffer* buffer) {
    if (buffer == nullptr || buffer->data == nullptr) return Status::Ok();
    if (buffer->registered_storage && buffer->registered_storage->ReleaseSettled() != CUDA_SUCCESS)
        return MakeStatus(StatusCode::kCudaError, "release capture local registered storage failed");
    *buffer = {};
    return Status::Ok();
}

std::uint64_t PackRegion(const CaptureRegion& region) {
    return static_cast<std::uint64_t>(region.x) | (static_cast<std::uint64_t>(region.y) << 16U) |
           (static_cast<std::uint64_t>(region.width) << 32U) | (static_cast<std::uint64_t>(region.height) << 48U);
}

CaptureRegion UnpackRegion(std::uint64_t packed) {
    return CaptureRegion{
        .x = static_cast<std::uint32_t>(packed & 0xFFFFU),
        .y = static_cast<std::uint32_t>((packed >> 16U) & 0xFFFFU),
        .width = static_cast<std::uint32_t>((packed >> 32U) & 0xFFFFU),
        .height = static_cast<std::uint32_t>((packed >> 48U) & 0xFFFFU),
    };
}

}  // namespace mmltk::backend::media::capture::capture_internal
