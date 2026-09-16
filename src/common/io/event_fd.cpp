#include "src/common/io/event_fd.h"
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
namespace mmltk::common::io {
bool signal_event_fd(const int descriptor) noexcept {
    if (descriptor < 0) return false;
    constexpr std::uint64_t value = 1U;
    ssize_t written = -1;
    do { written = ::write(descriptor, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(value)) || (written < 0 && errno == EAGAIN);
}
EventFdWait wait_event_fd(const int descriptor) noexcept {
    pollfd readiness{.fd = descriptor, .events = POLLIN, .revents = 0};
    int poll_result = -1;
    do { poll_result = ::poll(&readiness, 1U, -1); } while (poll_result < 0 && errno == EINTR);
    if (poll_result != 1 || (readiness.revents & POLLIN) == 0) return {EventFdWaitStatus::WaitFailed, 0U};
    std::uint64_t count = 0U;
    ssize_t result = -1;
    do { result = ::read(descriptor, &count, sizeof(count)); } while (result < 0 && errno == EINTR);
    return result == static_cast<ssize_t>(sizeof(count)) ? EventFdWait{EventFdWaitStatus::Woken, count} : EventFdWait{EventFdWaitStatus::ReadFailed, 0U};
}
void drain_event_fd(const int descriptor) noexcept {
    if (descriptor < 0) return;
    std::uint64_t value = 0U;
    for (;;) {
        const ssize_t count = ::read(descriptor, &value, sizeof(value));
        if (count == static_cast<ssize_t>(sizeof(value))) continue;
        if (count < 0 && errno == EINTR) continue;
        return;
    }
}
}  // namespace mmltk::common::io
