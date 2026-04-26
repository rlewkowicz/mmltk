#pragma once

#include "console_output.h"

#include <chrono>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mmltk::gui::subprocess {

template <typename Stage>
struct ChildSetupFailure {
    Stage stage{};
    int error_number = 0;
};

template <typename Stage>
struct CapturedChildProcessResult {
    std::string output;
    int status = -1;
    std::optional<ChildSetupFailure<Stage>> setup_failure;
};

struct CapturedChildProcess {
    pid_t pid = -1;
    int stdout_fd = -1;
    int setup_error_fd = -1;
};

template <typename Stage>
inline bool write_child_setup_failure(const int fd, const Stage stage) noexcept {
    if (fd < 0) {
        return false;
    }

    const ChildSetupFailure<Stage> failure{stage, errno};
    const auto* data = reinterpret_cast<const char*>(&failure);
    std::size_t written = 0;
    while (written < sizeof(failure)) {
        const ssize_t chunk = ::write(fd, data + written, sizeof(failure) - written);
        if (chunk <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }
    return true;
}

template <typename Stage>
inline std::optional<ChildSetupFailure<Stage>> read_child_setup_failure(const int fd) {
    if (fd < 0) {
        return std::nullopt;
    }

    ChildSetupFailure<Stage> failure{};
    auto* data = reinterpret_cast<char*>(&failure);
    std::size_t read_bytes = 0;
    while (read_bytes < sizeof(failure)) {
        const ssize_t chunk = ::read(fd, data + read_bytes, sizeof(failure) - read_bytes);
        if (chunk > 0) {
            read_bytes += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    if (read_bytes != sizeof(failure)) {
        return std::nullopt;
    }
    return failure;
}

template <typename Stage, typename StageLabelFn>
inline std::string format_child_setup_failure(const ChildSetupFailure<Stage>& failure, StageLabelFn&& stage_label,
                                              const std::string_view process_name) {
    std::ostringstream stream;
    stream << process_name << " child " << stage_label(failure.stage)
           << " failed: " << std::strerror(failure.error_number);
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
    if (::pipe(pipe_fds.data()) == 0) {
        return;
    }
    pipe_fds = {-1, -1};
    throw std::runtime_error(std::string("failed to create ") + std::string(label) + ": " + std::strerror(errno));
}

inline void create_cloexec_pipe(std::array<int, 2>& pipe_fds, const std::string_view label) {
    if (::pipe2(pipe_fds.data(), O_CLOEXEC) == 0) {
        return;
    }
    pipe_fds = {-1, -1};
    throw std::runtime_error(std::string("failed to create ") + std::string(label) + ": " + std::strerror(errno));
}

inline bool set_nonblocking(const int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline int wait_child_process(pid_t pid) {
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(pid, &status, 0);
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        return status;
    }
}

inline void close_captured_child_process(CapturedChildProcess& process) noexcept {
    if (process.stdout_fd >= 0) {
        ::close(process.stdout_fd);
        process.stdout_fd = -1;
    }
    if (process.setup_error_fd >= 0) {
        ::close(process.setup_error_fd);
        process.setup_error_fd = -1;
    }
    process.pid = -1;
}

template <typename ChildSetupFn>
inline CapturedChildProcess spawn_captured_child_process(const std::string_view process_name,
                                                         ChildSetupFn&& child_setup,
                                                         const bool nonblocking_output = true) {
    std::array<int, 2> stdout_pipe{-1, -1};
    create_pipe(stdout_pipe, std::string(process_name) + " output pipe");

    std::array<int, 2> setup_error_pipe{-1, -1};
    try {
        create_cloexec_pipe(setup_error_pipe, std::string(process_name) + " child setup pipe");
    } catch (...) {
        close_pipe_pair(stdout_pipe);
        throw;
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        const int error_number = errno;
        close_pipe_pair(stdout_pipe);
        close_pipe_pair(setup_error_pipe);
        throw std::runtime_error(std::string("failed to fork ") + std::string(process_name) + ": " +
                                 std::strerror(error_number));
    }
    if (child_pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(setup_error_pipe[0]);
        try {
            child_setup(stdout_pipe[1], setup_error_pipe[1]);
        } catch (...) {
            std::_Exit(127);
        }
        std::_Exit(127);
    }

    ::close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    ::close(setup_error_pipe[1]);
    setup_error_pipe[1] = -1;

    CapturedChildProcess process;
    process.pid = child_pid;
    process.stdout_fd = stdout_pipe[0];
    process.setup_error_fd = setup_error_pipe[0];
    if (!nonblocking_output || set_nonblocking(process.stdout_fd)) {
        return process;
    }

    const int error_number = errno;
    close_captured_child_process(process);
    (void)::kill(child_pid, SIGKILL);
    (void)::waitpid(child_pid, nullptr, 0);
    throw std::runtime_error(std::string("failed to set nonblocking ") + std::string(process_name) +
                             " output pipe: " + std::strerror(error_number));
}

template <typename Stage, typename ChildSetupFn>
inline CapturedChildProcessResult<Stage> run_captured_child_process(
    const std::string_view process_name, const std::string_view output_error_prefix, ChildSetupFn&& child_setup,
    const std::chrono::milliseconds poll_interval = std::chrono::milliseconds{20}) {
    CapturedChildProcess process = spawn_captured_child_process(process_name, std::forward<ChildSetupFn>(child_setup));
    CapturedChildProcessResult<Stage> result;
    try {
        while (true) {
            result.output += mmltk::gui::console_output::read_fd(process.stdout_fd, output_error_prefix, true);

            int status = 0;
            const pid_t waited = ::waitpid(process.pid, &status, WNOHANG);
            if (waited < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("failed to wait for ") + std::string(process_name) + ": " +
                                         std::strerror(errno));
            }
            if (waited == 0) {
                std::this_thread::sleep_for(poll_interval);
                continue;
            }

            result.status = status;
            break;
        }

        result.output += mmltk::gui::console_output::read_fd(process.stdout_fd, output_error_prefix, true);
        result.setup_failure = read_child_setup_failure<Stage>(process.setup_error_fd);
        close_captured_child_process(process);
        return result;
    } catch (...) {
        const pid_t pid = process.pid;
        close_captured_child_process(process);
        if (pid > 0) {
            (void)::waitpid(pid, nullptr, 0);
        }
        throw;
    }
}

using mmltk::gui::console_output::append_console_output;
using mmltk::gui::console_output::read_fd;
using mmltk::gui::console_output::trim_output_tail;

}  // namespace mmltk::gui::subprocess
