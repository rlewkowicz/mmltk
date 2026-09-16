#include "src/frameworks/process/subprocess_utils.h"
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
namespace {
namespace process = mmltk::frameworks::process;
static_assert(!std::is_copy_constructible_v<process::CapturedChildProcess>);
static_assert(!std::is_copy_assignable_v<process::CapturedChildProcess>);
static_assert(std::is_nothrow_move_constructible_v<process::CapturedChildProcess>);
static_assert(std::is_nothrow_move_assignable_v<process::CapturedChildProcess>);
[[nodiscard]] bool write_exact(const int fd, const std::string_view payload) {
    return ::write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size());
}
[[noreturn]] void exit_after_writing_stdout(const std::string_view payload) { std::_Exit(write_exact(STDOUT_FILENO, payload) ? 0 : 127); }
TEST_CASE("child_setup_failure_round_trips", "[frameworks][process]") {
    std::array<int, 2> pipe_fds{-1, -1};
    REQUIRE(::pipe2(pipe_fds.data(), O_CLOEXEC) == 0);
    const process::ChildSetupFailure expected{process::ChildSetupStage::Exec, ENOENT};
    REQUIRE(::write(pipe_fds[1], &expected, sizeof(expected)) == static_cast<ssize_t>(sizeof(expected)));
    ::close(std::exchange(pipe_fds[1], -1));
    const auto failure = process::read_child_setup_failure(pipe_fds[0]);
    REQUIRE(failure.has_value());
    CHECK(failure->stage == process::ChildSetupStage::Exec);
    CHECK(failure->error_number == ENOENT);
    ::close(std::exchange(pipe_fds[0], -1));
}
TEST_CASE("captured_child_process_owns_descriptors_and_moves_exactly_once", "[frameworks][process]") {
    auto child = process::spawn_captured_child_process("test child", [](const int output_fd, const int setup_fd) {
        process::prepare_captured_output_child(output_fd, setup_fd);
        exit_after_writing_stdout("spawned output\n");
    });
    REQUIRE(child.pid() > 0);
    REQUIRE(child.pidfd() >= 0);
    REQUIRE(child.stdout_fd() >= 0);
    REQUIRE(child.setup_error_fd() >= 0);
    process::CapturedChildProcess moved = std::move(child);
    CHECK(child.pid() < 0);
    CHECK(child.pidfd() < 0);
    CHECK(child.stdout_fd() < 0);
    CHECK(child.setup_error_fd() < 0);
    const pid_t pid = moved.pid();
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    CHECK(moved.release_pid() == pid);
    std::array<char, 128U> output{};
    const ssize_t output_size = ::read(moved.stdout_fd(), output.data(), output.size());
    REQUIRE(output_size >= 0);
    const auto setup_failure = process::read_child_setup_failure(moved.setup_error_fd());
    moved.close();
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(std::string_view(output.data(), static_cast<std::size_t>(output_size)) == "spawned output\n");
    CHECK_FALSE(setup_failure.has_value());
}
TEST_CASE("captured_child_runner_collects_output", "[frameworks][process]") {
    const auto result = process::run_captured_child_process("test child", "failed to read test child output: ", [](const int output_fd, const int setup_fd) {
        process::prepare_captured_output_child(output_fd, setup_fd);
        exit_after_writing_stdout("captured output\n");
    });
    CHECK(result.output == "captured output\n");
    REQUIRE(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 0);
    CHECK_FALSE(result.setup_failure.has_value());
}
TEST_CASE("captured_child_runner_reports_setup_failure", "[frameworks][process]") {
    const auto result = process::run_captured_child_process("test child", "failed to read test child output: ", [](const int, const int setup_fd) {
        errno = ENOENT;
        process::fail_child_setup(setup_fd, process::ChildSetupStage::Exec);
    });
    CHECK(result.output.empty());
    REQUIRE(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 127);
    REQUIRE(result.setup_failure.has_value());
    CHECK(result.setup_failure->stage == process::ChildSetupStage::Exec);
    CHECK(result.setup_failure->error_number == ENOENT);
}
TEST_CASE("captured_child_runner_enforces_output_bound", "[frameworks][process]") {
    CHECK_THROWS_AS(process::run_captured_child_process(
                        "test child", "failed to read test child output: ",
                        [](const int output_fd, const int setup_fd) {
                            process::prepare_captured_output_child(output_fd, setup_fd);
                            exit_after_writing_stdout("too much output");
                        },
                        {}, std::chrono::milliseconds{0}, -1, false, 4U),
                    std::runtime_error);
}
}  // namespace
