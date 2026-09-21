#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "spdmon/spdmon.hpp"
#include "src/backend/data/dataset_compiler.h"
#include "src/test_support/console_output.h"
#include "src/test_support/environment_test_utils.hpp"
namespace {
using ProgressClock = spdmon::ProgressBar::clock_t;
using ProgressTimePoint = ProgressClock::time_point;
ProgressClock::duration::rep g_progress_elapsed_ticks = 0;
ProgressTimePoint test_progress_now() { return ProgressTimePoint{ProgressClock::duration{g_progress_elapsed_ticks}}; }
void set_progress_now(const std::chrono::milliseconds elapsed) {
    g_progress_elapsed_ticks = std::chrono::duration_cast<ProgressClock::duration>(elapsed).count();
}
class ScopedStderrCapture final {
   public:
    ScopedStderrCapture() {
        std::fflush(stderr);
        if (::pipe(pipe_fds_.data()) != 0) { throw std::runtime_error("pipe failed: " + std::string{std::strerror(errno)}); }
        saved_stderr_ = ::dup(STDERR_FILENO);
        if (saved_stderr_ < 0) {
            close_pipe();
            throw std::runtime_error("stderr capture failed: " + std::string{std::strerror(errno)});
        }
        if (::dup2(pipe_fds_[1], STDERR_FILENO) < 0) {
            static_cast<void>(::close(saved_stderr_));
            saved_stderr_ = -1;
            close_pipe();
            throw std::runtime_error("stderr capture failed: " + std::string{std::strerror(errno)});
        }
        static_cast<void>(::close(pipe_fds_[1]));
        pipe_fds_[1] = -1;
    }
    ~ScopedStderrCapture() noexcept {
        if (finished_) return;
        try {
            static_cast<void>(finish());
        } catch (...) {
            restore_stderr();
            close_pipe();
        }
    }
    ScopedStderrCapture(const ScopedStderrCapture&) = delete;
    ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;
    std::string finish() {
        if (finished_) return output_;
        std::fflush(stderr);
        restore_stderr();
        output_ = mmltk::testsupport::console_output::read_fd(pipe_fds_[0], "read failed: ");
        static_cast<void>(::close(pipe_fds_[0]));
        pipe_fds_[0] = -1;
        finished_ = true;
        return output_;
    }

   private:
    std::array<int, 2U> pipe_fds_{-1, -1};
    int saved_stderr_ = -1;
    bool finished_ = false;
    std::string output_;
    void restore_stderr() noexcept {
        if (saved_stderr_ < 0) return;
        static_cast<void>(::dup2(saved_stderr_, STDERR_FILENO));
        static_cast<void>(::close(saved_stderr_));
        saved_stderr_ = -1;
    }
    void close_pipe() noexcept {
        for (int& descriptor : pipe_fds_) {
            if (descriptor < 0) continue;
            static_cast<void>(::close(descriptor));
            descriptor = -1;
        }
    }
};
std::vector<std::string> normalize_terminal_output(const std::string& output) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t index = 0U; index < output.size();) {
        if (output[index] == '\r') {
            current.clear();
            ++index;
        } else if (output.compare(index, 4U, "\033[2K") == 0) {
            current.clear();
            index += 4U;
        } else if (output[index] == '\n') {
            lines.push_back(current);
            current.clear();
            ++index;
        } else {
            current.push_back(output[index++]);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}
std::size_t count_substring(const std::string& haystack, const std::string_view needle) {
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while ((offset = haystack.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}
void test_compile_postfix_formatting() {
    mmltk::backend::data::CompileProgress progress{};
    progress.phase = mmltk::backend::data::DatasetCompilePhase::Pixels;
    progress.active_workers = 2U;
    const std::string_view phase_label = mmltk::backend::data::dataset_compile_phase_label(progress.phase);
    CHECK(spdmon::format_progress_postfix(phase_label, progress.active_workers, 0U) == "pixels 2 active |");
    CHECK(spdmon::format_progress_postfix(phase_label, progress.active_workers, 1U) == "pixels 2 active /");
    CHECK(spdmon::format_progress_postfix(phase_label, progress.active_workers, 2U) == "pixels 2 active -");
    CHECK(spdmon::format_progress_postfix(phase_label, progress.active_workers, 3U) == "pixels 2 active \\");
}
void test_progress_bar_non_tty_width() {
    mmltk::testsupport::ScopedEnvironmentVariable columns{"COLUMNS", "40"};
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});
    {
        spdmon::ProgressBar bar{"compile-progress-fallback-width-check", 10U, "img", &test_progress_now};
        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1U);
    }
    const std::vector<std::string> lines = normalize_terminal_output(capture.finish());
    REQUIRE(lines.size() == 1U);
    CHECK(lines.back().find("compile-progress-fallback-width-check") == std::string::npos);
    CHECK(lines.back().size() <= 40U);
}
void test_progress_bar_redraw_throttling() {
    mmltk::testsupport::ScopedEnvironmentVariable columns{"COLUMNS", "120"};
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});
    {
        spdmon::ProgressBar bar{"compile", 10U, "img", &test_progress_now};
        bar.set_min_render_interval(std::chrono::seconds{10});
        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1U);
        set_progress_now(std::chrono::milliseconds{500});
        bar.add(1U);
    }
    CHECK(count_substring(capture.finish(), "\r\033[2K") == 1U);
}
void test_progress_bar_log_preserves_lines() {
    mmltk::testsupport::ScopedEnvironmentVariable columns{"COLUMNS", "120"};
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});
    {
        spdmon::ProgressBar bar{"compile", 3U, "img", &test_progress_now};
        set_progress_now(std::chrono::milliseconds{250});
        bar.add(1U);
        spdmon::ProgressBar::log("worker started");
        set_progress_now(std::chrono::milliseconds{500});
        bar.close();
    }
    const std::vector<std::string> lines = normalize_terminal_output(capture.finish());
    REQUIRE(lines.size() == 2U);
    CHECK(lines.front() == "worker started");
    CHECK(lines.back().find("compile") != std::string::npos);
}
void test_progress_bar_counter_rollback() {
    mmltk::testsupport::ScopedEnvironmentVariable columns{"COLUMNS", "120"};
    ScopedStderrCapture capture;
    set_progress_now(std::chrono::milliseconds{0});
    {
        spdmon::ProgressBar bar{"archive extraction", 100U, "img", &test_progress_now};
        set_progress_now(std::chrono::milliseconds{250});
        bar.set_current(80U);
        set_progress_now(std::chrono::milliseconds{500});
        bar.set_current(20U);
    }
    const std::vector<std::string> lines = normalize_terminal_output(capture.finish());
    REQUIRE(lines.size() == 1U);
    CHECK(lines.back().find("20/100") != std::string::npos);
}
}  // namespace
TEST_CASE("test_compile_postfix_formatting", "[acceptance][compile-progress][postfix]") { test_compile_postfix_formatting(); }
TEST_CASE("test_progress_bar_non_tty_width", "[acceptance][compile-progress][width]") { test_progress_bar_non_tty_width(); }
TEST_CASE("test_progress_bar_redraw_throttling", "[acceptance][compile-progress][throttle]") { test_progress_bar_redraw_throttling(); }
TEST_CASE("test_progress_bar_log_preserves_lines", "[acceptance][compile-progress][log]") { test_progress_bar_log_preserves_lines(); }
TEST_CASE("test_progress_bar_counter_rollback", "[acceptance][compile-progress][rollback]") { test_progress_bar_counter_rollback(); }
