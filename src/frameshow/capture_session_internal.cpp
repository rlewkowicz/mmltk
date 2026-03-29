#include "capture_session_impl.hpp"

#include <cstring>
#include <cstdlib>
#include <sstream>

#include <sys/ioctl.h>
#include <unistd.h>

namespace frameshow::capture_internal {

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

Status MakeStatus(StatusCode code, std::string message) {
  return Status{code, std::move(message)};
}

Status MakeErrnoStatus(StatusCode code, const char* label) {
  std::ostringstream oss;
  oss << label << " failed: " << std::strerror(errno);
  return {code, oss.str()};
}

Status MakeCudaStatus(cudaError_t code, const char* label) {
  std::ostringstream oss;
  oss << label << " failed: " << cudaGetErrorString(code);
  return {StatusCode::kCudaError, oss.str()};
}

int Xioctl(int fd, unsigned long request, void* arg) {
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
  if (out == nullptr || bytes == 0U) {
    return MakeStatus(StatusCode::kInvalidArgument, "invalid host buffer request");
  }

  const std::size_t page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  const std::size_t aligned_bytes = RoundUpToPage(bytes, page_size);

  void* memory = nullptr;
  if (::posix_memalign(&memory, page_size, aligned_bytes) != 0) {
    return MakeStatus(StatusCode::kInternalError,
                      "posix_memalign failed for capture buffer");
  }
  std::memset(memory, 0, aligned_bytes);

  out->data = memory;
  out->bytes = aligned_bytes;
  out->pinned = false;
  if (!pinned) {
    return Status::Ok();
  }

  const cudaError_t cuda_status =
      cudaHostRegister(memory, aligned_bytes, cudaHostRegisterDefault);
  if (cuda_status != cudaSuccess) {
    std::free(memory);
    *out = {};
    return MakeCudaStatus(cuda_status, "cudaHostRegister");
  }

  out->pinned = true;
  return Status::Ok();
}

void FreeHostBuffer(HostBuffer* buffer) {
  if (buffer == nullptr || buffer->data == nullptr) {
    return;
  }
  if (buffer->pinned) {
    cudaHostUnregister(buffer->data);
  }
  std::free(buffer->data);
  *buffer = {};
}

std::uint64_t PackRegion(const CaptureRegion& region) {
  return static_cast<std::uint64_t>(region.x) |
         (static_cast<std::uint64_t>(region.y) << 16U) |
         (static_cast<std::uint64_t>(region.width) << 32U) |
         (static_cast<std::uint64_t>(region.height) << 48U);
}

CaptureRegion UnpackRegion(std::uint64_t packed) {
  return CaptureRegion{
      .x = static_cast<std::uint32_t>(packed & 0xFFFFU),
      .y = static_cast<std::uint32_t>((packed >> 16U) & 0xFFFFU),
      .width = static_cast<std::uint32_t>((packed >> 32U) & 0xFFFFU),
      .height = static_cast<std::uint32_t>((packed >> 48U) & 0xFFFFU),
  };
}

}  // namespace frameshow::capture_internal
