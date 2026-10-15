#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <linux/videodev2.h>
#include "src/backend/media/capture/capture_session.h"
namespace mmltk::backend::media::live::test_support {
// Acceptance input only. CaptureSession owns the device descriptor, pinned
// USERPTR allocations, queueing, completion-driven recycling, and shutdown.
class LiveCaptureFixture final {
public:
 explicit LiveCaptureFixture(std::filesystem::path, std::uint64_t frame_limit = 0);
 ~LiveCaptureFixture();
 LiveCaptureFixture(const LiveCaptureFixture&) = delete;
 LiveCaptureFixture& operator=(const LiveCaptureFixture&) = delete;
 [[nodiscard]] mmltk::backend::media::capture::CaptureDeviceApi api() noexcept;
 [[nodiscard]] std::string path() const { return path_.string(); }

private:
 static int Ioctl(void*, int, unsigned long, void*) noexcept;
 int Invoke(int, unsigned long, void*) noexcept;
 int Signal(int) noexcept;
 std::filesystem::path path_;
 std::array<v4l2_buffer, 16> queued_{};
 std::size_t first_ = 0, count_ = 0;
 std::uint32_t width_ = 0, height_ = 0, sequence_ = 0;
 std::uint64_t frame_limit_ = 0, signaled_ = 0;
 bool streaming_ = false;
};
}  // namespace mmltk::backend::media::live::test_support
