#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "catch2_compat.hpp"
#include "filesystem_test_utils.hpp"
#include "linux_process_test_utils.hpp"
#include "src/common/io/file_memory.h"
#include "src/common/io/scoped_fd.h"

namespace {

using mmltk::common::io::FileHandle;
using mmltk::common::io::ScopedFd;
using mmltk::testsupport::arm_timerfd;
using mmltk::testsupport::consume_timerfd;
using mmltk::testsupport::reap_pidfd;
using mmltk::testsupport::ScopedTempDir;

constexpr auto kEntryDeadline = std::chrono::seconds{10};
constexpr auto kForcedReapDeadline = std::chrono::seconds{1};

enum class EntryTerminal : std::uint8_t {
    Reaped,
    DeadlineExpired,
    ReapDeadlineExpired,
    WaitFailed,
};

struct EntryResult final {
    EntryTerminal terminal = EntryTerminal::WaitFailed;
    int status = -1;

    [[nodiscard]] bool exited() const noexcept { return terminal == EntryTerminal::Reaped && WIFEXITED(status); }

    [[nodiscard]] int exit_code() const noexcept { return exited() ? WEXITSTATUS(status) : -1; }
};

class TemporaryFirefoxLog final {
   public:
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_.path(); }

   private:
    ScopedTempDir directory_{"mmltk-browser-runtime-entry"};
    std::filesystem::path path_ = directory_.path() / "firefox.log";
};

[[nodiscard]] EntryResult reap_entry(const int pidfd, const pid_t child) noexcept {
    const auto terminal = reap_pidfd(pidfd, child);
    if (!terminal.reaped) return {.terminal = EntryTerminal::WaitFailed};
    return {.terminal = EntryTerminal::Reaped, .status = terminal.status};
}

