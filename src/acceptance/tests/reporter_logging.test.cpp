#include <spdlog/details/log_msg_payload.h>
#include <spdlog/formatter.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/common/io/scoped_fd.h"
#include "src/test_support/linux_process_test_utils.hpp"
#include "src/test_support/subprocess_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
import mmltk.common.logging.mmltk_logging;
namespace {
using namespace mmltk::testsupport;
class FixedFormatter final : public spdlog::formatter {
   public:
    explicit FixedFormatter(std::string text) : text_(std::move(text)) {}
    void format(const spdlog::details::log_msg&, spdlog::memory_buf_t& dest) override {
        dest.clear();
        dest.append(text_.data(), text_.data() + text_.size());
    }
    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override { return std::make_unique<FixedFormatter>(text_); }

   private:
    std::string text_;
};
std::string current_test_binary_path() {
    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read <= 0) { throw std::runtime_error("failed to resolve current test binary path"); }
    buffer[static_cast<std::size_t>(bytes_read)] = '\0';
    return {buffer.data()};
}
SubprocessResult run_reporter_fixture(const std::string& reporter_name) {
    return run_subprocess_capture_output({
        current_test_binary_path(),
        "vendored_catch2_reporter_fixture",
        "--reporter",
        reporter_name,
        "--colour-mode",
        "none",
    });
}
void test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting() {
    spdlog::details::log_msg msg("vendored-helper", spdlog::level::info, "raw payload");
    FixedFormatter formatter("formatted payload");
    spdlog::memory_buf_t formatted;
    const spdlog::string_view_t raw_payload = spdlog::details::format_log_msg_payload(false, formatter, msg, formatted);
    REQUIRE((raw_payload == msg.payload));
    REQUIRE((formatted.size() == 0U));
    REQUIRE((spdlog::details::log_msg_payload_length(raw_payload) == static_cast<int>(msg.payload.size())));
    const spdlog::string_view_t formatted_payload = spdlog::details::format_log_msg_payload(true, formatter, msg, formatted);
    REQUIRE((formatted_payload == spdlog::string_view_t("formatted payload", 17)));
    REQUIRE((spdlog::details::log_msg_payload_length(formatted_payload) == 17));
}
void test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max() {
    constexpr std::array<char, 2> kSentinel{'x', '\0'};
    const std::size_t oversized_length = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 128U;
    const spdlog::string_view_t oversized_payload(kSentinel.data(), oversized_length);
    REQUIRE((spdlog::details::log_msg_payload_length(oversized_payload) == std::numeric_limits<int>::max()));
}
void test_catch2_compact_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("compact");
    REQUIRE((result.exit_code != 0));
    REQUIRE((result.output_text.find("failed: fixture_value == 2") != std::string::npos));
    REQUIRE((result.output_text.find("for: 1 == 2") != std::string::npos));
    REQUIRE((result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos));
}
void test_catch2_tap_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("tap");
    REQUIRE((result.exit_code != 0));
    REQUIRE((result.output_text.find("# vendored_catch2_reporter_fixture") != std::string::npos));
    REQUIRE((result.output_text.find("not ok 1 - fixture_value == 2") != std::string::npos));
    REQUIRE((result.output_text.find("for: 1 == 2") != std::string::npos));
    REQUIRE((result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos));
}
}  // namespace
TEST_CASE("vendored_catch2_reporter_fixture", "[.][core][vendored][reporter_fixture]") {
    const int fixture_value = 1;
    INFO("vendored reporter info");
    // NOLINTNEXTLINE(bugprone-chained-comparison): Catch2 decomposes REQUIRE through operator<=.
    REQUIRE(fixture_value == 2);
}
TEST_CASE("test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting", "[core][vendored]") {
    test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting();
}
TEST_CASE("test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max", "[core][vendored]") {
    test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max();
}
TEST_CASE("test_catch2_compact_reporter_formats_shared_assertion_details", "[core][vendored]") {
    test_catch2_compact_reporter_formats_shared_assertion_details();
}
TEST_CASE("test_catch2_tap_reporter_formats_shared_assertion_details", "[core][vendored]") { test_catch2_tap_reporter_formats_shared_assertion_details(); }

