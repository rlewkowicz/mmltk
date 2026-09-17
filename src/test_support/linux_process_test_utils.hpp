#pragma once
#include "src/common/io/event_fd.h"
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::testsupport {
// Exception cleanup for a directly owned fork child. Successful wait transfers
// terminal status to the caller; otherwise destruction kills and reaps it.
class ScopedTestChild final {
   public:
    explicit ScopedTestChild(pid_t child) noexcept : child_(child) {}
    ~ScopedTestChild() {
        if (child_ < 0) return;
        (void)::kill(child_, SIGKILL);
        int status;
        while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
    }
    ScopedTestChild(const ScopedTestChild&) = delete;
    ScopedTestChild& operator=(const ScopedTestChild&) = delete;
    [[nodiscard]] int Wait() {
        int status;
        pid_t result;
        do { result = ::waitpid(child_, &status, 0); } while (result < 0 && errno == EINTR);
        if (result < 0) throw std::runtime_error(std::string{"waitpid failed: "} + std::strerror(errno));
        child_ = -1;
        return status;
    }

   private:
    pid_t child_;
};
struct PidfdWaitResult final {
    bool reaped = false;
    int status = -1;
};
inline void arm_timerfd(const int descriptor, const std::chrono::nanoseconds duration, const std::string_view purpose) {
    const auto count = duration.count();
    const itimerspec specification{.it_interval = {},
                                   .it_value = {.tv_sec = static_cast<time_t>(count / 1'000'000'000LL), .tv_nsec = static_cast<long>(count % 1'000'000'000LL)}};
    if (::timerfd_settime(descriptor, 0, &specification, nullptr) != 0) {
        throw std::runtime_error(std::string{"failed to arm "} + std::string{purpose} + ": " + std::strerror(errno));
    }
}
[[nodiscard]] inline bool consume_timerfd(const int descriptor) noexcept {
    const auto received = mmltk::common::io::read_counter_fd(descriptor);
    return received.bytes == static_cast<ssize_t>(sizeof(received.count)) && received.count == 1U;
}
[[nodiscard]] inline PidfdWaitResult reap_pidfd(const int pidfd, const pid_t child) noexcept {
    siginfo_t terminal{};
    int result = -1;
    do { result = ::waitid(P_PIDFD, static_cast<id_t>(pidfd), &terminal, WEXITED | WNOHANG); } while (result != 0 && errno == EINTR);
    if (result != 0 || terminal.si_pid != child) return {};
    return {.reaped = true, .status = terminal.si_code == CLD_EXITED ? terminal.si_status << 8 : terminal.si_status & 0x7f};
}
}  // namespace mmltk::testsupport
