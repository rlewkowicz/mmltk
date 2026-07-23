#include "gui/console_output.h"
#include "gui/subprocess_utils.h"

#include "support/catch2_compat.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <sys/wait.h>
#include <string>

#include <unistd.h>

namespace {

enum class TestFailureStage : std::uint8_t {
    kExec = 0,
};

void close_pipe(std::array<int, 2>& pipe_fds) {
    mmltk::gui::subprocess::close_pipe_pair(pipe_fds);
}

void test_append_console_output_normalizes_terminal_sequences() {
    std::string tail;
    mmltk::gui::console_output::append_console_output(tail, "hello\rworld\nabc\b!\033[31m?\n", 128);
    assert(tail == "world\nab!?\n");
}

void test_child_setup_failure_round_trip() {
    std::array<int, 2> pipe_fds{-1, -1};
    mmltk::gui::subprocess::create_cloexec_pipe(pipe_fds, "test child setup pipe");

    errno = ENOENT;
    assert(mmltk::gui::subprocess::write_child_setup_failure(pipe_fds[1], TestFailureStage::kExec));
    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    const auto failure = mmltk::gui::subprocess::read_child_setup_failure<TestFailureStage>(pipe_fds[0]);
    assert(failure.has_value());
    if (!failure.has_value()) {
        return;
    }
    const auto& failure_value = *failure;
    assert(failure_value.stage == TestFailureStage::kExec);
    assert(failure_value.error_number == ENOENT);

    close_pipe(pipe_fds);
}

void test_spawn_captured_child_process_returns_open_fds() {
    auto child = mmltk::gui::subprocess::spawn_captured_child_process("test child", [](const int stdout_fd, const int) {
        if (::dup2(stdout_fd, STDOUT_FILENO) < 0 || ::dup2(stdout_fd, STDERR_FILENO) < 0) {
            std::_Exit(127);
        }
        ::close(stdout_fd);
        constexpr std::array<char, 16> kPayload{'s', 'p', 'a', 'w', 'n', 'e', 'd',  ' ',
                                                'o', 'u', 't', 'p', 'u', 't', '\n', '\0'};
        const ssize_t written = ::write(STDOUT_FILENO, kPayload.data(), kPayload.size() - 1U);
        if (written != static_cast<ssize_t>(kPayload.size() - 1U)) {
            std::_Exit(127);
        }
        std::_Exit(0);
    });

    assert(child.pid > 0);
    assert(child.stdout_fd >= 0);
    assert(child.setup_error_fd >= 0);

    int status = 0;
    assert(::waitpid(child.pid, &status, 0) == child.pid);
    const std::string output =
        mmltk::gui::console_output::read_fd(child.stdout_fd, "failed to read spawned child output: ", true);
    const auto setup_failure = mmltk::gui::subprocess::read_child_setup_failure<TestFailureStage>(child.setup_error_fd);
    mmltk::gui::subprocess::close_captured_child_process(child);

    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(output == "spawned output\n");
    assert(!setup_failure.has_value());
}

void test_run_captured_child_process_captures_output() {
    const auto result = mmltk::gui::subprocess::run_captured_child_process<TestFailureStage>(
        "test child", "failed to read test child output: ", [](const int stdout_fd, const int setup_error_fd) {
            if (::dup2(stdout_fd, STDOUT_FILENO) < 0 || ::dup2(stdout_fd, STDERR_FILENO) < 0) {
                errno = ENOENT;
                (void)mmltk::gui::subprocess::write_child_setup_failure(setup_error_fd, TestFailureStage::kExec);
                std::_Exit(127);
            }
            ::close(stdout_fd);
            constexpr std::array<char, 17> kPayload{'c', 'a', 'p', 't', 'u', 'r', 'e',  'd', ' ',
                                                    'o', 'u', 't', 'p', 'u', 't', '\n', '\0'};
            const ssize_t written = ::write(STDOUT_FILENO, kPayload.data(), kPayload.size() - 1U);
            if (written != static_cast<ssize_t>(kPayload.size() - 1U)) {
                std::_Exit(127);
            }
            std::_Exit(0);
        });

    assert(result.output == "captured output\n");
    assert(WIFEXITED(result.status));
    assert(WEXITSTATUS(result.status) == 0);
    assert(!result.setup_failure.has_value());
}

void test_run_captured_child_process_reports_setup_failure() {
    const auto result = mmltk::gui::subprocess::run_captured_child_process<TestFailureStage>(
        "test child", "failed to read test child output: ", [](const int, const int setup_error_fd) {
            errno = ENOENT;
            (void)mmltk::gui::subprocess::write_child_setup_failure(setup_error_fd, TestFailureStage::kExec);
            std::_Exit(127);
        });

    assert(result.output.empty());
    assert(WIFEXITED(result.status));
    assert(WEXITSTATUS(result.status) == 127);
    assert(result.setup_failure.has_value());
    if (!result.setup_failure.has_value()) {
        return;
    }
    const auto& failure = *result.setup_failure;
    assert(failure.stage == TestFailureStage::kExec);
    assert(failure.error_number == ENOENT);
}

void test_drain_nonblocking_fd_reads_available_output() {
    std::array<int, 2> pipe_fds{-1, -1};
    mmltk::gui::subprocess::create_pipe(pipe_fds, "test output pipe");
    mmltk::gui::subprocess::set_nonblocking(pipe_fds[0]);

    constexpr std::array<char, 6> kPayload{'r', 'e', 'a', 'd', 'y', '\0'};
    assert(::write(pipe_fds[1], kPayload.data(), kPayload.size() - 1U) == static_cast<ssize_t>(kPayload.size() - 1U));

    const std::string output =
        mmltk::gui::console_output::read_fd(pipe_fds[0], "failed to read test output pipe: ", true);
    assert(output == "ready");

    close_pipe(pipe_fds);
}

void test_read_fd_to_string_reads_until_eof() {
    std::array<int, 2> pipe_fds{-1, -1};
    mmltk::gui::subprocess::create_pipe(pipe_fds, "test blocking output pipe");

    constexpr std::array<char, 12> kPayload{'v', 'a', 's', 't', ' ', 'o', 'u', 't', 'p', 'u', 't', '\0'};
    assert(::write(pipe_fds[1], kPayload.data(), kPayload.size() - 1U) == static_cast<ssize_t>(kPayload.size() - 1U));
    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    const std::string output =
        mmltk::gui::console_output::read_fd(pipe_fds[0], "failed to read test blocking output pipe: ");
    assert(output == "vast output");

    close_pipe(pipe_fds);
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_append_console_output_normalizes_terminal_sequences);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_child_setup_failure_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_spawn_captured_child_process_returns_open_fds);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_run_captured_child_process_captures_output);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_run_captured_child_process_reports_setup_failure);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_drain_nonblocking_fd_reads_available_output);
MMLTK_REGISTER_TEST_CASE("[gui][subprocess_utils]", test_read_fd_to_string_reads_until_eof);
