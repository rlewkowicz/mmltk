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
CounterRead read_counter_fd(const int descriptor) noexcept {
    CounterRead result;
    do { result.bytes = ::read(descriptor, &result.count, sizeof(result.count)); } while (result.bytes < 0 && errno == EINTR);
    if (result.bytes < 0) result.error = errno;
    return result;
}
EventFdWait wait_event_fd(const int descriptor) noexcept {
    pollfd readiness{.fd = descriptor, .events = POLLIN, .revents = 0};
    int poll_result = -1;
    do { poll_result = ::poll(&readiness, 1U, -1); } while (poll_result < 0 && errno == EINTR);
    if (poll_result != 1 || (readiness.revents & POLLIN) == 0) return {EventFdWaitStatus::WaitFailed, 0U};
    const auto result = read_counter_fd(descriptor);
    return result.bytes == static_cast<ssize_t>(sizeof(result.count)) ? EventFdWait{EventFdWaitStatus::Woken, result.count}
                                                                   : EventFdWait{EventFdWaitStatus::ReadFailed, 0U};
}
void drain_event_fd(const int descriptor) noexcept {
    if (descriptor < 0) return;
    for (;;) {
        const auto result = read_counter_fd(descriptor);
        if (result.bytes != static_cast<ssize_t>(sizeof(result.count))) return;
    }
}
}  // namespace mmltk::common::io
