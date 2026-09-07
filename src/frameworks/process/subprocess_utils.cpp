#include "src/frameworks/process/subprocess_utils.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::frameworks::process {

// CLEANUP-IGNORE Declarative label switch is owned by ChildSetupStage.
const char* child_setup_stage_label(const ChildSetupStage stage) noexcept {
    switch (stage) {
        case ChildSetupStage::SetProcessGroup:
            return "setpgid";
        case ChildSetupStage::RedirectOutput:
            return "output redirect";
        case ChildSetupStage::RedirectErrors:
            return "error redirect";
        case ChildSetupStage::PreserveDescriptor:
            return "descriptor preservation";
        case ChildSetupStage::SetEnvironment:
            return "setenv";
        case ChildSetupStage::Exec:
            return "exec";
    }
    return "unknown setup";
}

CapturedChildProcess::CapturedChildProcess(const int pid, const int pidfd, const int stdout_fd, const int setup_error_fd) noexcept
    : pid_(pid), pidfd_(pidfd), stdout_fd_(stdout_fd), setup_error_fd_(setup_error_fd) {}
CapturedChildProcess::CapturedChildProcess(CapturedChildProcess&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)),
      pidfd_(std::move(other.pidfd_)),
      stdout_fd_(std::move(other.stdout_fd_)),
      setup_error_fd_(std::move(other.setup_error_fd_)) {}
CapturedChildProcess& CapturedChildProcess::operator=(CapturedChildProcess&& other) noexcept {
    if (this != &other) {
        close();
        pid_ = std::exchange(other.pid_, -1);
        pidfd_ = std::move(other.pidfd_);
        stdout_fd_ = std::move(other.stdout_fd_);
        setup_error_fd_ = std::move(other.setup_error_fd_);
    }
    return *this;
}
CapturedChildProcess::~CapturedChildProcess() { close(); }
int CapturedChildProcess::pid() const noexcept { return pid_; }
int CapturedChildProcess::pidfd() const noexcept { return pidfd_.get(); }
int CapturedChildProcess::stdout_fd() const noexcept { return stdout_fd_.get(); }
int CapturedChildProcess::setup_error_fd() const noexcept { return setup_error_fd_.get(); }
int CapturedChildProcess::release_pid() noexcept { return std::exchange(pid_, -1); }
int CapturedChildProcess::release_pidfd() noexcept { return pidfd_.release(); }
int CapturedChildProcess::release_stdout_fd() noexcept { return stdout_fd_.release(); }
int CapturedChildProcess::release_setup_error_fd() noexcept { return setup_error_fd_.release(); }
void CapturedChildProcess::close_output() noexcept { stdout_fd_.reset(); }
void CapturedChildProcess::close() noexcept {
    if (pid_ > 0) {
        (void)::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    pidfd_.reset();
    stdout_fd_.reset();
    setup_error_fd_.reset();
    pid_ = -1;
}
bool ChildCancellationTarget::valid() const noexcept { return context && requested; }
bool ChildCancellationTarget::is_requested() const noexcept { return valid() && requested(context); }
CapturedChildAborted::CapturedChildAborted(const std::string& message, const CapturedChildAbortReason reason)
    : std::runtime_error(message), reason_(reason) {}
CapturedChildAbortReason CapturedChildAborted::reason() const noexcept { return reason_; }

using ScopedFd = mmltk::common::io::ScopedFd;

inline int open_pidfd(const pid_t pid, const std::string_view process_name) {
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
    if (fd >= 0) { return fd; }
    throw std::runtime_error(std::string("failed to open pidfd for ") + std::string(process_name) + ": " + std::strerror(errno));
}

inline ScopedFd make_timeout_fd(const std::chrono::milliseconds timeout, const std::string_view process_name) {
    if (timeout.count() <= 0) { return ScopedFd{}; }
    const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(std::string("failed to create timeout fd for ") + std::string(process_name) + ": " + std::strerror(errno));
    }
    ScopedFd timeout_fd(fd);
    const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
    itimerspec specification{};
    specification.it_value.tv_sec = static_cast<time_t>(timeout_ns.count() / 1000000000LL);
    specification.it_value.tv_nsec = static_cast<long>(timeout_ns.count() % 1000000000LL);
    if (::timerfd_settime(fd, 0, &specification, nullptr) != 0) {
        throw std::runtime_error(std::string("failed to arm timeout fd for ") + std::string(process_name) + ": " + std::strerror(errno));
    }
    return timeout_fd;
}