void terminate_and_reap(const pid_t child, const int pidfd) noexcept {
    if (pidfd < 0 || (::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0U) != 0 && errno != ESRCH)) {
        static_cast<void>(::kill(child, SIGKILL));
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

[[nodiscard]] EntryResult run_entry(const std::filesystem::path& executable, const std::filesystem::path& firefox_log,
                                    const std::filesystem::path& working_directory, const std::filesystem::path& firefox_root,
                                    const int tracing = 0, const bool integration = false) {
    const std::string executable_path = executable.string();
    const std::string diagnostics_path = (working_directory / "trace.jsonl").string();
    std::array<int, 2U> control{-1, -1};
    if (integration && ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control.data()) != 0)
        throw std::runtime_error("cannot create entry integration control socket");
    ScopedFd control_child{control[0]};
    ScopedFd control_parent{control[1]};
    const std::string control_text = std::to_string(control_child.get());
    const auto native_output = working_directory / "native-output.txt";
    const pid_t child = ::fork();
    if (child < 0) { throw std::runtime_error(std::string{"failed to fork browser entry fixture: "} + std::strerror(errno)); }
    if (child == 0) {
        control_parent.reset();
        ScopedFd output{::open(native_output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
        if (output.get() < 0 || ::dup2(output.get(), STDOUT_FILENO) < 0 || ::dup2(output.get(), STDERR_FILENO) < 0 ||
            ::unsetenv("MMLTK_LOG_LEVEL") != 0 || ::unsetenv("MMLTK_LOG_FILE") != 0 || ::unsetenv("MMLTK_LOG_DIR") != 0 ||
            ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION", integration ? "1" : "0", 1) != 0)
            std::_Exit(126);
        if (integration && (::fcntl(control_child.get(), F_SETFD, 0) != 0 ||
                            ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_EXPLORE_CONTROL_FD", control_text.c_str(), 1) != 0 ||
                            ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DATASET_SOURCE", working_directory.c_str(), 1) != 0 ||
                            ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPILED_DIRECTORY", working_directory.c_str(), 1) != 0 ||
                            ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_RESOLUTION", "512", 1) != 0))
            std::_Exit(126);
        if (::chdir(working_directory.c_str()) != 0) std::_Exit(126);
        if ((tracing == 0 ? ::unsetenv("MMLTK_GUI_TRACE_FILE") : ::setenv("MMLTK_GUI_TRACE_FILE", diagnostics_path.c_str(), 1)) != 0 ||
            ::setenv("MMLTK_GUI_PIXEL_TRACE", tracing == 2 ? "1" : "0", 1) != 0)
            std::_Exit(126);
        if (firefox_log.empty()) {
            static_cast<void>(::unsetenv("MMLTK_FIREFOX_LOG_FILE"));
        } else if (::setenv("MMLTK_FIREFOX_LOG_FILE", firefox_log.c_str(), 1) != 0) {
            std::_Exit(126);
        }
        if (::setenv("MMLTK_BROWSER_APP_ASSET_ROOT_OVERRIDE", MMLTK_BROWSER_RUNTIME_ENTRY_ASSET_ROOT, 1) != 0 ||
            ::setenv("MMLTK_FIREFOX_RUNTIME_ROOT_OVERRIDE", firefox_root.c_str(), 1) != 0) {
            std::_Exit(126);
        }
        ::execl(executable_path.c_str(), executable_path.c_str(), nullptr);
        std::_Exit(127);
    }

    control_child.reset();
    int cleanup_pidfd = -1;
    try {
        ScopedFd pidfd{static_cast<int>(::syscall(SYS_pidfd_open, child, 0U))};
        cleanup_pidfd = pidfd.get();
        if (pidfd.get() < 0) { throw std::runtime_error(std::string{"failed to open browser entry pidfd: "} + std::strerror(errno)); }
        ScopedFd deadline{::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)};
        if (deadline.get() < 0) {
            throw std::runtime_error(std::string{"failed to create browser entry deadline: "} + std::strerror(errno));
        }
        arm_timerfd(deadline.get(), std::chrono::duration_cast<std::chrono::nanoseconds>(kEntryDeadline), "browser entry deadline");

        bool forced_stop_selected = false;
        // Each turn blocks for one kernel terminal or deadline event; this never
        // samples child state or relies on a sleep-based retry interval.
        for (;;) {
            pollfd descriptors[] = {
                {.fd = pidfd.get(), .events = POLLIN, .revents = 0},
                {.fd = deadline.get(), .events = POLLIN, .revents = 0},
            };
            int ready = -1;
            do {
                ready = ::poll(descriptors, 2U, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) { throw std::runtime_error(std::string{"failed to await browser entry terminal: "} + std::strerror(errno)); }
            if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                const EntryResult reaped = reap_entry(pidfd.get(), child);
                if (reaped.terminal != EntryTerminal::Reaped) { terminate_and_reap(child, pidfd.get()); }
                return reaped;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) == 0 || !consume_timerfd(deadline.get())) { continue; }
            if (forced_stop_selected) {
                const EntryResult reaped = reap_entry(pidfd.get(), child);
                if (reaped.terminal == EntryTerminal::Reaped) {
                    return {.terminal = EntryTerminal::DeadlineExpired, .status = reaped.status};
                }
                terminate_and_reap(child, pidfd.get());
                return {.terminal = EntryTerminal::ReapDeadlineExpired};
            }
            static_cast<void>(::syscall(SYS_pidfd_send_signal, pidfd.get(), SIGKILL, nullptr, 0U));
            forced_stop_selected = true;
            arm_timerfd(deadline.get(), std::chrono::duration_cast<std::chrono::nanoseconds>(kForcedReapDeadline),
                        "browser forced-reap deadline");
        }
    } catch (...) {
        terminate_and_reap(child, cleanup_pidfd);
        throw;
    }
}

TEST_CASE("browser runtime entry redirects Firefox logs and returns startup failures", "[gui][browser-runtime][entry]") {
    const std::filesystem::path entry{MMLTK_BROWSER_RUNTIME_ENTRY_FIXTURE};
    const std::filesystem::path healthy_firefox{MMLTK_BROWSER_RUNTIME_ENTRY_FAKE_FIREFOX_ROOT};
    const std::filesystem::path invalid_firefox{MMLTK_BROWSER_RUNTIME_ENTRY_INVALID_FIREFOX_ROOT};
    TemporaryFirefoxLog firefox_log;
    const EntryResult healthy = run_entry(entry, firefox_log.path(), firefox_log.directory(), healthy_firefox);
    REQUIRE(healthy.terminal == EntryTerminal::Reaped);
    REQUIRE(healthy.exited());
    CHECK(healthy.exit_code() == 0);

    const FileHandle log = FileHandle::open_readonly(firefox_log.path().string());
    std::string log_text(log.size(), '\0');
    if (!log_text.empty()) { log.pread_all(log_text.data(), log_text.size(), 0U); }
    CHECK(log_text.find("mmltk fake Firefox stdout\n") != std::string::npos);
    CHECK(log_text.find("mmltk fake Firefox stderr\n") != std::string::npos);

    for (const bool refuse_settings : {false, true}) {
        CAPTURE(refuse_settings);
        TemporaryFirefoxLog refusal;
        if (refuse_settings) std::filesystem::create_directories(refusal.directory() / ".mmltk-data" / "gui.json");
        const EntryResult refused = run_entry(entry, {}, refusal.directory(), refuse_settings ? healthy_firefox : invalid_firefox);
        REQUIRE(refused.terminal == EntryTerminal::Reaped);
        REQUIRE(refused.exited());
        CHECK(refused.exit_code() != 0);
        CHECK(std::filesystem::file_size(refusal.directory() / "native-output.txt") == 0U);
    }
}

TEST_CASE("desktop pixel probes require explicit opt-in beyond lifecycle tracing", "[gui][browser-runtime][entry][pixel]") {
    for (int tracing = 0; tracing != 3; ++tracing) {
        TemporaryFirefoxLog output;
        const auto result = run_entry(MMLTK_BROWSER_RUNTIME_ENTRY_FIXTURE, output.path(), output.directory(),
                                      MMLTK_BROWSER_RUNTIME_ENTRY_FAKE_FIREFOX_ROOT, tracing);
        REQUIRE(result.exited());
        REQUIRE(result.exit_code() == 0);
        const auto log = FileHandle::open_readonly(output.path().string());
        std::string text(log.size(), '\0');
        if (!text.empty()) log.pread_all(text.data(), text.size(), 0U);
        const std::string expected = "mmltk fake Firefox tracing lifecycle=" + std::to_string(tracing != 0) +
                                     " pixels=" + std::to_string(tracing == 2) + " environment=" + std::to_string(tracing == 2);
        CHECK(text.find(expected) != std::string::npos);
    }
}

TEST_CASE("integration entry selects explicit complete evidence without forcing ordinary logging", "[gui][browser-runtime][entry]") {
    for (const int tracing : {0, 1}) {
        TemporaryFirefoxLog output;
        const auto result = run_entry(MMLTK_BROWSER_RUNTIME_ENTRY_FIXTURE, output.path(), output.directory(),
                                      MMLTK_BROWSER_RUNTIME_ENTRY_FAKE_FIREFOX_ROOT, tracing, true);
        REQUIRE(result.exited());
        CHECK(result.exit_code() == 0);
        CHECK(std::filesystem::file_size(output.directory() / "native-output.txt") == 0U);
        CHECK(std::filesystem::exists(output.directory() / "trace.jsonl") == (tracing != 0));
    }
    TemporaryFirefoxLog refused;
    std::filesystem::create_directory(refused.directory() / "trace.jsonl");
    const auto result = run_entry(MMLTK_BROWSER_RUNTIME_ENTRY_FIXTURE, refused.path(), refused.directory(),
                                  MMLTK_BROWSER_RUNTIME_ENTRY_FAKE_FIREFOX_ROOT, 1, true);
    REQUIRE(result.exited());
    CHECK(result.exit_code() != 0);
    CHECK(std::filesystem::file_size(refused.directory() / "native-output.txt") == 0U);
    CHECK_FALSE(std::filesystem::exists(refused.path()));
}

}  // namespace
