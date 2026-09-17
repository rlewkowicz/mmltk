#include "audit_facts.h"
#include "session.h"
#include <cstddef>
#include "src/controller/presentation/annotation_palette.h"
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <cuda.h>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/linux_process_test_utils.hpp"
#include "src/acceptance/tests/workflow_wayland_inputs.h"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/contracts/gui_settings.h"
#include "src/controller/contracts/integration_control.h"
#include "src/controller/contracts/annotation.h"
#include "src/frameworks/serialization/serialization.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/backend/data/tests/test_fixture.h"
#include "artifact_cursor.h"
#include "surface_audit.h"
#include "pixel_audit.h"
#include "native_audit.h"
#include "browser_audit.h"
namespace mmltk::acceptance::wayland {
using mmltk::common::io::ScopedFd;
using mmltk::controller::ExploreAcceptanceGate;
using mmltk::testsupport::arm_timerfd;
using mmltk::testsupport::consume_timerfd;
using mmltk::testsupport::reap_pidfd;
using mmltk::testsupport::ScopedTempDir;
constexpr int kCompiledResolution = 512;
constexpr auto kWaylandStartupDeadline = std::chrono::seconds{20};
constexpr auto kWaylandInteractionDeadline = std::chrono::seconds{6};
constexpr auto kWaylandWorkDeadline = std::chrono::seconds{15};
constexpr auto kWaylandFailureSettlementDeadline = std::chrono::seconds{5};
constexpr auto kWaylandShutdownDeadline = std::chrono::seconds{15};
[[nodiscard]] bool execution_requested() noexcept {
    const char* const requested = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION");
    return requested != nullptr && std::string_view{requested} == "1";
}
[[nodiscard]] bool gdr_transport_available() noexcept {
    if (::access("/dev/gdrdrv", R_OK | W_OK) == 0) return true;
    if (cuInit(0U) != CUDA_SUCCESS) return false;
    CUdevice device{};
    int dmabuf = 0;
    return cuDeviceGet(&device, 0) == CUDA_SUCCESS && cuDeviceGetAttribute(&dmabuf, static_cast<CUdevice_attribute>(152), device) == CUDA_SUCCESS &&
           dmabuf != 0;
}
[[nodiscard]] std::filesystem::path configured_path(const char* const name, const std::filesystem::path& fallback) {
    const char* const configured = std::getenv(name);
    return configured != nullptr && configured[0] != '\0' ? std::filesystem::absolute(configured) : std::filesystem::absolute(fallback);
}
[[nodiscard]] std::filesystem::path latest_wayland_artifact(const std::string_view filename) {
    const char* const configured_root = std::getenv("MMLTK_REPO_ROOT");
    const std::filesystem::path repository = configured_root != nullptr && configured_root[0] != '\0' ? configured_root : std::filesystem::current_path();
    return repository / "build" / "validation" / filename;
}
void prepare_latest_log(const std::filesystem::path& path, const std::string_view description, const std::string& identity) {
    std::filesystem::create_directories(path.parent_path());
    if (std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) != 0U) {
        const auto history = path.parent_path() / (path.filename().string() + ".history");
        std::filesystem::create_directories(history);
        std::filesystem::rename(path, history / (identity + (path.extension() == ".jsonl" ? ".jsonl" : ".log")));
    }
    if (!std::ofstream{path, std::ios::binary | std::ios::trunc})
        throw std::runtime_error("failed to prepare " + std::string{description} + " at " + path.string());
}
[[nodiscard]] std::filesystem::path artifact_sibling(const std::filesystem::path& native, const std::string_view suffix) {
    return native.parent_path() / (native.stem().string() + std::string{suffix});
}
void require_independent_artifacts(const std::span<const std::filesystem::path> paths) {
    for (std::size_t index = 0U; index != paths.size(); ++index) {
        const auto canonical = std::filesystem::weakly_canonical(paths[index]);
        for (std::size_t previous = 0U; previous != index; ++previous) {
            if (canonical == std::filesystem::weakly_canonical(paths[previous]) ||
                (std::filesystem::exists(paths[index]) && std::filesystem::exists(paths[previous]) &&
                 std::filesystem::equivalent(paths[index], paths[previous])))
                throw std::runtime_error("evidence artifacts must have independent writer destinations: " + paths[index].string());
        }
    }
}
void rotate_process_log_family(const std::filesystem::path& native, const std::string& identity) {
    const std::string prefix = native.stem().string() + "-mozilla-";
    const std::string application_prefix = native.stem().string() + "-application.";
    for (const auto& entry : std::filesystem::directory_iterator(native.parent_path())) {
        const auto name = entry.path().filename().string();
        if ((!name.starts_with(prefix) && !name.starts_with(application_prefix)) || !entry.is_regular_file()) continue;
        prepare_latest_log(entry.path(), "process log family", identity);
        std::filesystem::remove(entry.path());
    }
}
[[nodiscard]] std::string read_from(const std::filesystem::path& path, const std::uintmax_t offset) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end <= 0 || offset >= static_cast<std::uintmax_t>(end)) return {};
    input.seekg(static_cast<std::streamoff>(offset));
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
[[nodiscard]] std::string read_tail(const std::filesystem::path& path) {
    constexpr std::size_t maximum = 96U * 1024U;
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) return "[artifact unavailable]";
    const auto end = input.tellg();
    if (end < 0) return "[artifact size unavailable]";
    const auto size = std::min(static_cast<std::uintmax_t>(end), static_cast<std::uintmax_t>(maximum));
    input.seekg(end - static_cast<std::streamoff>(size));
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!input.read(result.data(), static_cast<std::streamsize>(size))) return "[artifact tail read failed]";
    return result;
}
[[nodiscard]] std::size_t permitted_cpu_count() noexcept {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 0U;
    return static_cast<std::size_t>(CPU_COUNT(&allowed));
}
[[nodiscard]] std::string process_status_text(const int status) {
    if (WIFEXITED(status)) return "exit " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return "signal " + std::to_string(WTERMSIG(status)) + (WCOREDUMP(status) ? " (core dumped)" : "");
    return "wait status " + std::to_string(status);
}
class BrowserHostProcess final {
   public:
    BrowserHostProcess(const std::filesystem::path& executable, const std::filesystem::path& diagnostics, const std::filesystem::path& runtime_log,
                       const std::filesystem::path& firefox_log, const std::filesystem::path& working_directory, const TerminationMode termination,
                       const mmltk::backend::data::testsupport::FixtureSpec& fixture, const std::filesystem::path& compiled_directory,
                       const std::string& viewer_scenario, const bool logging = true, const bool high_dpi = false, const bool h2d = true,
                       const bool pixel_probes = false, const std::string& probe_failure = {},
                       const mmltk::backend::data::testsupport::FixtureSpec* square_fixture = nullptr) {
        const std::string executable_text = executable.string();
        const std::string diagnostics_text = diagnostics.string();
        const std::string runtime_log_text = runtime_log.string();
        const std::string application_log_text = artifact_sibling(diagnostics, "-application.log").string();
        const std::string firefox_text = firefox_log.string();
        const std::string mozilla_text = artifact_sibling(diagnostics, "-mozilla%PID.log").string();
        const std::string dataset_source = mmltk::backend::data::testsupport::dataset_dir(fixture);
        const std::string compiled_text = compiled_directory.string();
        const std::string resolution = std::to_string(viewer_scenario == "square" ? 384 : kCompiledResolution);
        const std::string square_source = square_fixture ? mmltk::backend::data::testsupport::dataset_dir(*square_fixture) : "";
        const std::string square_compiled = square_fixture ? mmltk::backend::data::testsupport::compiled_dir(*square_fixture) : "";
        std::array<int, 2U> control_pipe{};
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control_pipe.data()) != 0)
            throw std::runtime_error("failed to create Explore acceptance control channel");
        ScopedFd control_read{control_pipe[0]};
        ScopedFd control_write{control_pipe[1]};
        const std::string control_descriptor = std::to_string(control_read.get());
        child_ = ::fork();
        if (child_ < 0) throw std::runtime_error(std::string{"failed to fork packaged browser host: "} + std::strerror(errno));
        if (child_ == 0) {
            static_cast<void>(::setpgid(0, 0));
            control_write.reset();
            ScopedFd runtime_output{::open(runtime_log_text.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC)};
            const bool configured =
                runtime_output.get() >= 0 && ::dup2(runtime_output.get(), STDOUT_FILENO) >= 0 && ::dup2(runtime_output.get(), STDERR_FILENO) >= 0 &&
                ::fcntl(control_read.get(), F_SETFD, 0) == 0 && ::chdir(working_directory.c_str()) == 0 &&
                (logging ? (::setenv("MMLTK_LOG_LEVEL", "trace", 1) == 0 && ::setenv("MMLTK_LOG_FILE", application_log_text.c_str(), 1) == 0 &&
                            ::setenv("MMLTK_GUI_TRACE_FILE", diagnostics_text.c_str(), 1) == 0 &&
                            ::setenv("MMLTK_FIREFOX_LOG_FILE", firefox_text.c_str(), 1) == 0 &&
                            ::setenv("MOZ_LOG",
                                     "WebGPU:5,Widget:5,WidgetVSync:5,WidgetWayland:5,Dmabuf:5,WidgetCompositor:5,"
                                     "nsRefreshDriver:5,PresShell:5,Clipboard:5,WidgetClipboard:5,rotate:16",
                                     1) == 0 &&
                            ::setenv("MOZ_LOG_FILE", mozilla_text.c_str(), 1) == 0 && ::setenv("RUST_BACKTRACE", "full", 1) == 0)
                         : (::unsetenv("MMLTK_LOG_LEVEL") == 0 && ::unsetenv("MMLTK_LOG_FILE") == 0 && ::unsetenv("MMLTK_LOG_DIR") == 0 &&
                            ::unsetenv("MMLTK_GUI_TRACE_FILE") == 0 && ::unsetenv("MMLTK_FIREFOX_LOG_FILE") == 0 && ::unsetenv("MOZ_LOG") == 0 &&
                            ::unsetenv("MOZ_LOG_FILE") == 0 && ::unsetenv("RUST_BACKTRACE") == 0)) &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION", "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DPI", high_dpi ? "1.5" : "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_VIEWER_SCENARIO", viewer_scenario.c_str(), 1) == 0 &&
                ::setenv("MMLTK_GUI_PIXEL_TRACE", logging && pixel_probes ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PROBE_FAILURE", probe_failure.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PIXEL_FIXTURE",
                         fixture.pixel_evidence && (viewer_scenario == "retained" || viewer_scenario == "dpi") ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPLETION_GATE", viewer_scenario == "retained" ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PENDING_SUPERSESSION", logging && (viewer_scenario == "retained" || viewer_scenario == "dpi") ? "1" : "0",
                         1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_SQUARE_SOURCE", square_source.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_SQUARE_COMPILED", square_compiled.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DATASET_SOURCE", dataset_source.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPILED_DIRECTORY", compiled_text.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_RESOLUTION", resolution.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_EXPLORE_CONTROL_FD", control_descriptor.c_str(), 1) == 0 &&
                (termination != TerminationMode::WindowClose || ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_WINDOW_CLOSE", "1", 1) == 0);
            if (!configured) std::_Exit(126);
            if (h2d)
                ::execl(executable_text.c_str(), executable_text.c_str(), nullptr);
            else
                ::execl(executable_text.c_str(), executable_text.c_str(), "--gdrcopy", nullptr);
            std::_Exit(127);
        }
        control_read.reset();
        control_write_ = std::move(control_write);
        if (::setpgid(child_, child_) != 0 && errno != EACCES && errno != ESRCH) {
            terminate();
            throw std::runtime_error(std::string{"failed to isolate browser host: "} + std::strerror(errno));
        }
        pidfd_.reset(static_cast<int>(::syscall(SYS_pidfd_open, child_, 0U)));
        if (pidfd_.get() < 0) {
            terminate();
            throw std::runtime_error(std::string{"failed to open browser host pidfd: "} + std::strerror(errno));
        }
    }
    ~BrowserHostProcess() { terminate(); }
    BrowserHostProcess(const BrowserHostProcess&) = delete;
    BrowserHostProcess& operator=(const BrowserHostProcess&) = delete;
    [[nodiscard]] int control_fd() const noexcept { return control_write_.get(); }
    [[nodiscard]] std::string_view frontend_failure() const noexcept { return frontend_failure_; }
    [[nodiscard]] bool explore_control_closed() const noexcept { return explore_control_closed_; }
    [[nodiscard]] int pidfd() const noexcept { return pidfd_.get(); }
    [[nodiscard]] bool active() const noexcept { return child_ > 0 && pidfd_.get() >= 0; }
    [[nodiscard]] int status() const noexcept { return status_; }
    void interrupt() noexcept {
        control_write_.reset();
        signal_process(SIGINT);
    }
    [[nodiscard]] bool command_explore(const std::uint8_t command) noexcept {
        if (control_write_.get() < 0) return false;
        ssize_t written = -1;
        do { written = ::send(control_write_.get(), &command, sizeof(command), MSG_NOSIGNAL); } while (written < 0 && errno == EINTR);
        if (written == sizeof(command)) return true;
        control_write_.reset();
        return false;
    }
    [[nodiscard]] std::optional<mmltk::controller::ExploreAcceptanceGate::ControlObservation> receive_explore_event() noexcept {
        if (control_write_.get() < 0) return std::nullopt;
        using Observation = ExploreAcceptanceGate::ControlObservation;
        Observation event{};
        frontend_failure_ = {};
        ssize_t consumed = -1;
        do { consumed = ::recv(control_write_.get(), control_buffer_.data(), control_buffer_.size(), MSG_TRUNC); } while (consumed < 0 && errno == EINTR);
        if (consumed >= static_cast<ssize_t>(sizeof(event)) && consumed <= static_cast<ssize_t>(control_buffer_.size())) {
            std::memcpy(&event, control_buffer_.data(), sizeof(event));
            const auto bytes = static_cast<std::size_t>(consumed) - sizeof(event);
            const bool failed = event.event == ExploreAcceptanceGate::ControlEvent::Frontend &&
                                event.slot == static_cast<std::uint64_t>(mmltk::controller::contracts::IntegrationControlKind::Failed);
            if ((bytes == 0U || failed) && (!failed || event.staging_bytes != 0U)) {
                frontend_failure_ = {control_buffer_.data() + sizeof(event), bytes};
                return event;
            }
        }
        if (consumed == 1 && static_cast<unsigned char>(control_buffer_[0]) == static_cast<unsigned char>(ExploreAcceptanceGate::ControlEvent::InitialWait))
            return event;
        explore_control_closed_ = consumed == 0;
        control_write_.reset();
        return std::nullopt;
    }
    void close_explore_control() noexcept { control_write_.reset(); }
    void retain_peer(const pid_t peer) {
        if (peer_group_ == peer && peer_pidfd_.get() >= 0) return;
        if (peer <= 0 || peer_group_ > 0) throw std::runtime_error("invalid Firefox peer custody transition");
        ScopedFd peer_pidfd{static_cast<int>(::syscall(SYS_pidfd_open, peer, 0U))};
        if (peer_pidfd.get() < 0) throw std::runtime_error(std::string{"failed to retain Firefox peer pidfd: "} + std::strerror(errno));
        const pid_t group = ::getpgid(peer);
        if (group != peer) throw std::runtime_error("Firefox peer is not the leader of its isolated process group");
        peer_group_ = group;
        peer_pidfd_ = std::move(peer_pidfd);
    }
    [[nodiscard]] bool kill_peer(const pid_t peer) noexcept {
        if (peer <= 0 || peer != peer_group_ || peer_pidfd_.get() < 0) return false;
        return ::syscall(SYS_pidfd_send_signal, peer_pidfd_.get(), SIGKILL, nullptr, 0U) == 0 || errno == ESRCH;
    }
    [[nodiscard]] int reap() noexcept {
        if (!active()) return status_;
        const auto terminal = reap_pidfd(pidfd_.get(), child_);
        if (!terminal.reaped) return -1;
        status_ = terminal.status;
        child_ = -1;
        pidfd_.reset();
        return status_;
    }
    [[nodiscard]] std::optional<int> reap_if_exited() noexcept {
        if (!active()) return status_;
        int terminal = 0;
        pid_t result = -1;
        do { result = ::waitpid(child_, &terminal, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result != child_) return std::nullopt;
        status_ = terminal;
        child_ = -1;
        pidfd_.reset();
        return status_;
    }
    void terminate() noexcept {
        control_write_.reset();
        terminate_peer();
        if (child_ > 0) {
            static_cast<void>(::kill(-child_, SIGKILL));
            int terminal = 0;
            while (::waitpid(child_, &terminal, 0) < 0 && errno == EINTR) {}
            status_ = terminal;
            child_ = -1;
            pidfd_.reset();
        }
    }

   private:
    void terminate_peer() noexcept {
        if (peer_group_ <= 0) return;
        const pid_t peer_group = std::exchange(peer_group_, -1);
        static_cast<void>(::kill(-peer_group, SIGKILL));
        if (peer_pidfd_.get() >= 0) {
            pollfd peer_exit{.fd = peer_pidfd_.get(), .events = POLLIN, .revents = 0};
            int ready = -1;
            do { ready = ::poll(&peer_exit, 1U, 1000); } while (ready < 0 && errno == EINTR);
        }
        peer_pidfd_.reset();
    }
    void signal_process(const int signal) noexcept {
        if (!active()) return;
        if (::syscall(SYS_pidfd_send_signal, pidfd_.get(), signal, nullptr, 0U) != 0 && errno != ESRCH) static_cast<void>(::kill(child_, signal));
    }
    pid_t child_ = -1;
    ScopedFd pidfd_;
    pid_t peer_group_ = -1;
    ScopedFd peer_pidfd_;
    ScopedFd control_write_;
    std::array<char, sizeof(ExploreAcceptanceGate::ControlObservation) + mmltk::controller::contracts::kIntegrationFailureMaxBytes> control_buffer_;
    std::string_view frontend_failure_;
    bool explore_control_closed_ = false;
    int status_ = -1;
};
void report_consumed_record(const nlohmann::json& record, const std::string_view source) {
    const std::string event = record.value("event", "");
    const bool protocol_failure = event.find("protocol") != std::string::npos ||
                                  (record.contains("terminal") && record.at("terminal").is_string() && record.at("terminal") == "protocol_failure");
    const bool failure = event.find("failed") != std::string::npos || event.find("failure") != std::string::npos ||
                         event.find("invalid") != std::string::npos || event.find("incomplete") != std::string::npos || protocol_failure;
    if (failure) {
        std::cerr << "workspace-wayland[" << source << "]: " << record.dump() << '\n' << std::flush;
        return;
    }
    static constexpr std::array milestones{
        std::string_view{"browser.server.started"},
        std::string_view{"browser.server.peer_opened"},
        std::string_view{"browser.server.peer_closed"},
        std::string_view{"child.spawned"},
        std::string_view{"acceptance.completion.held"},
        std::string_view{"acceptance.completion.released"},
        std::string_view{"integration.dataset_complete"},
        std::string_view{"integration.explore_reopened"},
        std::string_view{"integration.annotation_ready"},
        std::string_view{"integration.complete"},
        std::string_view{"shutdown.requested"},
        std::string_view{"shutdown.firefox_terminal"},
        std::string_view{"shutdown.complete"},
    };
    if (std::ranges::contains(milestones, event)) { std::cout << "workspace-wayland[" << source << "]: " << record.dump() << '\n' << std::flush; }
}
class ArtifactNotifications final {
   public:
    ArtifactNotifications(const std::filesystem::path& first, const std::filesystem::path& second) {
        descriptor_.reset(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
        if (descriptor_.get() < 0) throw std::runtime_error("failed to create integration artifact notifications");
        add(first.parent_path());
        if (second.parent_path() != first.parent_path()) add(second.parent_path());
    }
    [[nodiscard]] int descriptor() const noexcept { return descriptor_.get(); }
    void consume() noexcept {
        std::array<char, 4096U> buffer{};
        while (::read(descriptor_.get(), buffer.data(), buffer.size()) > 0) {}
    }

   private:
    void add(const std::filesystem::path& directory) {
        std::filesystem::create_directories(directory);
        if (::inotify_add_watch(descriptor_.get(), directory.c_str(), IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE) < 0)
            throw std::runtime_error("failed to watch integration artifacts");
    }
    ScopedFd descriptor_;
};
[[nodiscard]] std::optional<int> await_shutdown(BrowserHostProcess& process, const int deadline) {
    for (;;) {
        std::array<pollfd, 2U> descriptors{{
            {.fd = process.pidfd(), .events = POLLIN, .revents = 0},
            {.fd = deadline, .events = POLLIN, .revents = 0},
        }};
        int ready = -1;
        do { ready = ::poll(descriptors.data(), descriptors.size(), -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) throw std::runtime_error("failed to await packaged browser shutdown");
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) return process.reap();
        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            static_cast<void>(consume_timerfd(deadline));
            return std::nullopt;
        }
    }
}
void compile_wayland_fixture(const mmltk::backend::data::testsupport::FixtureSpec& fixture, const std::uint32_t resolution) {
    using namespace mmltk::backend::data;
    const auto plan = DatasetCompiler::prepare({.source_dir = testsupport::dataset_dir(fixture),
                                                .output_dir = testsupport::compiled_dir(fixture),
                                                .split = fixture.split,
                                                .target_width = resolution,
                                                .target_height = resolution,
                                                .worker_cpus = {}},
                                               {fixture.split});
    DatasetCompiler::compile(plan, 0U);
}
    PreparedWaylandInputs::PreparedWaylandInputs()
        : root_("mmltk-wayland-inputs"),
          square_{.root_dir = (root_.path() / "square").string(),
                  .split = "train",
                  .width = 768,
                  .height = 384,
                  .num_images = 128,
                  .background_images = 10,
                  .pixel_evidence = true},
          mixed_{.root_dir = (root_.path() / "mixed").string(),
                 .split = "train",
                 .width = 768,
                 .height = 384,
                 .num_images = 300,
                 .background_images = 10,
                 .pixel_evidence = true},
          probe_{.root_dir = (root_.path() / "probe").string(),
                 .split = "train",
                 .width = 768,
                 .height = 384,
                 .num_images = 3,
                 .background_images = 0,
                 .pixel_evidence = true} {
        using namespace mmltk::backend::data::testsupport;
        for (const auto* fixture : {&square_, &mixed_}) {
            create_synthetic_dataset(*fixture);
            // The top grid edge's image-side sample uses this unpadded background card.
            replace_synthetic_image(*fixture, 2, 384, 384);
            replace_synthetic_image(*fixture, 8, 192, 384);
            replace_synthetic_image(*fixture, 9, 384, 192);
            replace_synthetic_image(*fixture, 11, 192, 384);
            const auto split = std::filesystem::path(dataset_dir(*fixture)) / fixture->split;
            std::filesystem::copy_file(split / "000012.jsonl", split / "000001.jsonl", std::filesystem::copy_options::overwrite_existing);
            std::ofstream dropped_instance{split / "000013.jsonl", std::ios::app};
            if (!dropped_instance) throw std::runtime_error("cannot prepare dropped-instance compiler evidence");
            dropped_instance << R"({"class":"person","bbox_xyxy":[1,0,2,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"1:1","image_size_wh":[)"
                             << fixture->width << ',' << fixture->height << "]}\n";
        }
        replace_synthetic_image(square_, 1, 384, 384);
        // This small prerequisite is compiled once. The primary browser owns
        // the one real 512-pixel compile/control/error workflow.
        compile_wayland_fixture(square_, 384U);
    }

