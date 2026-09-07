#include <poll.h>
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
                                    const std::filesystem::path& working_directory, const std::filesystem::path& firefox_root) {
    const std::string executable_path = executable.string();
    const pid_t child = ::fork();
    if (child < 0) { throw std::runtime_error(std::string{"failed to fork browser entry fixture: "} + std::strerror(errno)); }
    if (child == 0) {
        if (::chdir(working_directory.c_str()) != 0) std::_Exit(126);
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

    TemporaryFirefoxLog persistence_refusal;
    std::filesystem::create_directories(persistence_refusal.directory() / ".mmltk-data" / "gui.json");
    const EntryResult refused_settings = run_entry(entry, {}, persistence_refusal.directory(), healthy_firefox);
    REQUIRE(refused_settings.terminal == EntryTerminal::Reaped);
    REQUIRE(refused_settings.exited());
    CHECK(refused_settings.exit_code() != 0);

    TemporaryFirefoxLog firefox_refusal;
    const EntryResult refused_firefox = run_entry(entry, {}, firefox_refusal.directory(), invalid_firefox);
    REQUIRE(refused_firefox.terminal == EntryTerminal::Reaped);
    REQUIRE(refused_firefox.exited());
    CHECK(refused_firefox.exit_code() != 0);
}

}  // namespace
