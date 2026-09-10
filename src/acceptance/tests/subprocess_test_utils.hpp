#pragma once

#include "linux_process_test_utils.hpp"
#include "src/common/io/scoped_fd.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
    if (std::filesystem::exists(configured_path, error) && !error) { return configured_path.string(); }

    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read > 0) {
        buffer[static_cast<std::size_t>(bytes_read)] = '\0';
        const std::filesystem::path sibling_cli = std::filesystem::path(buffer.data()).parent_path() / "mmltk";
        error.clear();
        if (std::filesystem::exists(sibling_cli, error) && !error) { return sibling_cli.string(); }
    }

    const std::filesystem::path installed_cli = "/opt/mmltk/bin/mmltk";
    error.clear();
    if (std::filesystem::exists(installed_cli, error) && !error) { return installed_cli.string(); }

    return configured_path.string();
}

namespace {

[[nodiscard]] inline std::string make_errno_message(const char* operation) { return std::string(operation) + ": " + std::strerror(errno); }

inline void set_nonblocking(int fd) {
    int flags;
    do { flags = ::fcntl(fd, F_GETFL, 0); } while (flags < 0 && errno == EINTR);
    if (flags < 0) { throw std::runtime_error(make_errno_message("fcntl(F_GETFL) failed")); }
    int result;
    do { result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK); } while (result < 0 && errno == EINTR);
    if (result < 0) { throw std::runtime_error(make_errno_message("fcntl(F_SETFL) failed")); }
}

inline void append_available_output(mmltk::common::io::ScopedFd& fd, std::string& text, std::string& combined_text) {
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd.get(), buffer.data(), buffer.size());
        if (bytes_read > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            combined_text.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            fd.reset();
            return;
        }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
        throw std::runtime_error(make_errno_message("read failed"));
    }
}

}  // namespace

inline SubprocessResult run_subprocess_capture_output(const std::vector<std::string>& args) {
    if (args.empty()) { throw std::runtime_error("run_subprocess_capture_output requires at least one argument"); }

    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1);
    for (const auto& arg : args) raw_args.push_back(const_cast<char*>(arg.c_str()));
    raw_args.push_back(nullptr);

    std::array<int, 2> stdout_pipe{-1, -1};
    std::array<int, 2> stderr_pipe{-1, -1};
    if (::pipe2(stdout_pipe.data(), O_CLOEXEC) != 0) throw std::runtime_error(make_errno_message("pipe(stdout) failed"));
    mmltk::common::io::ScopedFd stdout_read{stdout_pipe[0]}, stdout_write{stdout_pipe[1]};
    if (::pipe2(stderr_pipe.data(), O_CLOEXEC) != 0) throw std::runtime_error(make_errno_message("pipe(stderr) failed"));
    mmltk::common::io::ScopedFd stderr_read{stderr_pipe[0]}, stderr_write{stderr_pipe[1]};

    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error(make_errno_message("fork failed"));
    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        const auto duplicate = [](int source, int destination) {
            int result;
            do { result = ::dup2(source, destination); } while (result < 0 && errno == EINTR);
            return result;
        };
        if (duplicate(stdout_pipe[1], STDOUT_FILENO) < 0 || duplicate(stderr_pipe[1], STDERR_FILENO) < 0) {
            std::fprintf(stderr, "dup2 failed: %s\n", std::strerror(errno));
            std::_Exit(127);
        }
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execvp(raw_args.front(), raw_args.data());
        std::fprintf(stderr, "execvp failed: %s\n", std::strerror(errno));
        std::_Exit(127);
    }

    ScopedTestChild child{pid};
    stdout_write.reset();
    stderr_write.reset();
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    std::string output_text;
    std::string stdout_text;
    std::string stderr_text;
    while (true) {
        if (stdout_read.get() < 0 && stderr_read.get() < 0) { break; }
        std::array<pollfd, 2> poll_fds{{
            {stdout_read.get(), POLLIN | POLLHUP | POLLERR | POLLNVAL, 0},
            {stderr_read.get(), POLLIN | POLLHUP | POLLERR | POLLNVAL, 0},
        }};
        const int ready = ::poll(poll_fds.data(), poll_fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) { continue; }
            throw std::runtime_error(make_errno_message("poll failed"));
        }

        if (stdout_read.get() >= 0 && (poll_fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            append_available_output(stdout_read, stdout_text, output_text);
        }
        if (stderr_read.get() >= 0 && (poll_fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            append_available_output(stderr_read, stderr_text, output_text);
        }
    }

    const int status = child.Wait();
    if (!WIFEXITED(status)) {
        std::string failure = "subprocess command:";
        for (const auto& argument : args) {
            failure += " [" + argument + "]";
        }
        if (WIFSIGNALED(status)) {
            failure += "\nterminated by signal " + std::to_string(WTERMSIG(status));
            if (WCOREDUMP(status)) failure += " (core dumped)";
        } else if (WIFSTOPPED(status)) {
            failure += "\nstopped by signal " + std::to_string(WSTOPSIG(status));
        } else if (WIFCONTINUED(status)) {
            failure += "\ncontinued without terminal exit";
        } else {
            failure += "\nunrecognized wait status";
        }
        failure += "\nraw wait status: " + std::to_string(status) + "\nstdout:\n" + stdout_text + "\nstderr:\n" + stderr_text;
        throw std::runtime_error(std::move(failure));
    }

    return SubprocessResult{
        WEXITSTATUS(status),
        std::move(output_text),
        std::move(stdout_text),
        std::move(stderr_text),
    };
}

}  // namespace mmltk::testsupport