const mmltk::testsupport::WorkflowWaylandInputs& PreparedWaylandInputs::workflows() {
        if (!workflows_) workflows_ = std::make_unique<mmltk::testsupport::WorkflowWaylandInputs>(root_.path() / "workflows");
        return *workflows_;
    }
const mmltk::backend::data::testsupport::FixtureSpec& PreparedWaylandInputs::probe() {
        using namespace mmltk::backend::data::testsupport;
        if (!std::filesystem::is_regular_file(compiled_bin_path(probe_))) {
            create_synthetic_dataset(probe_);
            // Preserve the selected image's exact annotation and six-class
            // catalog while keeping every input in the initial measured row.
            std::filesystem::copy_file(std::filesystem::path(dataset_dir(mixed_)) / mixed_.split / "000001.jsonl",
                                       std::filesystem::path(dataset_dir(probe_)) / probe_.split / "000001.jsonl",
                                       std::filesystem::copy_options::overwrite_existing);
            compile_wayland_fixture(probe_, kCompiledResolution);
        }
        return probe_;
    }
WaylandSession::WaylandSession(std::shared_ptr<PreparedWaylandInputs> inputs, const TerminationMode terminal, std::string profile,
                               const bool diagnostics_enabled, const bool dpi, const bool host_to_device, std::string fault, const bool pixels_enabled,
                               const mmltk::backend::data::testsupport::FixtureSpec* ordinary_fixture)
    : inputs_(std::move(inputs)),
      ordinary_fixture_(ordinary_fixture ? *ordinary_fixture : inputs_->mixed()),
      termination(terminal),
      profile_(std::move(profile)),
      logging(diagnostics_enabled),
      high_dpi(dpi),
      h2d(host_to_device),
      pixel_probes(logging && pixels_enabled),
      pixel_fixture(profile_ == "retained" || profile_ == "dpi"),
      probe_failure(std::move(fault)),
      ordinary_compiled_(profile_ == "retained" ? working.path() / "compiled"
                                                : std::filesystem::path{mmltk::backend::data::testsupport::compiled_dir(ordinary_fixture_)}),
      pixel_audit(pixel_probes) {
    diagnostics =
        !logging ? quiet_artifacts.path() / "native.jsonl" : configured_path("MMLTK_GUI_TRACE_FILE", latest_wayland_artifact("latest-wayland-test.jsonl"));
    runtime_log =
        !logging ? quiet_artifacts.path() / "runtime.log" : configured_path("MMLTK_LOG_FILE", latest_wayland_artifact("latest-wayland-test-native.log"));
    firefox_log = !logging ? quiet_artifacts.path() / "firefox.log"
                           : configured_path("MMLTK_FIREFOX_LOG_FILE", latest_wayland_artifact("latest-wayland-test-firefox.log"));
    acceptance_log = artifact_sibling(diagnostics, "-acceptance.jsonl");
    const auto rotation_id = std::to_string(::getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (logging) {
        const std::array owned_artifacts{diagnostics, acceptance_log, runtime_log, firefox_log, artifact_sibling(diagnostics, "-application.log")};
        require_independent_artifacts(owned_artifacts);
        prepare_latest_log(diagnostics, "workspace Wayland native trace", rotation_id);
        prepare_latest_log(acceptance_log, "workspace Wayland parent evidence", rotation_id);
        rotate_process_log_family(diagnostics, rotation_id);
        prepare_latest_log(artifact_sibling(diagnostics, "-application.log"), "workspace Wayland application log", rotation_id);
    }
    prepare_latest_log(runtime_log, "workspace Wayland native runtime log", rotation_id);
    if (logging) prepare_latest_log(firefox_log, "workspace Wayland Firefox log", rotation_id);
    auto initial_settings = mmltk::controller::contracts::default_gui_settings_state();
    initial_settings.ui.dark_mode = false;
    initial_settings.ui.ui_scale = 1.0F;
    initial_settings.workflows.explore.h2d_dataloader = h2d;
    initial_settings.workflows.train.request.h2d_dataloader = h2d;
    if (profile_ == "workflows") inputs_->workflows().Configure(initial_settings, working.path());
    std::filesystem::create_directories(working.path() / ".mmltk-data");
    std::ofstream settings_file{working.path() / ".mmltk-data" / "gui.json"};
    REQUIRE(settings_file);
    settings_file << mmltk::controller::contracts::snapshot_gui_settings(initial_settings).dump();
    settings_file.close();
    notifications_ = std::make_unique<ArtifactNotifications>(diagnostics, firefox_log);
    process_ = std::make_unique<BrowserHostProcess>(MMLTK_TEST_MMLTK_GUI_LAUNCHER, diagnostics, runtime_log, firefox_log, working.path(), termination,
                                                    ordinary_fixture_, ordinary_compiled_, profile_ == "blocked" ? "quiet" : profile_, logging, high_dpi, h2d,
                                                    pixel_probes, probe_failure, &inputs_->square());
    deadline.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
    if (deadline.get() < 0) throw std::runtime_error(std::string{"failed to create workspace Wayland acceptance deadline: "} + std::strerror(errno));
    native_cursor_ = std::make_unique<JsonLineCursor>(diagnostics, 0U);
    browser_cursor_ = std::make_unique<JsonLineCursor>(firefox_log, 0U, JsonLineCursor::Format::FirefoxText);
}
void WaylandSession::ConsumeRecords(const bool final) {
    native_cursor_->consume(native, [&](const auto& record) {
        surface_audit.native(record);
        pixel_audit.consume(record);
        report_consumed_record(record, "native");
    });
    if (native.firefox_pid > 0) process_->retain_peer(native.firefox_pid);
    browser_cursor_->consume(
        browser,
        [&](const auto& record) {
            surface_audit.browser(record);
            pixel_audit.consume(record);
            browser.atlas_draws.failure.report(acceptance_log, "acceptance.atlas_draw.failed", firefox_log, browser_cursor_->line());
            browser.owned_atlas_failure.report(acceptance_log, "acceptance.owned_atlas.failed", firefox_log, browser_cursor_->line());
            const auto event = record.value("event", "");
            if (event == "integration.workflow.completed") workflow_steps_.insert(record.value("detail", ""));
            if (event == "integration.workflow.pixels") workflow_pixels_.insert(record.value("detail", ""));
            report_consumed_record(record, "firefox");
        },
        final);
    browser_failed_ = browser_failed_ || browser.failed_before_termination();
    for (const auto& [generation, evidence] : browser.gallery_generations) {
        const auto digest = native.placeholder_digests.find(generation);
        const auto cardinality = native.placeholder_cardinalities.find(generation);
        if ((digest != native.placeholder_digests.end() && digest->second != evidence.digest) ||
            (cardinality != native.placeholder_cardinalities.end() && cardinality->second != evidence.slots.size()))
            continue;
        native.join_gallery_publication(generation, evidence.slots);
    }
}
void WaylandSession::RunWorkflows() {
    auto& process = *process_;
    arm_timerfd(deadline.get(), kWaylandStartupDeadline, "workflow browser startup");
    std::size_t progress = 0U;
    const auto diagnostics_on_exit = [&] {
        std::cerr << "\nworkflow native log:\n" << read_tail(runtime_log) << "\nworkflow browser log:\n" << read_tail(firefox_log) << std::flush;
    };
    for (;;) {
        ConsumeRecords();
        if (browser_failed_ || native.failed_before_termination() || !surface_audit.failure.empty()) {
            diagnostics_on_exit();
            FAIL("workflow browser reported a product or physical ownership failure");
        }
        if (frontend_settled_ && surface_audit.evidence_settled()) break;
        if (browser.phase_progress_revision != progress) {
            progress = browser.phase_progress_revision;
            arm_timerfd(deadline.get(), browser.phase_progress_class == "work" ? kWaylandWorkDeadline : kWaylandInteractionDeadline,
                        "workflow phase progress");
        }
        std::array<pollfd, 4U> waits{{
            {.fd = process.pidfd(), .events = POLLIN, .revents = 0},
            {.fd = notifications_->descriptor(), .events = POLLIN, .revents = 0},
            {.fd = process.control_fd(), .events = POLLIN, .revents = 0},
            {.fd = deadline.get(), .events = POLLIN, .revents = 0},
        }};
        int ready = -1;
        do { ready = ::poll(waits.data(), waits.size(), -1); } while (ready < 0 && errno == EINTR);
        REQUIRE(ready >= 0);
        if (waits[1].revents) notifications_->consume();
        if (waits[2].revents & POLLIN) {
            const auto event = process.receive_explore_event();
            REQUIRE(event.has_value());
            REQUIRE(event->event == ExploreAcceptanceGate::ControlEvent::Frontend);
            REQUIRE(event->generation == scenario_sequence_);
            using Kind = mmltk::controller::contracts::IntegrationControlKind;
            const auto kind = static_cast<Kind>(event->slot);
            if (kind == Kind::Settled) {
                REQUIRE_FALSE(frontend_settled_);
                frontend_settled_ = true;
            } else if (kind == Kind::Failed) {
                INFO(process.frontend_failure());
                diagnostics_on_exit();
                FAIL("rendered workflow failed");
            } else {
                REQUIRE(kind == Kind::Progress);
            }
        }
        if (waits[0].revents || waits[3].revents) {
            ConsumeRecords();
            diagnostics_on_exit();
            INFO(browser.phase_progress_name);
            FAIL("rendered workflow exited or exceeded its phase deadline");
        }
    }
    CHECK((workflow_steps_ == std::set<std::string>{"train", "validation", "compiled", "image", "video", "stop", "theme", "narrow"}));
    CHECK((workflow_pixels_ == std::set<std::string>{"train", "validation", "detail", "compiled", "image", "video", "stop", "theme", "narrow"}));
    CHECK_FALSE(surface_audit.surfaces.empty());
    process.interrupt();
    arm_timerfd(deadline.get(), kWaylandShutdownDeadline, "workflow shutdown");
    const auto terminal = await_shutdown(process, deadline.get());
    REQUIRE(terminal.has_value());
    ConsumeRecords(true);
    native_cursor_->finish();
    INFO(surface_audit.joined_failure());
    CHECK(surface_audit.joined_failure().empty());
    CHECK(*terminal == 0);
    CHECK(native.peer_open_count > 0U);
    CHECK(native.peer_close_count == native.peer_open_count);
    CHECK(native.shutdown_requested);
    CHECK(native.firefox_terminal);
    CHECK(native.shutdown_complete);
    CHECK_FALSE(native.shutdown_incomplete);
    CHECK_FALSE(native.worker_failed);
    CHECK_FALSE(browser_failed_);
}
void WaylandSession::RunScenario(const std::string& viewer_scenario, const bool last, const bool dark, const bool pending_reconstruction) {
    if (viewer_scenario == "workflows") {
        RunWorkflows();
        return;
    }
    const auto permitted_cpus = permitted_cpu_count();
    const auto& fixture = viewer_scenario == "square" ? inputs_->square() : ordinary_fixture_;
    const auto compiled_directory =
        viewer_scenario == "square" ? std::filesystem::path{mmltk::backend::data::testsupport::compiled_dir(fixture)} : ordinary_compiled_;
    auto& process = *process_;
    auto& notifications = *notifications_;
    auto& native_cursor = *native_cursor_;
    auto deadline_duration = kWaylandStartupDeadline;
    std::string deadline_stage{"browser startup"};
    const auto arm_acceptance_deadline = [&](const std::chrono::seconds duration, std::string stage) {
        deadline_duration = duration;
        deadline_stage = std::move(stage);
        arm_timerfd(deadline.get(), duration, "workspace Wayland progress deadline");
    };
    arm_acceptance_deadline(kWaylandStartupDeadline, deadline_stage);
    std::cout << "workspace-wayland: " << kWaylandStartupDeadline.count() << "-second browser startup deadline armed"
              << "\nnative JSONL: " << diagnostics << "\nnative runtime log: " << runtime_log << "\nFirefox log: " << firefox_log << '\n'
              << std::flush;
    if (profile_ == "blocked" || profile_ == "quiet") {
        // The acceptance socket reports entry into the real I/O gate. This
        // shutdown case needs neither diagnostic records nor a startup delay.
        std::uint64_t progress = 0U;
        // CLEANUP-IGNORE: This wait combines acceptance commands, peer death and a phase deadline; production polls child custody, stop and
        // escalation.
        for (;;) {
            std::array<pollfd, 3U> waits{{
                {.fd = process.control_fd(), .events = POLLIN, .revents = 0},
                {.fd = process.pidfd(), .events = POLLIN, .revents = 0},
                {.fd = deadline.get(), .events = POLLIN, .revents = 0},
            }};
            int ready;
            do { ready = ::poll(waits.data(), waits.size(), -1); } while (ready < 0 && errno == EINTR);
            REQUIRE(ready > 0);
            INFO("quiet profile " << profile_ << ", stage " << deadline_stage << ", progress " << progress << ", pressure entered " << pressure_entered_);
            REQUIRE((waits[2].revents & POLLIN) == 0);
            REQUIRE((waits[1].revents & POLLIN) == 0);
            REQUIRE((waits[0].revents & POLLIN) != 0);
            const auto entered = process.receive_explore_event();
            REQUIRE(entered.has_value());
            if (entered->event == ExploreAcceptanceGate::ControlEvent::InitialWait) {
                if (profile_ == "blocked") break;
                REQUIRE(process.command_explore(2U));
            } else if (entered->event == ExploreAcceptanceGate::ControlEvent::HeldWait) {
                REQUIRE(process.command_explore(4U));
            } else if (entered->event == ExploreAcceptanceGate::ControlEvent::Frontend) {
                REQUIRE(entered->generation == scenario_sequence_);
                using Kind = mmltk::controller::contracts::IntegrationControlKind;
                const auto kind = static_cast<Kind>(entered->slot);
                if (kind == Kind::Failed)
                    FAIL("quiet integration failed after progress " << progress << ", failure receipt " << entered->compiled_index << ", frontend source line "
                                                                    << entered->staging_bytes << "\nfrontend error: " << process.frontend_failure()
                                                                    << "\nnative runtime output:\n"
                                                                    << read_tail(runtime_log));
                if (kind == Kind::PressureEntered) {
                    REQUIRE_FALSE(pressure_entered_);
                    pressure_entered_ = true;
                } else if (kind == Kind::Settled) {
                    REQUIRE(pressure_entered_);
                    frontend_settled_ = true;
                    break;
                } else if (kind == Kind::Progress) {
                    REQUIRE(entered->compiled_index > progress);
                    progress = entered->compiled_index;
                    arm_acceptance_deadline((progress & 3U) == 2U ? kWaylandWorkDeadline : kWaylandInteractionDeadline, "quiet typed workflow progress");
                } else {
                    FAIL("invalid quiet integration control direction");
                }
            }
        }
        process.interrupt();
        arm_acceptance_deadline(kWaylandShutdownDeadline, "shutdown");
        const auto terminal = await_shutdown(process, deadline.get());
        REQUIRE(terminal.has_value());
        CHECK(*terminal == 0);
        if (!logging) {
            CHECK_FALSE(std::filesystem::exists(diagnostics));
            CHECK_FALSE(std::filesystem::exists(firefox_log));
            CHECK_FALSE(std::filesystem::exists(artifact_sibling(diagnostics, "-application.log")));
            INFO("quiet runtime output: " << read_tail(runtime_log));
            CHECK(std::filesystem::file_size(runtime_log) == 0U);
        }
        return;
    }
    std::size_t observed_phase_progress = 0U;
    std::size_t observed_work_progress = 0U;
    const auto refresh_progress_deadline = [&] {
        const bool phase_changed = browser.phase_progress_revision != observed_phase_progress;
        const bool work_advanced = browser.phase_progress_class == "work" && browser.work_progress_revision != observed_work_progress;
        if (!phase_changed && !work_advanced) return false;
        observed_phase_progress = browser.phase_progress_revision;
        observed_work_progress = browser.work_progress_revision;
        const auto duration = browser.phase_progress_class == "work"      ? kWaylandWorkDeadline
                              : browser.phase_progress_class == "startup" ? kWaylandStartupDeadline
                                                                          : kWaylandInteractionDeadline;
        arm_acceptance_deadline(duration, "integration phase " + browser.phase_progress_name);
        if (observed_phase_progress == 1U || observed_phase_progress % 25U == 0U)
            std::cout << "workspace-wayland: progress " << browser.phase_progress_name << " (phase " << observed_phase_progress << ")\n" << std::flush;
        return true;
    };
    const auto report_artifacts = [&] {
        std::cerr << "\nnative diagnostics:\n"
                  << read_tail(diagnostics) << "\nnative runtime log:\n"
                  << read_tail(runtime_log) << "\nFirefox diagnostics:\n"
                  << read_tail(firefox_log) << '\n';
    };
    bool exited_early = false;
    bool released_one_lane = false;
    bool released_remaining_lanes = false;
    const bool recover_before_reads = !probe_failure.empty() && probe_failure != "allocation";
    bool recovery_redraw_requested = false;
    bool released_held_read = false;
    bool held_read_waiting = false;
    bool visible_read_armed = false;
    std::optional<ExploreAcceptanceGate::ControlObservation> visible_read_held;
    bool visible_read_released = false;
    bool capacity_armed = false;
    bool capacity_available = false;
    std::optional<ExploreAcceptanceGate::ControlObservation> capacity_held;
    std::optional<std::string> supersession_held;
    bool supersession_released = false;
    bool terminal_failure_observed = false;
    std::uint64_t released_placeholder_generation = 0U;
    NativeAudit::FinalCursorGenerations final_generations;
    const auto command_explore = [&](const std::uint8_t command, const std::string_view stage) {
        if (!process.command_explore(command)) {
            std::cerr << "workspace-wayland: Explore control channel closed during " << stage;
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland Explore control command failed");
        }
        std::cout << "workspace-wayland: " << stage << '\n' << std::flush;
    };
    const auto rendered_padding_exported = [&](const NativeAudit::PaddingOrientation orientation) {
        return std::ranges::any_of(native.rendered_probe_frames, [&](const auto& publication) {
            const auto slots = native.placeholder_slots.find(publication.first.first);
            return native.padding_orientations.contains(publication.first) && native.padding_orientations.at(publication.first) == orientation &&
                   native.aligned_rendered_probe(publication.first) && slots != native.placeholder_slots.end() &&
                   std::ranges::any_of(publication.second,
                                       [&](const auto frame_revision) { return browser.rendered_frame_for_slots(slots->second, frame_revision, 0U); });
        });
    };
    const auto seeded_augmentation_ready = [&] {
        return std::ranges::all_of(std::array{0U, 1U}, [&](const auto seed) {
            const auto browser_frame = browser.augmentation_frames.find(seed);
            if (browser_frame == browser.augmentation_frames.end()) return false;
            const auto generation = native.augmentation_generation_for(seed, browser_frame->second);
            if (!generation) return false;
            const auto slots = native.placeholder_slots.find(*generation);
            return slots != native.placeholder_slots.end() && browser.rendered_frame_for_slots(slots->second, browser_frame->second, 0U);
        });
    };
    const auto terminal_annotation_ready = [&] {
        return viewer_scenario != "terminal" ||
               (native.annotation_copied && native.annotation_opened && native.annotation_edited && browser.annotation_ready && browser.annotation_tool &&
                browser.annotation_pointer && browser.complete && browser.surface_draws.contains(browser.presentation_receipt) &&
                browser.surface_redraws.contains(browser.presentation_receipt));
    };
    const auto acceptance_blockers = [&] {
        std::vector<std::string> blockers;
        const auto add = [&blockers](const std::string_view owner, const std::string_view blocker) {
            if (!blocker.empty()) blockers.emplace_back(std::string{owner} + ": " + std::string{blocker});
        };
        add("surface", surface_audit.failure);
        add("pixel", pixel_audit.failure);
        if (viewer_scenario.empty()) {
            add("native", native.readiness_blocker(final_generations, seeded_augmentation_ready(), pixel_fixture));
            add("browser", browser.readiness_blocker());
        } else {
            if (!native.presentation_ready || !native.explore_rendered) blockers.emplace_back("native: viewer product");
            if (!browser.viewer_complete || !browser.surface_draws.contains(browser.viewer_presentation))
                blockers.emplace_back("browser: completed viewer draw");
            if (!terminal_annotation_ready()) blockers.emplace_back("native/browser: terminal Annotation lifecycle");
        }
        if (pixel_fixture) {
            if (!rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical)) blockers.emplace_back("rendered probe: vertical padding export");
            if (!rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal)) blockers.emplace_back("rendered probe: horizontal padding export");
        }
        if (pixel_probes && !pixel_audit.raw_complete()) blockers.emplace_back("pixel: physical 25-sample chain");
        if (pixel_probes && !pixel_audit.viewer_nonblack_complete()) blockers.emplace_back("pixel: nonblack viewer");
        if (pixel_probes && viewer_scenario == "square" && !pixel_audit.retained_logical_content) blockers.emplace_back("pixel: retained logical content");
        if (pixel_probes && viewer_scenario == "square" && !pixel_audit.upscale_growth) blockers.emplace_back("pixel: four-times allocation growth");
        if (!pixel_audit.probe_failure_complete(probe_failure)) blockers.emplace_back("pixel: injected probe-failure recovery");
        if (!frontend_settled_) blockers.emplace_back("frontend: typed scenario settlement");
        std::string surface_blocker;
        if (!surface_audit.evidence_settled(&surface_blocker)) add("surface", surface_blocker);
        if (pending_reconstruction && !surface_audit.pending_supersession_completed()) blockers.emplace_back("surface: completed pending-candidate handoff");
        if ((viewer_scenario.empty() || viewer_scenario == "copy" || viewer_scenario == "semantics") &&
            !pixel_audit.continuity_complete(viewer_scenario.empty()))
            blockers.emplace_back("pixel: copy and semantic continuity");
        if (pixel_fixture && !pixel_audit.composition_complete()) blockers.emplace_back("pixel: atlas fixture composition");
        return blockers;
    };
    const auto blocker_summary = [&] {
        const auto blockers = acceptance_blockers();
        std::string summary;
        for (const auto& blocker : blockers) {
            if (!summary.empty()) summary.append("; ");
            summary.append(blocker);
        }
        return summary;
    };
    const auto record_acceptance_state = [&](const std::string_view event, const bool terminal) {
        if (!logging) return;
        append_acceptance_record(acceptance_log, {{"event", event},
                                                  {"terminal", terminal},
                                                  {"termination", termination_label(termination)},
                                                  {"deadline_stage", deadline_stage},
                                                  {"deadline_seconds", deadline_duration.count()},
                                                  {"work_progress_revision", browser.work_progress_revision},
                                                  {"blockers", acceptance_blockers()},
                                                  {"final_material_generation", final_generations.material},
                                                  {"final_cursor_generation", final_generations.cursor},
                                                  {"browser_final_generation", browser.final_cursor_generation},
                                                  {"browser_final_frame_revision", browser.final_cursor_frame_revision},
                                                  {"browser_final_slot_count", browser.final_cursor_slot_count},
                                                  {"held_generation", native.held_generation},
                                                  {"held_slot", native.held_slot},
                                                  {"held_compiled_index", native.held_compiled_index},
                                                  {"held_staging_bytes", native.held_capacity}});
    };
    bool readiness_reached = false;
    for (;;) {
        ConsumeRecords();
        if (!terminal_failure_observed) static_cast<void>(refresh_progress_deadline());
        if (!terminal_failure_observed && frontend_settled_ && !browser_failed_ && !viewer_scenario.empty() && browser.viewer_complete &&
            browser.surface_draws.contains(browser.viewer_presentation) && native.presentation_ready && native.explore_rendered &&
            surface_audit.failure.empty() && pixel_audit.failure.empty() &&
            (!pixel_probes || (pixel_audit.raw_complete() && pixel_audit.viewer_nonblack_complete())) && pixel_audit.probe_failure_complete(probe_failure) &&
            surface_audit.evidence_settled() && terminal_annotation_ready() && (!pixel_fixture || pixel_audit.composition_complete()) &&
            ((viewer_scenario != "copy" && viewer_scenario != "semantics") || pixel_audit.continuity_complete(false)) &&
            (!pending_reconstruction || surface_audit.pending_supersession_completed()) &&
            (viewer_scenario != "square" || !pixel_probes || (pixel_audit.retained_logical_content && pixel_audit.upscale_growth))) {
            readiness_reached = true;
            break;
        }
        if (const auto* slots = browser.final_cursor_slots()) {
            if (const auto identified = native.final_generations_for(*slots, browser.final_cursor_generation, browser.final_cursor_frame_revision))
                final_generations = *identified;
        }
        const auto recovery = pixel_audit.probe_failure_evidence(probe_failure);
        if (supersession_held && !supersession_released && surface_audit.pending_fallback(*supersession_held)) {
            command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ReleasePendingSupersession),
                            "released pending arena after the exact retained fallback draw settled");
            supersession_released = true;
        }
        if (recover_before_reads && !recovery_redraw_requested && recovery.forwarded) {
            command_explore(16U, "requested exact-content probe recovery");
            recovery_redraw_requested = true;
        }
        const bool reads_releasable = !recover_before_reads || pixel_audit.copy_probe_omission_proven() || (recovery_redraw_requested && recovery.complete());
        if (reads_releasable && !released_one_lane && browser.explore_ready) {
            const auto rendered_placeholder = std::ranges::find_if(native.placeholder_slots, [this](const auto& placeholder) {
                const auto cardinality = native.placeholder_cardinalities.find(placeholder.first);
                return cardinality != native.placeholder_cardinalities.end() && cardinality->second == placeholder.second.size() &&
                       !placeholder.second.empty() && browser.rendered_frame_for_slots(placeholder.second, 0U, 0U);
            });
            if (rendered_placeholder != native.placeholder_slots.end()) {
                command_explore(1U, "released one Explore lane");
                released_placeholder_generation = rendered_placeholder->first;
                released_one_lane = true;
            }
        }
        if (reads_releasable && !released_remaining_lanes && native.acceptance_first_patch_exact) {
            command_explore(2U, "released remaining Explore lanes");
            if (released_placeholder_generation == 0U) released_placeholder_generation = native.partial_generation;
            // A ready prefetch can satisfy the partial-patch evidence before
            // the first placeholder is drawn. Releasing all also releases one.
            released_one_lane = true;
            released_remaining_lanes = true;
        }
        if (reads_releasable && released_one_lane && !released_remaining_lanes && !native.acceptance_first_patch_exact &&
            native.explore_generation > released_placeholder_generation && native.placeholder_cardinalities.contains(native.explore_generation)) {
            command_explore(1U, "reissued one Explore lane for superseding generation");
            released_placeholder_generation = native.explore_generation;
        }
        if (!released_held_read && !viewer_scenario.empty() && (native.acceptance_held_read || held_read_waiting)) {
            command_explore(4U, "released held Explore read");
            released_held_read = true;
        }
        if (viewer_scenario.empty() && native.acceptance_held_stale) released_held_read = true;
        const bool rendered_probe_exported = !pixel_fixture || (rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical) &&
                                                                rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal));
        const bool expected_window_close = termination == TerminationMode::WindowClose &&
                                           native.product_completed(final_generations, seeded_augmentation_ready(), pixel_fixture) && !native.active_peer();
        const bool terminal_failure = (native.failed_before_termination() && !expected_window_close) || browser.failed_before_termination() ||
                                      !surface_audit.failure.empty() || !pixel_audit.failure.empty();
        if (terminal_failure && !terminal_failure_observed) {
            std::cerr << "workspace-wayland failed before readiness"
                      << "\nnative failure: " << native.failure_blocker() << "\nbrowser failure: " << browser.failure_blocker()
                      << "\nacceptance blockers: " << blocker_summary();
            record_acceptance_state("acceptance.readiness.blocked", true);
            report_artifacts();
            if (const auto terminal = process.reap_if_exited()) {
                std::cerr << "native host terminal status: " << process_status_text(*terminal) << '\n';
                std::cerr << std::flush;
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            std::cerr << "native host remained active at browser failure; draining evidence to the acceptance deadline\n";
            std::cerr << std::flush;
            terminal_failure_observed = true;
            arm_acceptance_deadline(kWaylandFailureSettlementDeadline, "terminal failure settlement");
        }
        if (!terminal_failure_observed && frontend_settled_ && !browser_failed_ &&
            (native.product_ready(final_generations, seeded_augmentation_ready(), pixel_fixture) || expected_window_close) && browser.product_ready() &&
            rendered_probe_exported && (!pixel_probes || (pixel_audit.raw_complete() && pixel_audit.viewer_nonblack_complete())) &&
            pixel_audit.probe_failure_complete(probe_failure) && surface_audit.evidence_settled() && (!pixel_fixture || pixel_audit.composition_complete()) &&
            pixel_audit.continuity_complete(true) && (!pending_reconstruction || surface_audit.pending_supersession_completed())) {
            readiness_reached = true;
            break;
        }
        if (viewer_scenario.empty() && !terminal_failure_observed && browser.terminal_evidence_settled() && !browser.product_ready()) {
            const auto blocker = browser.readiness_blocker();
            std::cerr << "workspace-wayland browser terminal evidence is incomplete: " << blocker;
            record_acceptance_state("acceptance.readiness.blocked", true);
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland browser terminal evidence is incomplete");
        }
        // CLEANUP-IGNORE: This acceptance loop polls process, JSONL, and deadline descriptors; production polls
        // independent child-custody, stop, and timer descriptors.
        std::array<pollfd, 4U> descriptors{{
            {.fd = process.pidfd(), .events = POLLIN, .revents = 0},
            {.fd = notifications.descriptor(), .events = POLLIN, .revents = 0},
            {.fd = process.control_fd(), .events = POLLIN, .revents = 0},
            {.fd = deadline.get(), .events = POLLIN, .revents = 0},
        }};
        int ready = -1;
        do { ready = ::poll(descriptors.data(), descriptors.size(), -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            std::cerr << "workspace-wayland failed to poll product evidence: " << std::strerror(errno);
            record_acceptance_state("acceptance.poll.failed", true);
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland evidence poll failed");
        }
        if ((descriptors[3].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            static_cast<void>(consume_timerfd(deadline.get()));
            ConsumeRecords();
            if (!terminal_failure_observed && refresh_progress_deadline()) continue;
            if (terminal_failure_observed) {
                std::cerr << "workspace-wayland terminal failure did not settle within " << deadline_duration.count() << " seconds";
                record_acceptance_state("acceptance.failure_settlement.stalled", true);
                report_artifacts();
                std::cerr << std::flush;
                process.terminate();
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            std::cerr << "workspace-wayland made no progress within " << deadline_duration.count() << " seconds"
                      << "\nstalled stage: " << deadline_stage << "\ntermination: " << termination_label(termination)
                      << "\nacceptance blockers: " << blocker_summary();
            record_acceptance_state("acceptance.readiness.stalled", true);
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland acceptance phase exceeded its progress deadline");
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) notifications.consume();
        if ((descriptors[2].revents & POLLIN) != 0) {
            const auto event = process.receive_explore_event();
            if (!event) {
                // A successful child teardown can close the sequenced-packet
                // control peer before its pidfd becomes readable. Let the
                // process/readiness path judge that terminal transition.
                if (process.explore_control_closed()) continue;
                record_acceptance_state("acceptance.control.failed", true);
                process.terminate();
                FAIL("workspace Wayland Explore control event was invalid");
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::PendingSupersessionHeld) {
                REQUIRE(pending_reconstruction);
                REQUIRE_FALSE(supersession_held.has_value());
                REQUIRE((event->source_high != 0U || event->source_low != 0U));
                supersession_held = SurfaceAudit::native_identity({{"surface_high", event->source_high}, {"surface_low", event->source_low}});
                continue;
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::VisibleReadHeld) {
                REQUIRE(visible_read_armed);
                REQUIRE_FALSE(visible_read_held.has_value());
                visible_read_held = *event;
                if (logging)
                    append_acceptance_record(
                        acceptance_log,
                        {{"event", "acceptance.visible_read.held"}, {"generation", event->generation}, {"compiled_index", event->compiled_index}});
                continue;
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld ||
                event->event == ExploreAcceptanceGate::ControlEvent::NativeCapacityAvailable) {
                REQUIRE(capacity_armed);
                REQUIRE((event->source_high != 0U || event->source_low != 0U));
                REQUIRE(event->transfer != 0U);
                REQUIRE(event->publication != 0U);
                if (logging)
                    append_acceptance_record(acceptance_log, {{"event", event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld
                                                                            ? "acceptance.native_completion.held"
                                                                            : "acceptance.native_capacity.available"},
                                                              {"workspace_source_high", event->source_high},
                                                              {"workspace_source_low", event->source_low},
                                                              {"transfer_sequence", event->transfer},
                                                              {"presentation_revision", event->publication}});
                if (event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld) {
                    REQUIRE_FALSE(capacity_held.has_value());
                    capacity_held = *event;
                } else {
                    REQUIRE(capacity_held.has_value());
                    REQUIRE_FALSE(capacity_available);
                    REQUIRE(event->source_high == capacity_held->source_high);
                    REQUIRE(event->source_low == capacity_held->source_low);
                    REQUIRE(event->transfer == capacity_held->transfer);
                    REQUIRE(event->publication == capacity_held->publication);
                    capacity_available = true;
                    command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ReleaseNativeCompletion),
                                    "process held native completion after exact Available receipt");
                }
                continue;
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::Frontend) {
                REQUIRE(event->generation == scenario_sequence_);
                using Kind = mmltk::controller::contracts::IntegrationControlKind;
                const auto kind = static_cast<Kind>(event->slot);
                if (kind == Kind::VisibleReadArmRequested) {
                    REQUIRE_FALSE(visible_read_armed);
                    visible_read_armed = true;
                    command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ArmVisibleRead),
                                    "arm the exact visible compiled-image read");
                } else if (kind == Kind::VisibleReadReleaseRequested) {
                    REQUIRE(visible_read_held.has_value());
                    REQUIRE_FALSE(visible_read_released);
                    visible_read_released = true;
                    command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ReleaseVisibleRead),
                                    "release visible miss after pending hover, selection and gallery return");
                } else if (kind == Kind::CapacityArmRequested) {
                    REQUIRE_FALSE(capacity_armed);
                    capacity_armed = true;
                    command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ArmNativeCompletion),
                                    "arm native release-completion hold after both sample slots are occupied");
                } else if (kind == Kind::Settled) {
                    REQUIRE_FALSE(frontend_settled_);
                    frontend_settled_ = true;
                } else if (kind == Kind::Failed) {
                    terminal_failure_observed = true;
                    arm_acceptance_deadline(kWaylandFailureSettlementDeadline, "typed frontend failure");
                } else {
                    REQUIRE(kind == Kind::Progress);
                }
                continue;
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::HeldWait) held_read_waiting = true;
            native.RecordHeldControlObservation(*event);
            if (logging) {
                const auto control_event = event->event == ExploreAcceptanceGate::ControlEvent::HeldWait      ? "acceptance.control.held_wait"
                                           : event->event == ExploreAcceptanceGate::ControlEvent::HeldProceed ? "acceptance.control.held_proceed"
                                           : event->event == ExploreAcceptanceGate::ControlEvent::HeldStale   ? "acceptance.control.held_stale"
                                                                                                              : "acceptance.control.initial_wait";
                append_acceptance_record(acceptance_log, {{"event", control_event},
                                                          {"sequence", event->generation},
                                                          {"value", event->slot},
                                                          {"detail", event->compiled_index},
                                                          {"staging_bytes", event->staging_bytes}});
            }
            if (native.causal_inconsistent) {
                record_acceptance_state("acceptance.control.failed", true);
                process.terminate();
                FAIL("workspace Wayland Explore control event had inconsistent identity");
            }
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ConsumeRecords();
            if (terminal_failure_observed) {
                std::cerr << "workspace-wayland native host settled after a terminal integration failure";
                record_acceptance_state("acceptance.failure_settlement.completed", true);
                report_artifacts();
                std::cerr << std::flush;
                const int terminal = process.reap();
                std::cerr << "native host terminal status: " << process_status_text(terminal) << '\n' << std::flush;
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            const bool terminal_window_close = termination == TerminationMode::WindowClose &&
                                               native.product_completed(final_generations, seeded_augmentation_ready(), pixel_fixture) && !native.active_peer();
            exited_early =
                !((native.product_ready(final_generations, seeded_augmentation_ready(), pixel_fixture) || terminal_window_close) && browser.product_ready());
            if (exited_early) {
                std::cerr << "workspace-wayland child exited before readiness";
                record_acceptance_state("acceptance.readiness.blocked", true);
                report_artifacts();
                std::cerr << std::flush;
                const int terminal = process.reap();
                std::cerr << "native host terminal status: " << process_status_text(terminal) << '\n' << std::flush;
            }
            break;
        }
    }
    bool peer_killed = false;
    if (readiness_reached) record_acceptance_state("acceptance.readiness.completed", false);
    REQUIRE(readiness_reached);
    REQUIRE(frontend_settled_);
    REQUIRE_FALSE(browser_failed_);
    if (last) {
        if (termination == TerminationMode::SignalInterrupt)
            process.interrupt();
        else if (termination == TerminationMode::AbruptPeerLoss)
            peer_killed = process.kill_peer(native.firefox_pid);
        else
            REQUIRE(process.command_explore(8U));
        arm_acceptance_deadline(kWaylandShutdownDeadline, "shutdown");
    }
    const auto terminal_result = !last              ? std::optional<int>{0}
                                 : process.active() ? await_shutdown(process, deadline.get())
                                                    : std::optional<int>{process.status()};
    if (!terminal_result) {
        ConsumeRecords();
        std::cerr << "workspace-wayland shutdown made no progress within " << deadline_duration.count() << " seconds"
                  << "\ntermination: " << termination_label(termination) << "\nacceptance blockers: " << blocker_summary();
        record_acceptance_state("acceptance.shutdown.stalled", true);
        report_artifacts();
        std::cerr << std::flush;
        process.terminate();
        FAIL("workspace Wayland acceptance exceeded its configured deadline during shutdown");
    }
    const int terminal = *terminal_result;
    // Keep the settled prefix fixed through scenario checks and rollover.
    // Later records remain in the retained cursors for the next scenario.
    if (last) {
        ConsumeRecords(true);
        native_cursor.finish();
    }
    const std::string native_text = read_tail(diagnostics);
    const std::string browser_text = read_tail(firefox_log);
    INFO("termination: " << termination_label(termination));
    INFO("native diagnostics: " << native_text);
    INFO("native runtime log: " << read_tail(runtime_log));
    INFO("Firefox diagnostics: " << browser_text);
    INFO("native readiness blocker: " << native.readiness_blocker(final_generations, seeded_augmentation_ready(), pixel_fixture));
    INFO("browser readiness blocker: " << browser.readiness_blocker());
    INFO("browser failure: " << browser.failure_blocker());
    INFO("atlas draw first failure: " << browser.atlas_draws.failure.record().dump());
    INFO("owned atlas first failure: " << browser.owned_atlas_failure.record().dump());
    CHECK_FALSE(browser_failed_);
    CHECK_FALSE(browser.failed_before_termination());
    REQUIRE(terminal >= 0);
    CHECK(native.peer_open_count > 0U);
    if (last) {
        CHECK(native.peer_close_count == native.peer_open_count);
        if (termination == TerminationMode::SignalInterrupt) CHECK(native.peer_closed_after_shutdown);
        INFO("surface identity join: " << surface_audit.joined_failure());
        CHECK(surface_audit.joined_failure().empty());
        CHECK(native.shutdown_requested);
        CHECK(native.firefox_terminal);
        CHECK(native.shutdown_complete);
        CHECK_FALSE(native.shutdown_incomplete);
        CHECK_FALSE(native.worker_failed);
        if (termination == TerminationMode::AbruptPeerLoss) {
            CHECK(peer_killed);
            REQUIRE(WIFEXITED(terminal));
            CHECK(WEXITSTATUS(terminal) != 0);
        } else {
            CHECK(terminal == 0);
        }
    }
    INFO("pixel boundary evidence: " << pixel_audit.failure);
    CHECK(pixel_audit.failure.empty());
    CHECK(pixel_audit.probe_failure_complete(probe_failure));
    if (pixel_probes) {
        CHECK(pixel_audit.raw_complete());
        CHECK((pixel_audit.direct_joined != 0U || std::ranges::all_of(pixel_audit.joined, [](const auto count) { return count != 0U; })));
        CHECK(pixel_audit.canvas_seen);
        CHECK(pixel_audit.viewer_nonblack_complete());
    } else {
        CHECK(pixel_audit.samples.empty());
        CHECK_FALSE(pixel_audit.canvas_seen);
    }
    if (pixel_fixture) CHECK(pixel_audit.composition_complete());
    if (pixel_probes && viewer_scenario == "square") {
        CHECK(pixel_audit.retained_logical_content);
        CHECK(pixel_audit.upscale_growth);
    }
    if (viewer_scenario == "copy" || viewer_scenario == "semantics" || viewer_scenario.empty()) CHECK(pixel_audit.continuity_complete(viewer_scenario.empty()));
    CHECK_FALSE(browser.owned_atlas_interrupted);
    CHECK(browser.initial_atlas_complete);
    if (viewer_scenario == "terminal") {
        CHECK(terminal_annotation_ready());
        CHECK(browser.bounds_valid);
        CHECK_FALSE(native.failed_before_termination());
        CHECK(browser.presentation_receipt != 0U);
        CHECK(browser.surface_draws.contains(browser.presentation_receipt));
        CHECK(browser.surface_redraws.contains(browser.presentation_receipt));
    }
    if (pending_reconstruction) {
        CHECK(surface_audit.pending_supersession_completed());
        for (const auto& [id, surface] : surface_audit.surfaces)
            if (surface.candidate_withdrawn && surface.reconstruction) CHECK(browser.renderer_reconstructions[id] == 1U);
    }
    if (!viewer_scenario.empty()) {
        if (last && termination != TerminationMode::AbruptPeerLoss) CHECK(terminal == 0);
        CHECK(browser.viewer_complete);
        if (viewer_scenario == "rapid") {
            CHECK(browser.gallery_no_input_complete);
            CHECK(browser.owned_atlas_seen);
            CHECK(browser.atlas_draws.valid);
            CHECK((browser.atlas_draws.stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "row9", "end", "restored"}));
            CHECK(browser.atlas_draws.drawn_rows.contains(1U));
            CHECK(browser.atlas_draws.drawn_rows.contains(2U));
            CHECK(browser.atlas_draws.drawn_rows.contains(10U));
            CHECK(browser.atlas_draws.grid_round_trip);
            CHECK(browser.atlas_geometry_valid);
            CHECK_FALSE(browser.atlas_scaled_frames.empty());
            CHECK(browser.atlas_native_capacity);
            CHECK(browser.atlas_draws.return_round_trip);
            CHECK(browser.atlas_draws.resize_stages.size() == 3U);
            CHECK((browser.detail_resize_measurements == std::set<std::uint64_t>{0U, 1U, 2U, 3U}));
            CHECK((browser.measured_resize_returns == std::set<std::string>{"landscape", "portrait-return", "landscape-return"}));
            if (profile_ == "retained") {
                REQUIRE(visible_read_held.has_value());
                REQUIRE(visible_read_released);
                REQUIRE(browser.pending_hover_index == visible_read_held->compiled_index);
                REQUIRE_FALSE(browser.shared_explore_motion.empty());
                REQUIRE(browser.pending_selection_index == visible_read_held->compiled_index);
                REQUIRE(browser.pending_read_generation == visible_read_held->generation);
                REQUIRE(browser.atlas_draws.away_return);
                REQUIRE(native.admission_seen);
                REQUIRE(native.admission_priority_valid);
                REQUIRE(native.cached_first_valid);
                std::array<bool, 2U> cached_scroll_pixels{};
                for (const auto& [generation, cached] : native.initial_cache) {
                    if (cached.slots.empty() || cached.restored_ordinal == 0U) continue;
                    const auto frames = native.published_frames.find(generation);
                    if (frames == native.published_frames.end() || frames->second.empty()) continue;
                    const auto first_frame = frames->second.front().second;
                    for (const auto& [sample, acquisition] : browser.atlas_draws.acquisitions) {
                        if (scalar(acquisition, "frame_revision") != first_frame) continue;
                        const auto pixels = browser.atlas_draws.ready_cell_samples.find(sample);
                        if (pixels == browser.atlas_draws.ready_cell_samples.end()) continue;
                        const auto& slots = native.placeholder_slots.at(generation);
                        if (std::ranges::any_of(cached.slots, [&](auto slot) {
                                const auto image = slots.find(slot);
                                return image != slots.end() && pixels->second.contains(image->second);
                            }))
                            cached_scroll_pixels[cached.forward ? 0U : 1U] = true;
                    }
                }
                CHECK(cached_scroll_pixels[0U]);
                CHECK(cached_scroll_pixels[1U]);
                REQUIRE(native.admitted_reads.contains({visible_read_held->generation, visible_read_held->compiled_index}));
                REQUIRE(browser.atlas_draws.held_stages.size() == 6U);
                for (const auto& [stage, draw] : browser.atlas_draws.held_stages) {
                    CAPTURE(stage);
                    const auto& indices = draw["visible_indices"];
                    const auto held = std::ranges::find(indices, nlohmann::json(visible_read_held->compiled_index));
                    REQUIRE(held != indices.end());
                    const auto slot = static_cast<std::size_t>(held - indices.begin());
                    REQUIRE(slot < draw["ready_slots"].size());
                    CHECK(draw["ready_slots"][slot] == (stage == "held-complete"));
                    if (stage == "held-visible") CHECK(browser.held_placeholder_motion(visible_read_held->compiled_index, visible_read_held->generation));
                }
                REQUIRE(capacity_held.has_value());
                REQUIRE(capacity_available);
                const auto source = SurfaceAudit::native_identity({{"surface_high", capacity_held->source_high}, {"surface_low", capacity_held->source_low}});
                bool exact_retry = false;
                for (const auto& [arena, state] : surface_audit.surfaces) {
                    const auto copied = state.receipts.find(browser.capacity_retry_publication);
                    if (copied == state.receipts.end()) continue;
                    const auto first = state.transfers.find({capacity_held->publication, capacity_held->transfer});
                    const auto retry = state.transfers.find({browser.capacity_retry_publication, copied->second.transfer});
                    exact_retry = copied->second.stage == 3U && !copied->second.release_only && first != state.transfers.end() &&
                                  retry != state.transfers.end() && first->second.read.source == source && first->second.releasing && first->second.released &&
                                  retry->second.releasing && retry->second.released && browser.capacity_retry_publication >= capacity_held->publication &&
                                  browser.capacity_retry_frame == copied->second.frame;
                    if (exact_retry) break;
                }
                CHECK(exact_retry);
                if (logging) {
                    std::uint64_t workspace_bytes = 0U, workspaces = 0U, arenas = 0U, copies = 0U, direct_reads = 0U, offers = 0U;
                    std::uint64_t release_only_reads = 0U;
                    std::uint64_t encodings = 0U, settlements = 0U, abandonments = 0U, reader_releases = 0U;
                    for (const auto& [id, allocation] : surface_audit.sources) {
                        if (!allocation.native.retired()) {
                            ++workspaces;
                            workspace_bytes += allocation.bytes;
                        }
                    }
                    for (const auto& [id, allocation] : surface_audit.surfaces) {
                        if (allocation.created != 0U && allocation.retired == 0U && allocation.source_textures.empty()) ++arenas;
                        for (const auto& [_, receipt] : allocation.receipts) {
                            if (receipt.stage != 3U) continue;
                            if (receipt.release_only)
                                ++release_only_reads;
                            else if (receipt.direct_sampling)
                                ++direct_reads;
                            else
                                ++copies;
                        }
                        offers += std::ranges::count_if(allocation.transfers, [](const auto& transfer) { return !transfer.second.releasing; });
                        for (const auto& [publication, custody] : allocation.custody) {
                            encodings += custody.encoded;
                            settlements += custody.settled;
                            abandonments += custody.abandoned;
                            reader_releases += custody.released;
                        }
                    }
                    append_acceptance_record(acceptance_log, {{"event", "acceptance.physical_inventory"},
                                                              {"live_workspaces", workspaces},
                                                              {"workspace_bytes", workspace_bytes},
                                                              {"live_sample_arenas", arenas},
                                                              {"live_sample_slots", arenas * 2U},
                                                              {"completed_browser_copies", copies},
                                                              {"settled_direct_reads", direct_reads},
                                                              {"settled_release_only_reads", release_only_reads},
                                                              {"unacquired_offers", offers},
                                                              {"encoded_draws", encodings},
                                                              {"settled_draws", settlements},
                                                              {"abandoned_draws", abandonments},
                                                              {"final_reader_releases", reader_releases},
                                                              {"explore_peak_pinned_bytes", native.explore_max_pinned}});
                }
            }
            CHECK((browser.atlas_notices == std::set<std::string>{"explore.gallery.capacity", "explore.gallery.empty"}));
            CHECK(browser.atlas_draws.empty_draw_submitted(surface_audit));
            CHECK((browser.atlas_window_draws == std::set<std::string>{"fullscreen", "restored"}));
            CHECK(browser.atlas_themes.contains(dark ? "dark" : "light"));
            CHECK(browser.atlas_device_scales.contains(high_dpi ? 1.5 : 1.0));
            CHECK((browser.atlas_visibility_modes == std::set<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7}));
            CHECK((browser.atlas_scroll_stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "row9", "end", "restored"}));
        }
        CHECK(browser.detail_fit);
        CHECK(browser.surface_draws.contains(browser.viewer_presentation));
        CHECK(native.explore_rendered);
        CHECK(native.presentation_ready);
        if (last) CHECK(native.shutdown_complete);
        CHECK_FALSE(native.worker_failed);
        if (viewer_scenario == "semantics") {
            CHECK((browser.viewer_overlay_modes == std::set<std::uint64_t>{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}));
            CHECK_FALSE(browser.viewer_label_colors.empty());
            CHECK(browser.viewer_class_colors);
            CHECK(browser.viewer_labels_without_boxes);
            for (const auto kind : {1U, 5U}) {
                CHECK(std::ranges::any_of(browser.viewer_label_products,
                                          [&](const auto& product) { return product.first == kind && browser.surface_draws.contains(product.second); }));
            }
        }
        if (viewer_scenario == "copy") {
            CHECK(browser.upscale_completed_pixels.size() == 3U);
            CHECK(browser.upscale_same_method.size() == 3U);
            CHECK((browser.annotation_shapes == std::set<std::string>{"Point", "Spline", "Skeleton"}));
            CHECK(browser.annotation_pixels_valid);
            CHECK(browser.annotation_layout_valid);
            CHECK((browser.annotation_layouts == std::set<std::string>{"narrow", "wide"}));
            for (const auto layout : {"wide", "narrow"}) {
                const auto found = browser.annotation_reachable.find(layout);
                REQUIRE(found != browser.annotation_reachable.end());
                const auto& controls = found->second;
                for (const auto control : {"annotation.save", "annotation.timeline", "annotation.stop"}) CHECK(controls.contains(control));
                // The source imports one object and six classes. Setup creates
                // three shapes and 32 disabled duplicates/classes. Each full
                // product pass adds four further objects.
                const std::uint64_t objects = std::string_view{layout} == "wide" ? 36U : 40U;
                const std::uint64_t classes = 38U;
                const auto tails = browser.annotation_tails.find(layout);
                REQUIRE(tails != browser.annotation_tails.end());
                const std::map<std::string, std::uint64_t> expected{
                    {"annotation.object." + std::to_string(objects - 1U), objects},
                    {"annotation.class." + std::to_string(classes - 1U), classes},
                };
                CHECK(tails->second == expected);
                for (const auto& [control, count] : expected) {
                    static_cast<void>(count);
                    CHECK(controls.contains(control));
                }
            }
            CHECK((browser.annotation_capabilities == std::set<std::string>{"enabled", "disabled"}));
            CHECK(browser.annotation_swatches == 2U);
            CHECK(browser.annotation_previews == 2U);
            CHECK_FALSE(browser.annotation_pixel_frames.empty());
            for (const auto& [source, presentation] : browser.annotation_pixel_frames) {
                CHECK(source != 0U);
                CHECK(browser.surface_draws.contains(presentation));
            }
            for (const auto operation : {"color-sample", "mask-fill", "move-Mask", "resize-mask", "paint-mask", "erase-mask", "move-Point", "move-Spline",
                                         "move-Skeleton", "spline-handle", "singleton-spline", "reclassify", "undo-class", "redo-class", "cancel-preview",
                                         "create-Box", "create-Point", "create-Skeleton"})
                CHECK(browser.annotation_product_operations[operation] >= 2U);
            for (const auto tool : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationTool>())
                CHECK(browser.annotation_product_operations["tool-" + std::string{tool.name}] >= 2U);
            for (const auto operation : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationMaskCleanup>())
                CHECK(browser.annotation_product_operations["cleanup-" + std::string{operation.name}] >= 2U);
            CHECK((browser.viewer_import_edits ==
                   std::set<std::string>{"box-move-undo-redo", "box-resize-undo-redo", "mask-paint-undo-redo", "mask-erase-undo-redo", "class-undo-redo"}));
            const auto saved_path = compiled_directory / "viewer-annotations.cbor";
            REQUIRE(std::filesystem::is_regular_file(saved_path));
            const auto saved_bytes = read_from(saved_path, 0U);
            const auto saved = mmltk::frameworks::serialization::decode<mmltk::controller::contracts::AnnotationUiState>(
                {.first = std::as_bytes(std::span{saved_bytes})}, {.max_bytes = mmltk::controller::contracts::kAnnotationUiStateByteBudget,
                                                                   .max_items = mmltk::controller::contracts::kAnnotationUiStateByteBudget});
            REQUIRE(saved.has_value());
            CHECK(saved->valid());
            CHECK(saved->scene.objects.size() == 44U);
            CHECK(saved->scene.categories.size() == 38U);
            CHECK(std::ranges::count_if(saved->scene.objects, [](const auto& object) { return !object.enabled; }) == 32);
            CHECK(saved->scene.palette == mmltk::controller::annotation_class_palette(saved->scene.categories.size()));
            for (const auto shape : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationShape>())
                CHECK(std::ranges::any_of(saved->scene.objects, [&](const auto& object) { return object.enabled && object.shape == shape.value; }));
            CHECK(std::ranges::any_of(saved->scene.objects, [](const auto& object) {
                return object.shape == mmltk::controller::contracts::AnnotationShape::Spline && object.spline_knots.size() == 1U;
            }));
            for (const auto& object : saved->scene.objects)
                for (const auto& run : object.mask.runs) {
                    CHECK(run.first >= object.box.first.x);
                    CHECK(static_cast<float>(run.last) + 1.0F <= object.box.second.x);
                    CHECK(run.row >= object.box.first.y);
                    CHECK(static_cast<float>(run.row) + 1.0F <= object.box.second.y);
                }
        }
        if (!last) AdvanceScenario();
        return;
    }
    CHECK_FALSE(exited_early);
    CHECK(native.product_completed(final_generations, seeded_augmentation_ready(), pixel_fixture));
    CHECK(browser.product_ready());
    REQUIRE(final_generations.material != 0U);
    REQUIRE(final_generations.cursor != 0U);
    REQUIRE(released_placeholder_generation != 0U);
    const auto released_placeholder = native.placeholder_slots.find(released_placeholder_generation);
    REQUIRE(released_placeholder != native.placeholder_slots.end());
    CHECK(browser.rendered_frame_for_slots(released_placeholder->second, 0U, 0U));
    const auto* final_cursor_slots = browser.final_cursor_slots();
    REQUIRE(final_cursor_slots != nullptr);
    const auto identified_final_generations =
        native.final_generations_for(*final_cursor_slots, browser.final_cursor_generation, browser.final_cursor_frame_revision);
    REQUIRE(identified_final_generations.has_value());
    CHECK(identified_final_generations->material == final_generations.material);
    CHECK(identified_final_generations->cursor == final_generations.cursor);
    const auto* pointer_slots = browser.pointer_slots();
    REQUIRE(pointer_slots != nullptr);
    const auto pointer_generation = native.generation_for(*pointer_slots);
    REQUIRE(pointer_generation.has_value());
    CHECK(browser.rendered_frame_for_slots(*pointer_slots, browser.pointer_frame_revision, 0U));
    if (pixel_fixture) {
        CHECK(rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical));
        CHECK(rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal));
    }
    CHECK(browser.compiled_images == static_cast<std::uint64_t>(fixture.num_images));
    CHECK(browser.compiled_width == static_cast<std::uint64_t>(kCompiledResolution));
    CHECK(browser.compiled_height == static_cast<std::uint64_t>(kCompiledResolution));
    CHECK(browser.presentation_receipt != 0U);
    CHECK(native.presentation_timeline != 0U);
    if (last) {
        CHECK(native.shutdown_requested);
        CHECK(native.firefox_terminal);
        CHECK(native.shutdown_complete);
    }
    CHECK_FALSE(native.shutdown_incomplete);
    CHECK_FALSE(native.worker_failed);
    CHECK_FALSE(native.invalid_message);
    CHECK_FALSE(native.peer_replaced);
    CHECK(native.peer_open_count >= 2U);
    CHECK_FALSE(native.interaction_rejected);
    CHECK(native.partial_generation != 0U);
    CHECK(native.partial_placeholder_ordinal != 0U);
    CHECK(native.partial_first_patch_ordinal > native.partial_placeholder_ordinal);
    CHECK(native.explore_ready_batch);
    REQUIRE(native.placeholder_slots.contains(native.partial_generation));
    REQUIRE(native.patched_slots.contains(native.partial_generation));
    CHECK_FALSE(native.patched_slots.at(native.partial_generation).empty());
    CHECK(std::ranges::any_of(browser.explore_slots,
                              [this](const auto& rendered) { return rendered.second == native.placeholder_slots.at(native.partial_generation); }));
    CHECK(native.exact_partial_slot_identity());
    CHECK(native.stale_thumbnail_discarded());
    CHECK_FALSE(native.explore_tile_regressed);
    CHECK_FALSE(native.explore_stale_patch);
    CHECK(native.explore_nproc == std::min(permitted_cpus, std::size_t{64U}));
    CHECK_FALSE(native.explore_nproc_changed);
    CHECK(native.explore_max_pinned > 0U);
    constexpr std::size_t compiled_pixels = kCompiledResolution * kCompiledResolution;
    constexpr std::size_t source_and_donor_bytes = 2U * compiled_pixels * 3U * sizeof(float);
    constexpr std::size_t donor_mask_bytes = ((compiled_pixels + 63U) / 64U) * sizeof(std::uint64_t);
    constexpr std::size_t fixture_descriptor_allowance = 64U * 1024U;
    constexpr std::size_t maximum_fixture_lane_bytes = source_and_donor_bytes + donor_mask_bytes + fixture_descriptor_allowance;
    CHECK(native.explore_max_pinned <= native.explore_nproc * maximum_fixture_lane_bytes);
    const auto compiled_path = compiled_directory / (fixture.split + ".bin");
    REQUIRE(std::filesystem::is_regular_file(compiled_path));
    // CLEANUP-IGNORE: Runtime audit facts and persisted dataset metadata are independent Wayland acceptance evidence.
    const auto compiled = mmltk::backend::data::inspect_compiled_dataset(compiled_path);
    CHECK(compiled.image_count == static_cast<std::uint32_t>(fixture.num_images));
    CHECK(compiled.width == static_cast<std::uint32_t>(kCompiledResolution));
    CHECK(compiled.height == static_cast<std::uint32_t>(kCompiledResolution));
    CHECK(compiled.channels != 0U);
    CHECK_FALSE(compiled.class_names().empty());
    if (!last) AdvanceScenario();
}
void WaylandSession::AdvanceScenario() {
    REQUIRE(frontend_settled_);
    REQUIRE_FALSE(browser_failed_);
    REQUIRE_FALSE(browser.failed_before_termination());
    REQUIRE_FALSE(native.failed_before_termination());
    REQUIRE(pixel_audit.failure.empty());
    REQUIRE(surface_audit.evidence_settled());
    surface_audit.SettleScenario();
    REQUIRE(surface_audit.failure.empty());
    REQUIRE(surface_audit.surfaces.size() <= kAcceptanceGenerationLimit);
    const auto live_publication = [&](const auto& identity, const auto revision) {
        const auto surface = surface_audit.surfaces.find(identity);
        return surface != surface_audit.surfaces.end() && surface->second.publications.contains(revision);
    };
    PixelBoundaryAudit next_pixels{pixel_probes};
    next_pixels.samples = std::move(pixel_audit.samples);
    std::erase_if(next_pixels.samples, [&](const auto& entry) {
        const auto surface = surface_audit.surfaces.find(entry.first.first);
        if (surface == surface_audit.surfaces.end()) return true;
        const bool complete = entry.second.complete();
        return complete && !live_publication(entry.first.first, entry.first.second);
    });
    for (auto& [_, publication] : next_pixels.samples) {
        publication.receivers[3] = {};
        publication.viewer = false;
        publication.counted.fill(false);
        publication.direct_counted = false;
    }
    pixel_audit = std::move(next_pixels);
    AtlasDrawAudit retained_atlas;
    retained_atlas.acquisitions = std::move(browser.atlas_draws.acquisitions);
    std::erase_if(retained_atlas.acquisitions, [&](const auto& entry) { return !surface_audit.surfaces.contains(std::get<0>(entry.first)); });
    std::set<AtlasDrawAudit::SourceKey> retained_sources;
    for (const auto& [_, capture] : retained_atlas.acquisitions) retained_sources.insert(AtlasDrawAudit::source_key(capture));
    std::map<std::array<std::uint64_t, 3U>, AtlasDrawAudit::SourceKey> latest_sources;
    for (const auto& [key, _] : browser.atlas_draws.sources) {
        const std::array owner{key[0], key[1], key[2]};
        auto [found, inserted] = latest_sources.try_emplace(owner, key);
        if (!inserted && key[3] > found->second[3]) found->second = key;
    }
    for (const auto& [_, key] : latest_sources) retained_sources.insert(key);
    retained_atlas.sources = std::move(browser.atlas_draws.sources);
    retained_atlas.sessions = std::move(browser.atlas_draws.sessions);
    std::erase_if(retained_atlas.sources, [&](const auto& entry) { return !retained_sources.contains(entry.first); });
    std::erase_if(retained_atlas.sessions, [&](const auto& entry) { return !retained_sources.contains(entry.second); });
    const bool current_atlas = browser.owned_atlas_current;
    auto prior_atlas_draw = browser.atlas_draws.overlap_draw ? std::move(browser.atlas_draws.overlap_draw) : std::move(browser.prior_scenario_atlas_draw);
    browser = {};
    browser.atlas_draws = std::move(retained_atlas);
    browser.owned_atlas_current = current_atlas;
    browser.prior_scenario_atlas_draw = std::move(prior_atlas_draw);
    NativeAudit next_native;
    next_native.server_started = native.server_started;
    next_native.peer_opened = native.peer_opened;
    next_native.firefox_pid = native.firefox_pid;
    next_native.peer_open_count = native.peer_open_count;
    next_native.peer_close_count = native.peer_close_count;
    next_native.presentation_ready = native.presentation_ready;
    next_native.presentation_timeline = native.presentation_timeline;
    next_native.explore_nproc = native.explore_nproc;
    native = std::move(next_native);
    ++scenario_sequence_;
    frontend_settled_ = false;
    REQUIRE(process_->command_explore(8U));
}
[[nodiscard]] std::shared_ptr<PreparedWaylandInputs> wayland_inputs(const bool require_compiled) {
    if (!execution_requested()) SKIP("workspace Wayland integration requires the packaged hardware runner");
    if (permitted_cpu_count() < 2U) SKIP("workspace Wayland streaming acceptance requires at least two permitted CPUs");
    // Dataset/artifact custody only: no GPU runtime or browser is global.
    // Catch enumeration never enters this function.
    static const auto inputs = std::make_shared<PreparedWaylandInputs>();
    const auto& fixture = inputs->mixed();
    if (require_compiled && !std::filesystem::is_regular_file(mmltk::backend::data::testsupport::compiled_bin_path(fixture))) {
        compile_wayland_fixture(fixture, kCompiledResolution);
    }
    return inputs;
}

WaylandSession::~WaylandSession() = default;

} // namespace mmltk::acceptance::wayland
