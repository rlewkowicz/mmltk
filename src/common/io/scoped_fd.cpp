#include "src/common/io/scoped_fd.h"
#include <unistd.h>
#include <utility>
namespace mmltk::common::io {
ScopedFd::ScopedFd(const int descriptor) noexcept : fd_(descriptor) {}
ScopedFd::~ScopedFd() { reset(); }
ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
    if (this != &other) reset(std::exchange(other.fd_, -1));
    return *this;
}
int ScopedFd::get() const noexcept { return fd_; }
int ScopedFd::release() noexcept { return std::exchange(fd_, -1); }
void ScopedFd::reset(const int next) noexcept {
    if (fd_ >= 0) static_cast<void>(::close(fd_));
    fd_ = next;
}
}  // namespace mmltk::common::io
