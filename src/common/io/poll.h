#pragma once
#include <poll.h>
#include <span>
namespace mmltk::common::io {
// Preserve poll's result, revents and errno while retrying interrupted waits.
// A retry retains the caller's timeout, including an unbounded negative timeout.
[[nodiscard]] int poll_descriptors(std::span<pollfd> descriptors, int timeout = -1) noexcept;
}  // namespace mmltk::common::io