inline bool write_child_setup_failure(const int fd, const ChildSetupStage stage) noexcept {
    if (fd < 0) { return false; }

    const ChildSetupFailure failure{stage, errno};
    const auto* data = reinterpret_cast<const char*>(&failure);
    std::size_t written = 0;
    while (written < sizeof(failure)) {
        const ssize_t chunk = ::write(fd, data + written, sizeof(failure) - written);
        if (chunk <= 0) { return false; }
        written += static_cast<std::size_t>(chunk);
    }
    return true;
}

std::optional<ChildSetupFailure> read_child_setup_failure(const int fd) {
    if (fd < 0) { return std::nullopt; }

    ChildSetupFailure failure{};
    auto* data = reinterpret_cast<char*>(&failure);
    std::size_t read_bytes = 0;
    while (read_bytes < sizeof(failure)) {
        const ssize_t chunk = ::read(fd, data + read_bytes, sizeof(failure) - read_bytes);
        if (chunk > 0) {
            read_bytes += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk == 0) { break; }
        if (errno == EINTR) { continue; }
        break;
    }
    if (read_bytes != sizeof(failure)) { return std::nullopt; }
    return failure;
}

std::string format_child_setup_failure(const ChildSetupFailure& failure, const std::string_view process_name) {
    std::ostringstream stream;
    stream << process_name << " child " << child_setup_stage_label(failure.stage) << " failed: " << std::strerror(failure.error_number);
    return stream.str();
}

inline void close_pipe_pair(std::array<int, 2>& pipe_fds) noexcept {
    if (pipe_fds[0] >= 0) {
        ::close(pipe_fds[0]);
        pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
        ::close(pipe_fds[1]);
        pipe_fds[1] = -1;
    }
}

inline void create_pipe(std::array<int, 2>& pipe_fds, const std::string_view label) {
    if (::pipe(pipe_fds.data()) == 0) { return; }
    pipe_fds = {-1, -1};
    throw std::runtime_error(std::string("failed to create ") + std::string(label) + ": " + std::strerror(errno));
}

inline void create_cloexec_pipe(std::array<int, 2>& pipe_fds, const std::string_view label) {
    if (::pipe2(pipe_fds.data(), O_CLOEXEC) == 0) { return; }
    pipe_fds = {-1, -1};
    throw std::runtime_error(std::string("failed to create ") + std::string(label) + ": " + std::strerror(errno));
}

inline bool set_nonblocking(const int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return false; }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int wait_child_process(const int pid) {
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(pid, &status, 0);
        if (waited < 0) {
            if (errno == EINTR) { continue; }
            return -1;
        }
        return status;
    }
}

inline void kill_captured_child(const pid_t pid, const bool kill_process_group) noexcept {
    if (pid <= 0) { return; }
    if (kill_process_group) {
        if (::kill(-pid, SIGKILL) == 0) { return; }
    }
    (void)::kill(pid, SIGKILL);
}

CapturedChildProcess spawn_captured_child_process_erased(const std::string_view process_name, const ChildSetupTarget child_setup,
                                                         const bool nonblocking_output) {
    std::array<int, 2> stdout_pipe{-1, -1};
    std::string stdout_pipe_label{process_name};
    stdout_pipe_label.append(" output pipe");
    create_pipe(stdout_pipe, stdout_pipe_label);

    std::array<int, 2> setup_error_pipe{-1, -1};
    try {
        std::string setup_error_pipe_label{process_name};
        setup_error_pipe_label.append(" child setup pipe");
        create_cloexec_pipe(setup_error_pipe, setup_error_pipe_label);
    } catch (...) {
        close_pipe_pair(stdout_pipe);
        throw;
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        const int error_number = errno;
        close_pipe_pair(stdout_pipe);
        close_pipe_pair(setup_error_pipe);
        throw std::runtime_error(std::string("failed to fork ") + std::string(process_name) + ": " + std::strerror(error_number));
    }
    if (child_pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(setup_error_pipe[0]);
        try {
            child_setup.invoke(child_setup.context, stdout_pipe[1], setup_error_pipe[1]);
        } catch (...) { std::_Exit(127); }
        std::_Exit(127);
    }

    ::close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    ::close(setup_error_pipe[1]);
    setup_error_pipe[1] = -1;

    int pidfd = -1;
    try {
        pidfd = open_pidfd(child_pid, process_name);
    } catch (...) {
        ::close(stdout_pipe[0]);
        ::close(setup_error_pipe[0]);
        (void)::kill(child_pid, SIGKILL);
        (void)::waitpid(child_pid, nullptr, 0);
        throw;
    }
    CapturedChildProcess process{child_pid, pidfd, stdout_pipe[0], setup_error_pipe[0]};
    if (!nonblocking_output || set_nonblocking(process.stdout_fd())) { return process; }

    const int error_number = errno;
    process.close();
    throw std::runtime_error(std::string("failed to set nonblocking ") + std::string(process_name) +
                             " output pipe: " + std::strerror(error_number));
}

