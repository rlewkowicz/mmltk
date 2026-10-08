#include "src/common/io/poll.h"
#include <cerrno>
namespace mmltk::common::io {
int poll_descriptors(std::span<pollfd> descriptors, int timeout) noexcept {
 int ready;
 do { ready = ::poll(descriptors.data(), descriptors.size(), timeout); } while (ready < 0 && errno == EINTR);
 return ready;
}
}  // namespace mmltk::common::io
