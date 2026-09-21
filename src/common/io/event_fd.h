#pragma once
#include <cstdint>
#include <sys/types.h>
namespace mmltk::common::io {
// EAGAIN is success for a nonblocking eventfd: a saturated counter already
// carries the wake edge. Callers decide whether other failures are fatal.
[[nodiscard]] bool signal_event_fd(int descriptor) noexcept;
struct CounterRead final {
 std::uint64_t count = 0U;
 ssize_t bytes = -1;
 int error = 0;
};
// Retries only interrupted reads. The caller owns size, value and failure policy.
// Captures failure errno without clearing or otherwise changing ambient errno.
[[nodiscard]] CounterRead read_counter_fd(int descriptor) noexcept;
enum class EventFdWaitStatus : std::uint8_t {
 // The descriptor signalled POLLIN and its counter was consumed.
 Woken = 0U,
 // The wait itself failed: poll reported an error or a condition other than POLLIN.
 WaitFailed = 1U,
 // POLLIN was observed but the counter could not be consumed.
 ReadFailed = 2U,
};
struct EventFdWait {
 EventFdWaitStatus status = EventFdWaitStatus::WaitFailed;
 std::uint64_t count = 0U;
};
// Blocks a worker thread on a blocking eventfd until it is signalled and consumes the counter.
// Every worker loop that parks on an eventfd shares this wait, so the EINTR retries and the
// readiness checks are written once; callers decide which failures are fatal for their contract.
[[nodiscard]] EventFdWait wait_event_fd(int descriptor) noexcept;
void drain_event_fd(int descriptor) noexcept;
}  // namespace mmltk::common::io