// Post-fork child setup cannot throw or return; every failed step reports its stage down the
// setup pipe and hard-exits. `succeeded` is evaluated by the caller so short-circuiting and
// errno capture stay exactly where they were.
[[noreturn]] void fail_child_setup(const int setup_fd, const ChildSetupStage stage) noexcept {
    (void)write_child_setup_failure(setup_fd, stage);
    std::_Exit(127);
}

void require_child_setup_step(const bool succeeded, const int setup_fd, const ChildSetupStage stage) noexcept {
    if (succeeded) { return; }
    fail_child_setup(setup_fd, stage);
}

void prepare_captured_output_child(const int output_fd, const int setup_fd) noexcept {
    require_child_setup_step(::setpgid(0, 0) == 0, setup_fd, ChildSetupStage::SetProcessGroup);
    require_child_setup_step(::dup2(output_fd, STDOUT_FILENO) >= 0 && ::dup2(output_fd, STDERR_FILENO) >= 0, setup_fd,
                             ChildSetupStage::RedirectOutput);
    ::close(output_fd);
}

// execv/execvp need a NULL-terminated char* array that stays valid for the call. Owning the
// backing strings alongside the pointer array keeps that lifetime coupling in one place.
class ArgvBuffer::Impl final {
   public:
    explicit Impl(std::vector<std::string> arguments) : storage_(std::move(arguments)) {
        pointers_.reserve(storage_.size() + 1U);
        for (std::string& argument : storage_) {
            pointers_.push_back(argument.data());
        }
        pointers_.push_back(nullptr);
    }

    [[nodiscard]] char* const* data() const noexcept { return pointers_.data(); }

    [[nodiscard]] char* program() const noexcept { return pointers_.front(); }

   private:
    std::vector<std::string> storage_;
    std::vector<char*> pointers_;
};
ArgvBuffer::ArgvBuffer(std::vector<std::string> arguments) : impl_(std::make_unique<Impl>(std::move(arguments))) {}
ArgvBuffer::~ArgvBuffer() = default;
char* const* ArgvBuffer::data() const noexcept { return impl_->data(); }
char* ArgvBuffer::program() const noexcept { return impl_->program(); }

inline void poll_child_process_events(std::array<pollfd, 4U>& wait_fds, const std::string_view wait_context) {
    int ready = -1;
    do {
        ready = ::poll(wait_fds.data(), wait_fds.size(), -1);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
        throw std::runtime_error(std::string("failed to wait for ") + std::string(wait_context) + ": " + std::strerror(errno));
    }
}

inline void read_captured_child_output(const int fd, CapturedChildProcessResult& result, const std::string_view error_prefix,
                                       const std::size_t output_limit, const std::size_t read_budget = kCapturedChildReadBudget) {
    if (fd < 0 || read_budget == 0U) { return; }

    std::array<char, 4096U> buffer{};
    std::size_t consumed = 0U;
    while (consumed < read_budget) {
        const std::size_t remaining_budget = read_budget - consumed;
        const std::size_t requested = std::min(buffer.size(), remaining_budget);
        const ssize_t bytes_read = ::read(fd, buffer.data(), requested);
        if (bytes_read > 0) {
            const std::size_t count = static_cast<std::size_t>(bytes_read);
            consumed += count;
            const std::size_t retained = std::min(result.output.size(), output_limit);
            const std::size_t remaining_output = output_limit - retained;
            const std::size_t append_count = std::min(count, remaining_output);
            if (append_count != 0U) { result.output.append(buffer.data(), append_count); }
            if (append_count != count) { result.output_limit_exceeded = true; }
            continue;
        }
        if (bytes_read == 0) { return; }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
        throw std::runtime_error(std::string(error_prefix) + std::strerror(errno));
    }
}

