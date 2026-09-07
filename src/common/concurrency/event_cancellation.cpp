#include "src/common/concurrency/event_cancellation.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include "src/common/io/event_fd.h"
#include "src/common/io/scoped_fd.h"

namespace mmltk::common::concurrency::detail {

bool signal_cancellation_descriptor(const int descriptor) noexcept { return mmltk::common::io::signal_event_fd(descriptor); }

bool cancellation_descriptor_requested(const int descriptor) noexcept {
    pollfd event{.fd = descriptor, .events = POLLIN, .revents = 0};
    int ready = -1;
    do {
        ready = ::poll(&event, 1U, 0);
    } while (ready < 0 && errno == EINTR);
    return ready > 0 && (event.revents & (POLLIN | POLLERR | POLLHUP)) != 0;
}
int duplicate_cancellation_descriptor(const int descriptor, const char* const purpose) {
    const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) throw std::system_error(errno, std::generic_category(), purpose);
    return duplicate;
}
std::pair<int, int> mint_cancellation_descriptors() {
    const int owner = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (owner < 0) throw std::system_error(errno, std::generic_category(), "failed to create cancellation eventfd");
    mmltk::common::io::ScopedFd owner_guard{owner};
    const int token = duplicate_cancellation_descriptor(owner, "failed to duplicate cancellation token fd");
    return {owner_guard.release(), token};
}

}  // namespace mmltk::common::concurrency::detail
