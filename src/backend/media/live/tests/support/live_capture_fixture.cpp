#include "live_capture_fixture.h"
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
namespace mmltk::backend::media::live::test_support {
LiveCaptureFixture::LiveCaptureFixture(std::filesystem::path path, std::uint64_t frame_limit) : path_(std::move(path)), frame_limit_(frame_limit) {
 if (::mkfifo(path_.c_str(), S_IRUSR | S_IWUSR) != 0) throw std::system_error(errno, std::generic_category(), "Live acceptance input FIFO");
}
LiveCaptureFixture::~LiveCaptureFixture() { static_cast<void>(::unlink(path_.c_str())); }
mmltk::backend::media::capture::CaptureDeviceApi LiveCaptureFixture::api() noexcept { return {this, &Ioctl, nullptr}; }
int LiveCaptureFixture::Ioctl(void* context, int fd, unsigned long request, void* argument) noexcept { return static_cast<LiveCaptureFixture*>(context)->Invoke(fd, request, argument); }
int LiveCaptureFixture::Signal(int fd) noexcept {
 if (frame_limit_ && signaled_ >= frame_limit_) return 0;
 const char ready = 1;
 ssize_t written;
 do { written = ::write(fd, &ready, 1); } while (written < 0 && errno == EINTR);
 if (written == 1) {
  ++signaled_;
  return 0;
 }
 return -1;
}
int LiveCaptureFixture::Invoke(int fd, unsigned long request, void* argument) noexcept {
 switch (request) {
  case VIDIOC_S_FMT: {
   const auto& format = static_cast<v4l2_format*>(argument)->fmt.pix;
   if (format.width == 0 || format.height == 0 || format.width > 4096 || format.height > 4096 || format.pixelformat != V4L2_PIX_FMT_BGR24) {
    errno = EINVAL;
    return -1;
   }
   width_ = format.width;
   height_ = format.height;
   return 0;
  }
  case VIDIOC_S_PARM: return 0;
  case VIDIOC_REQBUFS: {
   const auto count = static_cast<v4l2_requestbuffers*>(argument)->count;
   if (count > queued_.size()) {
    errno = EINVAL;
    return -1;
   }
   first_ = count_ = 0;
   return 0;
  }
  case VIDIOC_QBUF: {
   if (count_ == queued_.size()) {
    errno = ENOBUFS;
    return -1;
   }
   const auto buffer = *static_cast<v4l2_buffer*>(argument);
   if (buffer.m.userptr == 0 || buffer.length < std::size_t(width_) * height_ * 3) {
    errno = EINVAL;
    return -1;
   }
   queued_[(first_ + count_) % queued_.size()] = buffer;
   ++count_;
   return streaming_ ? Signal(fd) : 0;
  }
  case VIDIOC_STREAMON:
   signaled_ = 0;
   streaming_ = true;
   for (std::size_t index = 0; index < count_; ++index)
    if (Signal(fd) != 0) {
     const auto failure = errno;
     static_cast<void>(Invoke(fd, VIDIOC_STREAMOFF, nullptr));
     errno = failure;
     return -1;
    }
   return 0;
  case VIDIOC_STREAMOFF: {
   streaming_ = false;
   std::array<char, 32> pending{};
   while (::read(fd, pending.data(), pending.size()) > 0) {}
   first_ = count_ = 0;
   return 0;
  }
  case VIDIOC_DQBUF: {
   if (!streaming_ || count_ == 0) {
    errno = EAGAIN;
    return -1;
   }
   char ready;
   ssize_t received;
   do { received = ::read(fd, &ready, 1); } while (received < 0 && errno == EINTR);
   if (received != 1) return -1;
   auto buffer = queued_[first_];
   first_ = (first_ + 1) % queued_.size();
   --count_;
   auto* pixels = reinterpret_cast<std::uint8_t*>(buffer.m.userptr);
   for (std::uint32_t y = 0; y < height_; ++y)
    for (std::uint32_t x = 0; x < width_; ++x) {
     const auto offset = (std::size_t(y) * width_ + x) * 3;
     pixels[offset] = ((x / 16 + y / 16) % 2) ? 48 : 96;
     pixels[offset + 1] = 24;
     pixels[offset + 2] = 16;
    }
   buffer.bytesused = width_ * height_ * 3;
   buffer.sequence = sequence_++;
   const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
   buffer.timestamp = {timestamp / 1'000'000, timestamp % 1'000'000};
   buffer.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
   *static_cast<v4l2_buffer*>(argument) = buffer;
   return 0;
  }
  default: errno = EINVAL; return -1;
 }
}
}  // namespace mmltk::backend::media::live::test_support