// Both the explicit-cancel and the poll-driven cancel/timeout paths tear the child down the same
// way; only the reason in the thrown message differs.
[[noreturn]] inline void abort_captured_child_process(CapturedChildProcess& process, CapturedChildProcessResult& result,
                                                      const std::string_view process_name, const std::string_view output_error_prefix,
                                                      const CapturedChildAbortReason reason, const bool kill_process_group,
                                                      const std::size_t output_limit) {
    kill_captured_child(process.pid(), kill_process_group);
    result.status = wait_child_process(process.pid());
    static_cast<void>(process.release_pid());
    read_captured_child_output(process.stdout_fd(), result, output_error_prefix, output_limit);
    process.close();
    throw CapturedChildAborted(std::string(process_name) + (reason == CapturedChildAbortReason::Cancelled ? " cancelled" : " timed out"),
                               reason);
}

CapturedChildProcessResult run_captured_child_process_erased(const std::string_view process_name,
                                                             const std::string_view output_error_prefix, const ChildSetupTarget child_setup,
                                                             const ChildCancellationTarget cancellation,
                                                             const std::chrono::milliseconds timeout, const int cancel_fd,
                                                             const bool kill_process_group, const std::size_t output_limit) {
    if (output_limit == 0U || output_limit > kMaximumCapturedChildOutputBytes) {
        throw std::invalid_argument("captured child output limit is outside the framework bound");
    }
    ScopedFd timeout_fd = make_timeout_fd(timeout, process_name);
    CapturedChildProcess process = spawn_captured_child_process_erased(process_name, child_setup, true);
    CapturedChildProcessResult result;
    try {
        while (true) {
            read_captured_child_output(process.stdout_fd(), result, output_error_prefix, output_limit);
            if (result.output_limit_exceeded) { throw std::runtime_error(std::string(process_name) + " output exceeded capture limit"); }
            if (cancellation.is_requested()) {
                abort_captured_child_process(process, result, process_name, output_error_prefix, CapturedChildAbortReason::Cancelled,
                                             kill_process_group, output_limit);
            }

            std::array<pollfd, 4U> wait_fds{{
                pollfd{process.stdout_fd(), POLLIN, 0},
                pollfd{process.pidfd(), POLLIN, 0},
                pollfd{cancel_fd, POLLIN, 0},
                pollfd{timeout_fd.get(), POLLIN, 0},
            }};
            poll_child_process_events(wait_fds, process_name);

            if ((wait_fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                read_captured_child_output(process.stdout_fd(), result, output_error_prefix, output_limit);
                if (result.output_limit_exceeded) {
                    throw std::runtime_error(std::string(process_name) + " output exceeded capture limit");
                }
                if ((wait_fds[0].revents & POLLHUP) != 0) {
                    read_captured_child_output(process.stdout_fd(), result, output_error_prefix, output_limit, output_limit + 1U);
                    if (result.output_limit_exceeded) {
                        throw std::runtime_error(std::string(process_name) + " output exceeded capture limit");
                    }
                    process.close_output();
                }
            }
            if ((wait_fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                result.status = wait_child_process(process.pid());
                if (result.status < 0) { throw std::runtime_error(std::string("failed to reap ") + std::string(process_name)); }
                static_cast<void>(process.release_pid());
                break;
            }
            const bool cancelled = (wait_fds[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0 || cancellation.is_requested();
            const bool timed_out = (wait_fds[3].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
            if (cancelled || timed_out) {
                abort_captured_child_process(process, result, process_name, output_error_prefix,
                                             cancelled ? CapturedChildAbortReason::Cancelled : CapturedChildAbortReason::TimedOut,
                                             kill_process_group, output_limit);
            }
        }

        read_captured_child_output(process.stdout_fd(), result, output_error_prefix, output_limit, output_limit + 1U);
        if (result.output_limit_exceeded) { throw std::runtime_error(std::string(process_name) + " output exceeded capture limit"); }
        result.setup_failure = read_child_setup_failure(process.setup_error_fd());
        process.close();
        return result;
    } catch (...) {
        const pid_t pid = process.release_pid();
        process.close();
        if (pid > 0) {
            kill_captured_child(pid, kill_process_group);
            (void)::waitpid(pid, nullptr, 0);
        }
        throw;
    }
}

}  // namespace mmltk::frameworks::process
