#pragma once

#include <array>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace mmltk::testsupport {

struct SubprocessResult {
    int exit_code = -1;
    std::string output_text;
    std::string stdout_text;
    std::string stderr_text;
};

inline std::string mmltk_cli_path() {
#ifndef MMLTK_TEST_MMLTK_CLI_PATH
#error "MMLTK_TEST_MMLTK_CLI_PATH must be defined for CLI subprocess tests"
#endif
    const std::filesystem::path configured_path = MMLTK_TEST_MMLTK_CLI_PATH;
    std::error_code error;
    if (std::filesystem::exists(configured_path, error) && !error) {
        return configured_path.string();
    }

    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read > 0) {
        buffer[static_cast<std::size_t>(bytes_read)] = '\0';
        const std::filesystem::path sibling_cli =
            std::filesystem::path(buffer.data()).parent_path() / "mmltk";
        error.clear();
        if (std::filesystem::exists(sibling_cli, error) && !error) {
            return sibling_cli.string();
        }
    }

    const std::filesystem::path installed_cli = "/opt/mmltk/bin/mmltk";
    error.clear();
    if (std::filesystem::exists(installed_cli, error) && !error) {
        return installed_cli.string();
    }

    return configured_path.string();
}

namespace {

[[nodiscard]] inline std::string make_errno_message(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

inline void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error(make_errno_message("fcntl(F_GETFL) failed"));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(make_errno_message("fcntl(F_SETFL) failed"));
    }
}

inline void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

inline void append_available_output(int& fd, std::string& text, std::string& combined_text) {
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            combined_text.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            close_fd(fd);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_fd(fd);
        throw std::runtime_error(make_errno_message("read failed"));
    }
}

} // namespace

inline SubprocessResult run_subprocess_capture_output(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("run_subprocess_capture_output requires at least one argument");
    }

    std::array<int, 2> stdout_pipe{};
    std::array<int, 2> stderr_pipe{};
    if (::pipe(stdout_pipe.data()) != 0) {
        throw std::runtime_error(make_errno_message("pipe(stdout) failed"));
    }
    if (::pipe(stderr_pipe.data()) != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        throw std::runtime_error(make_errno_message("pipe(stderr) failed"));
    }

    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1);
    for (const auto& arg : args) {
        raw_args.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        throw std::runtime_error(make_errno_message("fork failed"));
    }
    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            std::fprintf(stderr, "dup2 failed: %s\n", std::strerror(errno));
            std::_Exit(127);
        }
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execvp(raw_args.front(), raw_args.data());
        std::fprintf(stderr, "execvp failed: %s\n", std::strerror(errno));
        std::_Exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    std::string output_text;
    std::string stdout_text;
    std::string stderr_text;
    while (true) {
        if (stdout_pipe[0] < 0 && stderr_pipe[0] < 0) {
            break;
        }
        std::array<pollfd, 2> poll_fds{{
            {stdout_pipe[0], POLLIN | POLLHUP | POLLERR | POLLNVAL, 0},
            {stderr_pipe[0], POLLIN | POLLHUP | POLLERR | POLLNVAL, 0},
        }};
        const int ready = ::poll(poll_fds.data(), poll_fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            close_fd(stdout_pipe[0]);
            close_fd(stderr_pipe[0]);
            throw std::runtime_error(make_errno_message("poll failed"));
        }

        if (stdout_pipe[0] >= 0 && (poll_fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            append_available_output(stdout_pipe[0], stdout_text, output_text);
        }
        if (stderr_pipe[0] >= 0 && (poll_fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            append_available_output(stderr_pipe[0], stderr_text, output_text);
        }
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(make_errno_message("waitpid failed"));
    }
    if (!WIFEXITED(status)) {
        throw std::runtime_error("subprocess did not exit normally");
    }

    return SubprocessResult{
        WEXITSTATUS(status),
        std::move(output_text),
        std::move(stdout_text),
        std::move(stderr_text),
    };
}

} // namespace mmltk::testsupport
