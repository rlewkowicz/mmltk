#include <poll.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include "src/test_support/filesystem_test_utils.hpp"
#include <csignal>
#include <string>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "src/common/io/scoped_fd.h"
#include "src/controller/services/firefox_process_owner.h"
namespace mmltk::test {
class FirefoxProcessOwnerTestAccess final {
   public:
    static void PreferRetainedPidWait(mmltk::controller::services::FirefoxProcessOwner& owner) noexcept { owner.prefer_retained_pid_wait_for_test(); }
};
}  // namespace mmltk::test
namespace {
namespace services = mmltk::controller::services;
using mmltk::common::io::ScopedFd;
void await_child_readiness(const int descriptor) {
    pollfd readiness{.fd = descriptor, .events = POLLIN, .revents = 0};
    int ready = -1;
    do { ready = ::poll(&readiness, 1U, 5'000); } while (ready < 0 && errno == EINTR);
    REQUIRE(ready == 1);
    REQUIRE((readiness.revents & POLLIN) != 0);
    std::uint64_t fact = 0U;
    ssize_t received = -1;
    do { received = ::read(descriptor, &fact, sizeof(fact)); } while (received < 0 && errno == EINTR);
    REQUIRE(received == static_cast<ssize_t>(sizeof(fact)));
    REQUIRE(fact == 1U);
}
struct FirefoxObservations final {
    pid_t process_group = -1;
    services::FirefoxProcessLifecycle terminal{};
    std::size_t terminal_count = 0U;
    bool accept_install = true;
    static bool Install(void* context, pid_t process_group) noexcept {
        static_cast<FirefoxObservations*>(context)->process_group = process_group;
        return process_group > 0 && static_cast<FirefoxObservations*>(context)->accept_install;
    }
    static void Publish(void* context, const services::FirefoxPhysicalObservation observation) noexcept {
        auto& observations = *static_cast<FirefoxObservations*>(context);
        observations.terminal = observation.process;
        ++observations.terminal_count;
    }
    [[nodiscard]] services::FirefoxProcessObservationTarget target() noexcept {
        return {
            .context = this,
            .install_process_group = &Install,
            .submit_observation = &Publish,
        };
    }
};
[[nodiscard]] services::FirefoxProcessOwner make_firefox_owner(FirefoxObservations& observations, std::string page_url) {
    return services::FirefoxProcessOwner{
        std::filesystem::path{"/tmp/mmltk-test-import.sock"},
        {
            .executable = MMLTK_BROWSER_RUNTIME_CHILD_FIXTURE,
            .page_url = std::move(page_url),
        },
        observations.target(),
    };
}
TEST_CASE("Firefox process owner reaps a normal pidfd terminal") {
    REQUIRE(services::block_browser_runtime_signals());
    FirefoxObservations observations;
    auto owner = make_firefox_owner(observations, "normal-exit");
    REQUIRE(owner.start() == services::FirefoxProcessStartResult::ChildInstalled);
    owner.wait();
    CHECK(observations.process_group > 0);
    CHECK(owner.lifecycle().terminal == services::FirefoxProcessTerminal::Exited);
    CHECK(owner.lifecycle().status == 0);
    CHECK(owner.lifecycle().error_code == 0);
}
TEST_CASE("Firefox process owner stops and joins through its eventfd") {
    REQUIRE(services::block_browser_runtime_signals());
    FirefoxObservations observations;
    auto owner = make_firefox_owner(observations, "term-ack");
    REQUIRE(owner.start() == services::FirefoxProcessStartResult::ChildInstalled);
    owner.request_stop();
    owner.wait();
    CHECK(owner.lifecycle().settled());
    CHECK(owner.lifecycle().stop_requested);
}
TEST_CASE("Firefox process owner escalates one expired stop deadline") {
    REQUIRE(services::block_browser_runtime_signals());
    std::array<int, 2U> readiness_descriptors{};
    REQUIRE(::pipe(readiness_descriptors.data()) == 0);
    ScopedFd readiness_reader{readiness_descriptors[0]};
    ScopedFd readiness_writer{readiness_descriptors[1]};
    FirefoxObservations observations;
    services::FirefoxProcessOwner owner{
        std::filesystem::path{"/tmp/mmltk-test-import.sock"},
        {
            .executable = MMLTK_BROWSER_RUNTIME_CHILD_FIXTURE,
            .page_url = "term-refuse:" + std::to_string(readiness_writer.get()),
            .stop_grace = std::chrono::milliseconds{10},
        },
        observations.target(),
    };
    REQUIRE(owner.start() == services::FirefoxProcessStartResult::ChildInstalled);
    readiness_writer.reset();
    await_child_readiness(readiness_reader.get());
    owner.request_stop();
    owner.wait();
    CHECK(owner.lifecycle().settled());
    CHECK(owner.lifecycle().kill_selected);
    CHECK(observations.terminal_count == 1U);
}
TEST_CASE("Firefox process owner settles a rejected child installation") {
    REQUIRE(services::block_browser_runtime_signals());
    FirefoxObservations observations;
    observations.accept_install = false;
    auto owner = make_firefox_owner(observations, "term-refuse");
    CHECK(owner.start() == services::FirefoxProcessStartResult::Terminal);
    owner.wait();
    CHECK(observations.process_group > 0);
    CHECK(observations.terminal_count == 1U);
    CHECK(owner.lifecycle().settled());
    errno = 0;
    CHECK(::kill(observations.process_group, 0) == -1);
    CHECK(errno == ESRCH);
}
TEST_CASE("Firefox process custody settles through its retained PID") {
    REQUIRE(services::block_browser_runtime_signals());
    FirefoxObservations observations;
    auto owner = make_firefox_owner(observations, "term-ack");
    REQUIRE(owner.start() == services::FirefoxProcessStartResult::ChildInstalled);
    mmltk::test::FirefoxProcessOwnerTestAccess::PreferRetainedPidWait(owner);
    owner.request_stop();
    owner.wait();
    CHECK(owner.lifecycle().settled());
    CHECK(observations.terminal_count == 1U);
}
}  // namespace
TEST_CASE("browser runtime exit policy classifies every owned Firefox terminal", "[gui][services][firefox][lifecycle]") {
    using services::FirefoxProcessLifecycle;
    using services::FirefoxProcessTerminal;
    struct ExitCase final {
        FirefoxProcessLifecycle lifecycle;
        int healthy;
        int unhealthy;
    };
    const std::array cases{
        ExitCase{{.terminal = FirefoxProcessTerminal::Exited, .status = 0}, 0, 1},
        ExitCase{{.terminal = FirefoxProcessTerminal::Exited, .status = 0, .stop_requested = true}, 0, 1},
        ExitCase{{.terminal = FirefoxProcessTerminal::Exited, .status = 23}, 23, 23},
        ExitCase{{.terminal = FirefoxProcessTerminal::Exited, .status = 17, .stop_requested = true}, 17, 17},
        ExitCase{{.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM}, 128 + SIGTERM, 128 + SIGTERM},
        ExitCase{{.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true}, 0, 128 + SIGTERM},
        ExitCase{{.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true, .kill_selected = true},
                 128 + SIGTERM, 128 + SIGTERM},
        ExitCase{{.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGKILL, .stop_requested = true, .kill_selected = true},
                 128 + SIGKILL, 128 + SIGKILL},
        ExitCase{{.terminal = FirefoxProcessTerminal::StartupFailed, .status = ENOENT}, ENOENT, ENOENT},
        ExitCase{{.terminal = FirefoxProcessTerminal::StartupFailed, .status = 1}, 1, 1},
        ExitCase{{.terminal = FirefoxProcessTerminal::StartupFailed, .status = 1, .error_code = EACCES}, 1, 1},
    };
    for (const auto& value : cases) {
        CAPTURE(value.lifecycle.status, value.lifecycle.stop_requested, value.lifecycle.kill_selected, value.lifecycle.error_code);
        CHECK(services::browser_runtime_exit_status(value.lifecycle, true) == value.healthy);
        CHECK(services::browser_runtime_exit_status(value.lifecycle, false) == value.unhealthy);
    }
}
TEST_CASE("Firefox startup cause is retained separately from its mapped status") {
    REQUIRE(services::block_browser_runtime_signals());
    for (const bool refused_log : {false, true}) {
        mmltk::testsupport::ScopedTempDir root{"mmltk-firefox-startup-error"};
        const auto non_executable = root.path() / "firefox";
        {
            std::ofstream file{non_executable};
            file << "fixture without execute permission\n";
        }
        std::filesystem::permissions(non_executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        FirefoxObservations observations;
        services::FirefoxProcessOwner owner{"/tmp/mmltk-test-import.sock",
                                            {.executable = refused_log ? std::filesystem::path{MMLTK_BROWSER_RUNTIME_CHILD_FIXTURE} : non_executable,
                                             .page_url = "normal-exit",
                                             .log_file = refused_log ? root.path() : std::filesystem::path{}},
                                            observations.target()};
        CHECK(owner.start() == services::FirefoxProcessStartResult::Terminal);
        owner.wait();
        const auto lifecycle = owner.lifecycle();
        CHECK(lifecycle.terminal == services::FirefoxProcessTerminal::StartupFailed);
        CHECK(lifecycle.status == 1);
        CHECK(lifecycle.error_code == (refused_log ? EISDIR : EACCES));
        CHECK(observations.terminal == lifecycle);
        CHECK(observations.terminal_count == 1U);
        CHECK(services::browser_runtime_exit_status(lifecycle, true) == 1);
        CHECK(services::browser_runtime_exit_status(lifecycle, false) == 1);
    }
}