TEST_CASE("fatal_reporting_fixture", "[.][core][logging][fatal_fixture]") {
    namespace logging = mmltk::common::logging;
    const char* scenario = std::getenv("MMLTK_FATAL_FIXTURE");
    REQUIRE(scenario != nullptr);
    const std::string_view mode{scenario};
    if (mode != "uninitialized") {
        auto config = logging::default_config("fatal-test");
        const bool configured = mode == "enabled" || mode == "disabled-after-init" || mode == "sink-failure" || mode == "init-failure";
        config.level = configured ? spdlog::level::info : spdlog::level::off;
        const char* destination = std::getenv("MMLTK_FATAL_LOG");
        REQUIRE(destination != nullptr);
        config.log_file = destination;
        try {
            logging::initialize(config);
        } catch (const std::exception& error) {
            logging::report_fatal("logging initialization", error.what());
            return;
        }
    }
    if (!logging::enabled(spdlog::level::info)) {
        bool collected = false;
        logging::info([&](auto&) { collected = true; });
        CHECK_FALSE(collected);
    }
    if (mode == "sink-failure") {
        class RefusedFormatter final : public spdlog::formatter {
           public:
            void format(const spdlog::details::log_msg&, spdlog::memory_buf_t&) override { throw std::runtime_error("refused sink"); }
            std::unique_ptr<spdlog::formatter> clone() const override { return std::make_unique<RefusedFormatter>(); }
        };
        spdlog::default_logger()->sinks().back()->set_formatter(std::make_unique<RefusedFormatter>());
    }
    if (mode == "disabled-after-init") logging::set_level(spdlog::level::off);
    if (mode == "long") {
        logging::report_fatal(std::string(2000U, 'c'), std::string(4000U, 'd'), -2147483647);
    } else {
        errno = EINVAL;
        logging::report_fatal("fixture component", "failure detail\nsecond line", 23);
        CHECK(errno == EINVAL);
    }
}
TEST_CASE("fatal reporting is bounded visible and independent of diagnostic sinks", "[core][logging]") {
    const ScopedTempDir directory{"mmltk-fatal-reporting"};
    for (const std::string mode : {"uninitialized", "off", "enabled", "disabled-after-init", "sink-failure", "init-failure", "long"}) {
        const auto log = directory.path() / (mode + ".log");
        if (mode == "init-failure") std::filesystem::create_directory(log);
        const bool enabled = mode == "enabled" || mode == "disabled-after-init" || mode == "sink-failure" || mode == "init-failure";
        const auto result = run_subprocess_capture_output({"env", "-u", "MMLTK_LOG_DIR", "MMLTK_FATAL_FIXTURE=" + mode,
            "MMLTK_LOG_LEVEL=off", "MMLTK_LOG_FILE=", "MMLTK_FATAL_LOG=" + log.string(),
            current_test_binary_path(), "fatal_reporting_fixture", "--reporter", "compact", "--colour-mode", "none"});
        INFO(result.output_text);
        REQUIRE(result.exit_code == 0);
        const auto begin = result.stderr_text.find("fatal: ");
        REQUIRE(begin != std::string::npos);
        const auto end = result.stderr_text.find('\n', begin);
        REQUIRE(end != std::string::npos);
        const auto terminal = result.stderr_text.substr(begin, end - begin + 1U);
        CHECK(terminal.size() <= 1024U);
        CHECK(result.stderr_text.find("fatal: ", end) == std::string::npos);
        CHECK(std::count(result.stderr_text.begin(), result.stderr_text.end(), '\n') == 3);
        if (mode == "init-failure") CHECK(result.stderr_text.find("logging initialization") != std::string::npos);
        else if (mode == "long") CHECK(result.stderr_text.find("... (status=-2147483647)") != std::string::npos);
        else CHECK(terminal == "fatal: fixture component: failure detail second line (status=23)\n");
        if (!enabled) CHECK_FALSE(std::filesystem::exists(log));
        if (mode == "enabled") {
            std::ifstream input{log};
            const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            CHECK(text.find("fixture component: failure detail second line (status=23)") != std::string::npos);
        }
        if (mode == "disabled-after-init") CHECK(std::filesystem::file_size(log) == 0U);
    }
}

TEST_CASE("fatal reporting preserves thread signals and status with a broken stderr pipe", "[core][logging]") {
    for (const bool pending_before : {false, true}) {
        std::array<int, 2U> descriptors{};
        REQUIRE(::pipe(descriptors.data()) == 0);
        mmltk::common::io::ScopedFd reader{descriptors[0]}, writer{descriptors[1]};
        reader.reset();
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            struct sigaction disposition{};
            disposition.sa_handler = SIG_DFL;
            sigset_t pipe_signal{}, original{}, after{}, pending{};
            if (::sigemptyset(&disposition.sa_mask) != 0 || ::sigaction(SIGPIPE, &disposition, nullptr) != 0 ||
                ::sigemptyset(&pipe_signal) != 0 || ::sigaddset(&pipe_signal, SIGPIPE) != 0 ||
                ::pthread_sigmask(pending_before ? SIG_BLOCK : SIG_UNBLOCK, &pipe_signal, nullptr) != 0 ||
                ::pthread_sigmask(SIG_BLOCK, nullptr, &original) != 0 || ::dup2(writer.get(), STDERR_FILENO) < 0) std::_Exit(90);
            if (pending_before && ::raise(SIGPIPE) != 0) std::_Exit(91);
            errno = EINVAL;
            mmltk::common::logging::report_fatal("broken pipe fixture", "must return");
            if (errno != EINVAL || ::pthread_sigmask(SIG_BLOCK, nullptr, &after) != 0 || ::sigpending(&pending) != 0) std::_Exit(92);
            for (int signal = 1; signal < NSIG; ++signal) {
                if (::sigismember(&original, signal) != ::sigismember(&after, signal)) std::_Exit(93);
            }
            if ((::sigismember(&pending, SIGPIPE) == 1) != pending_before) std::_Exit(94);
            std::_Exit(23);
        }
        mmltk::testsupport::ScopedTestChild custody{child};
        writer.reset();
        const int status = custody.Wait();
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 23);
    }
}
