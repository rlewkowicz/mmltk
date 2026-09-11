#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/generators/catch_generators.hpp>
#include <cuda.h>
#include "catch2_compat.hpp"
#include "filesystem_test_utils.hpp"
#include "linux_process_test_utils.hpp"
#include "src/backend/data/compiled_file_utils.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/imaging/raster/detail/raster_color.h"
#include "src/common/io/scoped_fd.h"
#include "src/controller/browser/application_stable_identity.h"
#include "src/controller/contracts/gui_settings.h"
#include "src/controller/contracts/annotation.h"
#include "src/controller/contracts/diagnostic_context.h"
#include "src/controller/contracts/visual_source.h"
#include "src/controller/presentation/workspace_presentation_types.h"
#include "src/frameworks/serialization/serialization.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/services/firefox_process_owner.h"
#include "test_fixture.h"

namespace {

// CLEANUP-IGNORE: The Wayland product driver imports its own typed test helpers and hardware-only termination
// vocabulary.
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
constexpr std::uint64_t kUpdateViewportEndpoint = mmltk::controller::browser::application_stable_id("explore", "UpdateViewport");
constexpr std::size_t kAcceptanceGenerationLimit = 128U;
constexpr std::size_t kAcceptanceSlotLimit = mmltk::controller::kExploreVisibleItemCapacity;
constexpr std::size_t kAcceptanceRecordLimit = 4096U;
constexpr const char* kExploreGalleryControl = "explore.gallery.workspace";

[[nodiscard]] constexpr std::string_view first_failed_check() noexcept { return {}; }

template <typename... Remaining>
[[nodiscard]] constexpr std::string_view first_failed_check(const bool passed, const std::string_view label,
                                                            Remaining&&... remaining) noexcept {
    return passed ? first_failed_check(std::forward<Remaining>(remaining)...) : label;
}

enum class TerminationMode : std::uint8_t {
    SignalInterrupt,
    WindowClose,
    AbruptPeerLoss,
};

[[nodiscard]] constexpr std::string_view termination_label(const TerminationMode mode) noexcept {
    switch (mode) {
        case TerminationMode::SignalInterrupt:
            return "sigint";
        case TerminationMode::WindowClose:
            return "window-close";
        case TerminationMode::AbruptPeerLoss:
            return "peer-loss";
    }
    std::terminate();
}

[[nodiscard]] bool execution_requested() noexcept {
    const char* const requested = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION");
    return requested != nullptr && std::string_view{requested} == "1";
}

[[nodiscard]] bool gdr_transport_available() noexcept {
    if (::access("/dev/gdrdrv", R_OK | W_OK) == 0) return true;
    if (cuInit(0U) != CUDA_SUCCESS) return false;
    CUdevice device{};
    int dmabuf = 0;
    return cuDeviceGet(&device, 0) == CUDA_SUCCESS &&
           cuDeviceGetAttribute(&dmabuf, static_cast<CUdevice_attribute>(152), device) == CUDA_SUCCESS && dmabuf != 0;
}

[[nodiscard]] std::filesystem::path configured_path(const char* const name, const std::filesystem::path& fallback) {
    const char* const configured = std::getenv(name);
    return configured != nullptr && configured[0] != '\0' ? std::filesystem::absolute(configured) : std::filesystem::absolute(fallback);
}

[[nodiscard]] std::filesystem::path latest_wayland_artifact(const std::string_view filename) {
    const char* const configured_root = std::getenv("MMLTK_REPO_ROOT");
    const std::filesystem::path repository =
        configured_root != nullptr && configured_root[0] != '\0' ? configured_root : std::filesystem::current_path();
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

void append_acceptance_record(const std::filesystem::path& path, nlohmann::json record) {
    record["kind"] = "acceptance_runtime";
    record["owner"] = "acceptance";
    record["steady_ns"] = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string payload = record.dump() + '\n';
    ScopedFd output{::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC)};
    if (output.get() < 0) throw std::runtime_error("failed to append workspace Wayland acceptance evidence");
    std::size_t offset = 0U;
    while (offset != payload.size()) {
        ssize_t written = -1;
        do {
            written = ::write(output.get(), payload.data() + offset, payload.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) throw std::runtime_error("failed to write workspace Wayland acceptance evidence");
        offset += static_cast<std::size_t>(written);
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
    BrowserHostProcess(const std::filesystem::path& executable, const std::filesystem::path& diagnostics,
                       const std::filesystem::path& runtime_log, const std::filesystem::path& firefox_log,
                       const std::filesystem::path& working_directory, const TerminationMode termination,
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
                runtime_output.get() >= 0 && ::dup2(runtime_output.get(), STDOUT_FILENO) >= 0 &&
                ::dup2(runtime_output.get(), STDERR_FILENO) >= 0 && ::fcntl(control_read.get(), F_SETFD, 0) == 0 &&
                ::chdir(working_directory.c_str()) == 0 &&
                (logging
                     ? (::setenv("MMLTK_LOG_LEVEL", "trace", 1) == 0 && ::setenv("MMLTK_LOG_FILE", application_log_text.c_str(), 1) == 0 &&
                        ::setenv("MMLTK_GUI_TRACE_FILE", diagnostics_text.c_str(), 1) == 0 &&
                        ::setenv("MMLTK_FIREFOX_LOG_FILE", firefox_text.c_str(), 1) == 0 &&
                        ::setenv("MOZ_LOG",
                                 "WebGPU:5,Widget:5,WidgetVSync:5,WidgetWayland:5,Dmabuf:5,WidgetCompositor:5,"
                                 "nsRefreshDriver:5,PresShell:5,rotate:16",
                                 1) == 0 &&
                        ::setenv("MOZ_LOG_FILE", mozilla_text.c_str(), 1) == 0 && ::setenv("RUST_BACKTRACE", "full", 1) == 0)
                     : (::unsetenv("MMLTK_LOG_LEVEL") == 0 && ::unsetenv("MMLTK_LOG_FILE") == 0 && ::unsetenv("MMLTK_LOG_DIR") == 0 &&
                        ::unsetenv("MMLTK_GUI_TRACE_FILE") == 0 && ::unsetenv("MMLTK_FIREFOX_LOG_FILE") == 0 &&
                        ::unsetenv("MOZ_LOG") == 0 && ::unsetenv("MOZ_LOG_FILE") == 0 && ::unsetenv("RUST_BACKTRACE") == 0)) &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION", "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DPI", high_dpi ? "1.5" : "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_VIEWER_SCENARIO", viewer_scenario.c_str(), 1) == 0 &&
                ::setenv("MMLTK_GUI_PIXEL_TRACE", logging && pixel_probes ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PROBE_FAILURE", probe_failure.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PIXEL_FIXTURE",
                         fixture.pixel_evidence && (viewer_scenario == "retained" || viewer_scenario == "dpi") ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPLETION_GATE", viewer_scenario == "retained" ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PENDING_SUPERSESSION",
                         logging && (viewer_scenario == "retained" || viewer_scenario == "dpi") ? "1" : "0", 1) == 0 &&
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
        do {
            written = ::send(control_write_.get(), &command, sizeof(command), MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written == sizeof(command)) return true;
        control_write_.reset();
        return false;
    }

    [[nodiscard]] std::optional<mmltk::controller::ExploreAcceptanceGate::ControlObservation> receive_explore_event() noexcept {
        if (control_write_.get() < 0) return std::nullopt;
        mmltk::controller::ExploreAcceptanceGate::ControlObservation event;
        ssize_t consumed = -1;
        do {
            consumed = ::recv(control_write_.get(), &event, sizeof(event), 0);
        } while (consumed < 0 && errno == EINTR);
        if (consumed == static_cast<ssize_t>(sizeof(event))) return event;
        if (consumed == 1 && event.event == mmltk::controller::ExploreAcceptanceGate::ControlEvent::InitialWait) return event;
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
        do {
            result = ::waitpid(child_, &terminal, WNOHANG);
        } while (result < 0 && errno == EINTR);
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
            do {
                ready = ::poll(&peer_exit, 1U, 1000);
            } while (ready < 0 && errno == EINTR);
        }
        peer_pidfd_.reset();
    }

    void signal_process(const int signal) noexcept {
        if (!active()) return;
        if (::syscall(SYS_pidfd_send_signal, pidfd_.get(), signal, nullptr, 0U) != 0 && errno != ESRCH)
            static_cast<void>(::kill(child_, signal));
    }

    pid_t child_ = -1;
    ScopedFd pidfd_;
    pid_t peer_group_ = -1;
    ScopedFd peer_pidfd_;
    ScopedFd control_write_;
    bool explore_control_closed_ = false;
    int status_ = -1;
};

[[nodiscard]] std::uint64_t scalar(const nlohmann::json& value, const char* const field) noexcept {
    const auto found = value.find(field);
    if (found == value.end()) return 0U;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (!found->is_string()) return 0U;
    const std::string& text = found->get_ref<const std::string&>();
    std::uint64_t result = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0U;
}

[[nodiscard]] std::string_view textual(const nlohmann::json& value, const char* const field) noexcept {
    const auto found = value.find(field);
    return found != value.end() && found->is_string() ? std::string_view{found->get_ref<const std::string&>()} : std::string_view{};
}

[[nodiscard]] double numeric(const nlohmann::json& value, const char* const field) noexcept {
    const auto found = value.find(field);
    if (found == value.end()) return 0.0;
    if (found->is_number()) return found->get<double>();
    if (!found->is_string()) return 0.0;
    const std::string& text = found->get_ref<const std::string&>();
    double result = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0.0;
}

// Native and browser files are drained independently. Validate ordering within
// each producer stream, then join their facts by the physical capability; file
// read order is not a cross-process happens-before relation.
struct SurfaceAudit final {
    struct Reconstruction final {
        std::string completed;
        std::string requested;
        std::size_t ordinal = 0U;
    };
    struct SourceRead final {
        std::string source;
        std::uint64_t allocation = 0U;
        std::uint64_t session = 0U;
        std::uint64_t frame = 0U;
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        bool operator==(const SourceRead&) const = default;
    };
    struct TransferReceipt final {
        SourceRead read;
        bool releasing = false;
        bool released = false;
    };
    struct ReceiverReceipt final {
        std::string source;
        std::uint64_t transfer = 0U;
        std::uint64_t layer = 0U;
        std::uint64_t slot = 0U;
        std::uint64_t session = 0U;
        std::uint64_t frame = 0U;
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        unsigned stage = 0U;
    };
    struct SampleCustody final {
        std::uint64_t selected = 0U;
        std::uint64_t encoded = 0U;
        std::uint64_t settled = 0U;
        std::uint64_t abandoned = 0U;
        bool acquired = false;
        bool released = false;
    };
    struct SurfaceState final {
        std::uint64_t generation = 0U;
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        std::uint64_t iced_generation = 0U;
        std::uint64_t browser_width = 0U;
        std::uint64_t browser_height = 0U;
        unsigned native_stage = 0U;
        // CLEANUP-IGNORE: Firefox import lifecycle state is distinct from the browser-controller outcome flags.
        unsigned firefox_stage = 0U;
        bool import_failed = false;
        bool firefox_import_failed = false;
        bool withdrawn = false;
        bool candidate_withdrawn = false;
        bool native_retired = false;
        bool firefox_withdrawn = false;
        bool firefox_retired = false;
        std::size_t created = 0U;
        std::size_t acquired = 0U;
        std::size_t discarded = 0U;
        std::size_t retired = 0U;
        std::optional<Reconstruction> reconstruction;
        std::map<std::uint64_t, SourceRead> reads;
        std::map<std::pair<std::uint64_t, std::uint64_t>, TransferReceipt> transfers;
        std::map<std::uint64_t, ReceiverReceipt> receipts;
        std::map<std::uint64_t, SampleCustody> custody;
        std::size_t import_dropped = 0U;
        std::map<std::pair<std::uint64_t, std::uint64_t>, SourceRead> releases;
        std::map<std::uint64_t, unsigned> source_steps;
        std::map<std::pair<std::uint64_t, std::uint64_t>, unsigned> publication_steps;
        std::set<std::uint64_t> failed_source_operations;
        std::set<std::pair<std::uint64_t, std::uint64_t>> failed_publications;
        std::map<std::uint64_t, std::uint64_t> ended_spans;
        std::map<std::uint64_t, std::uint64_t> publications;
        std::map<std::uint64_t, std::uint64_t> samples;
    };
    class SourceLifecycle final {
       public:
        [[nodiscard]] bool Observe(const unsigned step, const bool shutdown) {
            if (retired_) return false;
            if (step <= 3U) {
                if (cancelled_ || admission_ + 1U != step) return false;
                admission_ = step;
                return true;
            }
            if (admission_ == 0U) return false;
            if (step == 4U) {
                if (withdrawn_) return false;
                withdrawn_ = true;
                return true;
            }
            if (step != 5U || (!withdrawn_ && !shutdown)) return false;
            retired_ = true;
            return true;
        }
        [[nodiscard]] bool ready() const noexcept { return admission_ == 3U; }
        [[nodiscard]] bool live() const noexcept { return ready() && !withdrawn_ && !retired_; }
        [[nodiscard]] bool retired() const noexcept { return retired_; }
        [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
        [[nodiscard]] bool Cancel() noexcept {
            if (admission_ == 0U || ready() || !withdrawn_ || retired_ || cancelled_) return false;
            cancelled_ = true;
            return true;
        }

       private:
        unsigned admission_ = 0U;
        bool withdrawn_ = false;
        bool retired_ = false;
        bool cancelled_ = false;
    };
    struct SourceState final {
        SourceLifecycle native;
        SourceLifecycle browser;
        // CLEANUP-IGNORE: This independent source lifecycle oracle compares native and browser evidence; it cannot reuse their production
        // schema.
        bool failed = false;
        // CLEANUP-IGNORE: Independent observed source dimensions and allocation identity are acceptance evidence, not shared runtime
        // storage.
        std::uint64_t generation = 0U;
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        std::uint64_t allocation = 0U;
        std::uint64_t bytes = 0U;
        std::uint64_t pitch = 0U;
        std::uint64_t browser_width = 0U;
        std::uint64_t browser_height = 0U;
        std::uint64_t browser_allocation = 0U;
        std::string arena;
    };
    std::map<std::string, SourceState, std::less<>> sources;
    void source_transition(const std::string& id, const std::string_view event, const bool browser, const std::uint64_t code = 0U) {
        if (!valid_identity(id) || (!sources.contains(id) && sources.size() == kAcceptanceGenerationLimit)) {
            reject("source admission evidence has invalid identity or exceeds capacity");
            return;
        }
        auto& source = sources[id];
        auto& lifecycle = browser ? source.browser : source.native;
        unsigned next = 0U;
        if (event.ends_with("admitted") || event.ends_with("admission.enqueued"))
            next = 1U;
        else if (event.ends_with("claim_outcome") || event.ends_with("admission.written"))
            next = 2U;
        else if (event.ends_with("ready"))
            next = 3U;
        else if (event.ends_with("withdrawal"))
            next = 4U;
        else if (event.ends_with("retired") || event.ends_with("retirement"))
            next = 5U;
        else if (event.ends_with("import_failed")) {
            if (code == 1U && lifecycle.Cancel()) return;
            source.failed = true;
            reject("source import failed without a valid withdrawal");
            return;
        } else
            return;
        // Withdrawal closes admission while an already claimed import may
        // still finish. Physical retirement is the terminal boundary.
        if (!lifecycle.Observe(next, native_shutdown && !browser))
            reject("source admission or retirement is missing, duplicate, or reordered");
    }
    struct Draw final {
        std::string selected;
        std::string requested;
        std::size_t ordinal = 0U;
    };
    std::map<std::string, SurfaceState, std::less<>> surfaces;
    std::vector<Draw> draws;
    std::string failure;
    std::size_t browser_ordinal = 0U;
    bool native_shutdown = false;
    bool browser_shutdown = false;
    bool browser_exited = false;

    void reject(const std::string_view why) {
        if (failure.empty()) failure = why;
    }

    [[nodiscard]] bool admit(const std::string& id) {
        if (!failure.empty()) return false;
        const auto existing = surfaces.find(id);
        if (existing == surfaces.end()) {
            if (surfaces.size() < kAcceptanceGenerationLimit) return true;
        } else {
            const auto& state = existing->second;
            if (state.source_steps.size() < kAcceptanceRecordLimit && state.publication_steps.size() < kAcceptanceRecordLimit &&
                state.reads.size() < kAcceptanceRecordLimit && state.transfers.size() < kAcceptanceRecordLimit &&
                state.receipts.size() < kAcceptanceRecordLimit && state.releases.size() < kAcceptanceRecordLimit &&
                state.ended_spans.size() < kAcceptanceRecordLimit && state.samples.size() < kAcceptanceRecordLimit &&
                state.failed_source_operations.size() < kAcceptanceRecordLimit &&
                state.failed_publications.size() < kAcceptanceRecordLimit && draws.size() < kAcceptanceRecordLimit)
                return true;
        }
        reject("physical lifecycle evidence exceeded its bounded acceptance capacity");
        return false;
    }

    static std::string native_identity(const nlohmann::json& record, const bool workspace = false) {
        std::string result(32U, '0');
        for (const auto& [offset, field] : {std::pair{0U, "surface_high"}, std::pair{16U, "surface_low"}}) {
            std::array<char, 16U> digits{};
            const auto converted =
                std::to_chars(digits.data(), digits.data() + digits.size(),
                              scalar(record, workspace ? (offset == 0U ? "workspace_source_high" : "workspace_source_low") : field), 16);
            const auto count = static_cast<std::size_t>(converted.ptr - digits.data());
            result.replace(offset + 16U - count, count, digits.data(), count);
        }
        return result;
    }

    static bool valid_identity(const std::string_view id) {
        return id.size() == 32U && id != std::string(32U, '0') &&
               std::ranges::all_of(id, [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
    }

    void native(const nlohmann::json& record) {
        const std::string event = record.value("event", "");
        if (event.starts_with("presentation.copy.")) reject("Presentation performed an obsolete pixel copy");
        native_shutdown = native_shutdown || event == "shutdown.requested";
        browser_shutdown = browser_shutdown || event == "shutdown.firefox_terminal";
        browser_exited = browser_exited || event == "shutdown.firefox_terminal";
        if (event.starts_with("presentation.source.admission.") || event == "presentation.source.ready" ||
            event == "presentation.source.withdrawal" || event == "presentation.source.retirement") {
            if (scalar(record, "outcome") != 1U) reject("native source admission or retirement failed");
            const auto id = native_identity(record);
            source_transition(id, event, false);
            if (!failure.empty()) return;
            auto& source = sources.at(id);
            const auto generation = scalar(record, "sequence");
            const auto width = scalar(record, "capacity_width");
            const auto height = scalar(record, "capacity_height");
            const auto allocation = scalar(record, "workspace_allocation");
            if (generation == 0U || width == 0U || height == 0U || allocation == 0U || native_identity(record, true) != id ||
                (source.generation != 0U &&
                 (source.generation != generation || source.width != width || source.height != height || source.allocation != allocation)))
                reject("native source admission has missing or inconsistent physical provenance");
            source.generation = generation;
            source.width = width;
            source.height = height;
            const auto bytes = scalar(record, "workspace_bytes"), pitch = scalar(record, "workspace_pitch");
            if (width > pitch / 4U || height == 0U || bytes / height < pitch || scalar(record, "workspace_width") != width ||
                scalar(record, "workspace_height") != height || (source.bytes != 0U && (source.bytes != bytes || source.pitch != pitch)))
                reject("native physical allocation inventory is missing or inconsistent");
            source.bytes = bytes;
            source.pitch = pitch;
            source.allocation = allocation;
            return;
        }
        constexpr std::array transitions{"presentation.arena.advertised",
                                         "presentation.admission.enqueued",
                                         "presentation.admission.written",
                                         "presentation.import.outcome",
                                         "presentation.source_borrow.started",
                                         "presentation.source_borrow.completed",
                                         "presentation.source.read_submitted",
                                         "presentation.ready_sync.started",
                                         "presentation.ready_sync.completed",
                                         "presentation.frame.edge",
                                         "presentation.active.withdrawal",
                                         "presentation.candidate.withdrawal",
                                         "presentation.retirement",
                                         "presentation.release_wait.started",
                                         "presentation.release_wait.completed",
                                         "presentation.replacement"};
        if (record.value("kind", "") != "gui_runtime" || std::ranges::find(transitions, event) == transitions.end()) return;
        const auto id = native_identity(record);
        if (!record.contains("surface_high") || !record.contains("surface_low") || !valid_identity(id)) {
            reject("native lifecycle record has no physical identity");
            return;
        }
        if (!admit(id)) return;
        auto& state = surfaces[id];
        const auto generation = scalar(record, "sequence");
        const auto width = scalar(record, "capacity_width");
        const auto height = scalar(record, "capacity_height");
        if (generation == 0U || width == 0U || height == 0U || scalar(record, "selection_generation") == 0U ||
            scalar(record, "frame_revision") == 0U || !record.contains("condition") || !record.contains("outcome") ||
            (state.generation != 0U && (state.generation != generation || state.width != width || state.height != height)))
            reject("native surface provenance is missing or inconsistent");
        state.generation = generation;
        state.width = width;
        state.height = height;
        const bool copying = event == "presentation.source_borrow.started" || event == "presentation.source_borrow.completed" ||
                             event == "presentation.source.read_submitted";
        const bool transferred = event == "presentation.ready_sync.started" || event == "presentation.ready_sync.completed" ||
                                 event == "presentation.frame.edge" || event == "presentation.release_wait.started" ||
                                 event == "presentation.release_wait.completed";
        if (copying || transferred) {
            if (scalar(record, "source_revision") != scalar(record, "frame_revision") ||
                scalar(record, "allocation_generation") != generation || scalar(record, "presentation_revision") != scalar(record, "value"))
                reject("native operation mixes source allocation or publication identities");
            if (copying && (scalar(record, "presentation_revision") != 0U || scalar(record, "transfer_sequence") != 0U ||
                            scalar(record, "timeline_ready") != 0U))
                reject("new native source read inherited an incumbent physical publication");
            if (transferred && (scalar(record, "transfer_sequence") == 0U ||
                                scalar(record, "timeline_ready") != scalar(record, "transfer_sequence") * 2U - 1U))
                reject("native physical operation omitted its transfer identity");
        }
        if (state.native_retired) reject("native transition after physical retirement");
        const auto advance = [&](const unsigned next) {
            if (state.native_stage + 1U != next)
                reject("native admission stages are missing, duplicate, or reordered");
            else
                state.native_stage = next;
        };
        if (event == "presentation.arena.advertised")
            advance(1U);
        else if (event == "presentation.admission.enqueued")
            advance(2U);
        else if (event == "presentation.admission.written") {
            advance(3U);
        } else if (event == "presentation.import.outcome") {
            advance(4U);
            state.import_failed = scalar(record, "value") != 1U;
        } else if (event == "presentation.active.withdrawal" || event == "presentation.candidate.withdrawal") {
            if (state.native_stage != 4U || state.import_failed || state.withdrawn) reject("withdrawal lacks a unique completed import");
            state.withdrawn = true;
            state.candidate_withdrawn = event == "presentation.candidate.withdrawal";
        } else if (event == "presentation.retirement") {
            if (scalar(record, "outcome") != 1U) reject("native physical retirement failed");
            state.native_retired = true;
        } else {
            const bool span_end = event == "presentation.source_borrow.completed" || event == "presentation.ready_sync.completed" ||
                                  event == "presentation.release_wait.completed";
            if (span_end) {
                const auto span_outcome = scalar(record, "span_outcome");
                state.ended_spans.insert_or_assign(scalar(record, "span_id"), span_outcome);
                if (span_outcome != static_cast<std::uint64_t>(mmltk::controller::contracts::DiagnosticSpanOutcome::Success) ||
                    scalar(record, "outcome") != 1U) {
                    if (event == "presentation.source_borrow.completed")
                        state.failed_source_operations.insert(scalar(record, "trace_id"));
                    else if (event == "presentation.ready_sync.completed")
                        state.failed_publications.emplace(scalar(record, "value"), scalar(record, "transfer_sequence"));
                    return;
                }
            }
            if (event == "presentation.source.read_submitted" && scalar(record, "outcome") != 0U) return;
            const auto source_operation = scalar(record, "trace_id");
            const auto publication_key = std::pair{scalar(record, "value"), scalar(record, "transfer_sequence")};
            auto& steps = state.source_steps[source_operation];
            const auto observe = [&](unsigned& stage, const unsigned next) {
                if (state.native_stage != 4U || state.import_failed || stage + 1U != next)
                    reject("native source/read/ready stages are incomplete or reordered");
                else
                    stage = next;
            };
            if (event == "presentation.source_borrow.started") {
                if (state.failed_source_operations.erase(source_operation) != 0U) steps = 0U;
                observe(steps, 1U);
            } else if (event == "presentation.source_borrow.completed")
                observe(steps, 2U);
            else if (event == "presentation.source.read_submitted") {
                if (state.failed_source_operations.contains(source_operation))
                    reject("native source read followed a failed borrow");
                else {
                    observe(steps, 3U);
                    const auto source = native_identity(record, true);
                    const auto admitted = sources.find(source);
                    const SourceRead read{source,
                                          scalar(record, "workspace_allocation"),
                                          scalar(record, "source_session"),
                                          scalar(record, "source_revision"),
                                          scalar(record, "source_width"),
                                          scalar(record, "source_height")};
                    if (admitted == sources.end() || !admitted->second.native.live() || read.allocation == 0U ||
                        admitted->second.allocation != read.allocation || read.session == 0U || read.frame == 0U || read.width == 0U ||
                        read.height == 0U || read.width > admitted->second.width || read.height > admitted->second.height)
                        reject("source read does not name its admitted workspace allocation");
                    if (!state.reads.emplace(source_operation, read).second) reject("source read repeats an already submitted operation");
                }
            } else if (event == "presentation.ready_sync.started") {
                if (steps != 3U) reject("native ready synchronization lacks its submitted source read");
                auto& publication = state.publication_steps[publication_key];
                if (state.failed_publications.erase(publication_key) != 0U) publication = 0U;
                observe(publication, 1U);
                const auto read = state.reads.find(source_operation);
                if (read == state.reads.end() || !state.transfers.emplace(publication_key, TransferReceipt{read->second}).second)
                    reject("physical publication lacks a unique source read");
            } else if (event == "presentation.ready_sync.completed") {
                auto& publication = state.publication_steps[publication_key];
                observe(publication, 2U);
            } else if (event == "presentation.frame.edge") {
                auto& publication = state.publication_steps[publication_key];
                if (state.failed_publications.contains(publication_key)) {
                    reject("native frame publication followed a failed ready synchronization");
                } else {
                    observe(publication, 3U);
                    const auto publication_revision = scalar(record, "value");
                    const auto frame_revision = scalar(record, "frame_revision");
                    const auto [existing, inserted] = state.publications.emplace(publication_revision, frame_revision);
                    if (!inserted && existing->second != frame_revision)
                        reject("native frame publication changed within one publication revision");
                }
            }
            if (transferred) {
                const auto receipt = state.transfers.find(publication_key);
                const SourceRead observed{native_identity(record, true),    scalar(record, "workspace_allocation"),
                                          scalar(record, "source_session"), scalar(record, "source_revision"),
                                          scalar(record, "source_width"),   scalar(record, "source_height")};
                if (receipt == state.transfers.end() || receipt->second.read != observed)
                    reject("physical publication changed source read provenance");
                else if (event == "presentation.release_wait.started") {
                    if (receipt->second.releasing || state.publication_steps.at(publication_key) != 3U)
                        reject("source release wait lacks its unique published transfer");
                    receipt->second.releasing = true;
                } else if (event == "presentation.release_wait.completed") {
                    if (!receipt->second.releasing || receipt->second.released) reject("source release wait is missing or duplicated");
                    receipt->second.released = true;
                }
            }
        }
    }

    void browser(const nlohmann::json& record) {
        ++browser_ordinal;
        const std::string event = record.value("event", "");
        if (event == "browser.invalid_webgpu_texture") reject("Firefox reported an invalid WebGPU texture");
        browser_shutdown =
            browser_shutdown || (event == "firefox.workspace.channel_terminal" && record.value("terminal", "") == "orderly_bridge_close");
        if (event.starts_with("firefox.workspace.source.")) {
            if (event.ends_with("claim_outcome") && record.value("outcome", "") != "claimed")
                reject("Firefox failed to claim an admitted source");
            const std::string id = record.value("surface", "");
            source_transition(id, event, true, scalar(record, "code"));
            if (event == "firefox.workspace.source.admitted" && failure.empty()) {
                auto& source = sources.at(id);
                source.browser_width = scalar(record, "width");
                source.browser_height = scalar(record, "height");
                source.browser_allocation = scalar(record, "workspace_allocation");
                source.arena = record.value("arena", "");
                if (source.browser_width == 0U || source.browser_height == 0U || source.browser_allocation == 0U ||
                    !valid_identity(source.arena))
                    reject("Firefox source admission omitted its allocation or arena");
            }
            return;
        }
        if (event.find("capture") != std::string::npos && event.starts_with("iced.surface."))
            reject("Iced performed an obsolete capture pass");
        if (!event.starts_with("firefox.workspace.") && !event.starts_with("iced.surface.") && !event.starts_with("iced.frame.")) return;
        if (!record.contains("surface")) {
            if (event.starts_with("iced.surface.") || event == "firefox.workspace.admitted" || event == "firefox.workspace.claim_outcome" ||
                event == "firefox.workspace.registry_inserted" || event == "firefox.workspace.import_ready_emitted" ||
                event == "firefox.workspace.ready" || event == "firefox.workspace.withdrawal" || event == "firefox.workspace.retired")
                reject("browser lifecycle record has no physical identity");
            return;
        }
        const std::string id = record.value("surface", "");
        if (!valid_identity(id)) {
            reject("browser lifecycle record has malformed physical identity");
            return;
        }
        if (!admit(id)) return;
        auto& state = surfaces[id];
        if ((event == "firefox.workspace.admitted" || event == "firefox.workspace.claim_outcome") &&
            (!record.contains("width") || !record.contains("height")))
            reject("Firefox admission or claim has no dimensions");
        if (record.contains("width")) {
            const auto width = scalar(record, "width");
            const auto height = scalar(record, "height");
            if (width == 0U || height == 0U ||
                (state.browser_width != 0U && (state.browser_width != width || state.browser_height != height)))
                reject("browser dimensions are missing or inconsistent");
            state.browser_width = width;
            state.browser_height = height;
        }
        const auto advance = [&](const unsigned from) {
            if (state.firefox_stage != from) reject("Firefox admission stages are missing, duplicate, or reordered");
            state.firefox_stage = from + 1U;
        };
        if (event == "firefox.workspace.admitted")
            advance(0U);
        else if (event == "firefox.workspace.claim_outcome" && record.value("outcome", "") == "claimed")
            advance(1U);
        else if (event == "firefox.workspace.claim_outcome")
            reject("Firefox failed to claim the advertised surface");
        else if (event == "firefox.workspace.import_failed") {
            if (state.firefox_stage != 2U || state.firefox_import_failed || state.firefox_withdrawn || state.firefox_retired)
                reject("Firefox import failure lacks its unique claimed surface");
            state.firefox_import_failed = true;
        } else if (event == "firefox.workspace.registry_inserted")
            advance(2U);
        else if (event == "firefox.workspace.import_ready_emitted")
            advance(3U);
        else if (event == "firefox.workspace.ready")
            advance(4U);
        else if (event == "firefox.workspace.withdrawal")
            state.firefox_withdrawn = true;
        else if (event == "firefox.workspace.retired") {
            if (!state.firefox_withdrawn || state.firefox_retired) reject("Firefox retirement lacks its unique withdrawal");
            state.firefox_retired = true;
        }
        if (event == "firefox.workspace.frame_released") {
            const std::string source_id = record.value("source", "");
            const auto source = sources.find(source_id);
            const auto key = std::pair{scalar(record, "presentation_revision"), scalar(record, "transfer_sequence")};
            if (source == sources.end() || source->second.arena != id || !source->second.browser.ready() ||
                source->second.browser.retired() || key.first == 0U || key.second == 0U || scalar(record, "content_session") == 0U ||
                scalar(record, "content_sequence") == 0U)
                reject("release-only completion lacks its exact admitted source transfer");
            else if (!state.releases
                          .emplace(key, SourceRead{source_id, source->second.browser_allocation, scalar(record, "content_session"),
                                                   scalar(record, "content_sequence"), 0U, 0U})
                          .second)
                reject("release-only source transfer completed twice");
        }
        if (event == "firefox.workspace.frame_forwarded" || event == "firefox.workspace.frame_dispatched" ||
            event == "firefox.workspace.copy_completed") {
            const auto publication = scalar(record, "presentation_revision");
            const ReceiverReceipt observed{record.value("source", ""),        scalar(record, "transfer_sequence"),
                                           scalar(record, "layer"),           scalar(record, "slot"),
                                           scalar(record, "content_session"), scalar(record, "content_sequence"),
                                           scalar(record, "content_width"),   scalar(record, "content_height")};
            if (publication == 0U || observed.session == 0U || observed.frame == 0U || observed.width == 0U || observed.height == 0U ||
                !record.contains("layer") || !record.contains("slot") || observed.layer != 0U || observed.slot >= 2U)
                reject("receiver physical receipt has missing or invalid identity");
            auto& receipt = state.receipts[publication];
            if (event == "firefox.workspace.frame_forwarded") {
                const auto source = sources.find(observed.source);
                if (receipt.stage != 0U || observed.transfer == 0U || source == sources.end() || !source->second.browser.live() ||
                    source->second.arena != id || observed.width > source->second.browser_width ||
                    observed.height > source->second.browser_height || scalar(record, "timeline_ready") != observed.transfer * 2U - 1U ||
                    scalar(record, "timeline_release") != observed.transfer * 2U)
                    reject("Firefox forwarding lacks its exact admitted source transfer");
                receipt = observed;
                receipt.stage = 1U;
            } else {
                const unsigned required = event == "firefox.workspace.frame_dispatched" ? 1U : 2U;
                if (receipt.stage != required || receipt.layer != observed.layer || receipt.slot != observed.slot ||
                    receipt.session != observed.session || receipt.frame != observed.frame || receipt.width != observed.width ||
                    receipt.height != observed.height)
                    reject("receiver dispatch or copy completion is missing, duplicate, reordered, or mismatched");
                else
                    ++receipt.stage;
            }
        }
        if (event == "iced.surface.draw_encoded" || event == "iced.frame.draw_settled" || event == "iced.frame.draw_abandoned" ||
            event == "iced.frame.sample_released") {
            const auto publication = scalar(record, "presentation_revision");
            const auto receipt = state.receipts.find(publication);
            if (receipt == state.receipts.end() || receipt->second.stage < 2U ||
                scalar(record, "frame_revision") != receipt->second.frame || scalar(record, "content_session") != receipt->second.session ||
                !record.contains("slot") || scalar(record, "slot") != receipt->second.slot || !record.contains("layer") ||
                scalar(record, "layer") != receipt->second.layer || scalar(record, "content_width") != receipt->second.width ||
                scalar(record, "content_height") != receipt->second.height) {
                reject("Iced reader receipt lacks its exact dispatched sample");
                return;
            }
            auto& custody = state.custody[publication];
            if (custody.released || state.retired != 0U) {
                reject("Iced reader used or released an already retired sample");
            } else if (event == "iced.surface.draw_encoded") {
                if (!custody.acquired || custody.encoded >= custody.selected)
                    reject("Iced encoding lacks exact model-authorized selection");
                else
                    ++custody.encoded;
            } else if (event == "iced.frame.sample_released") {
                if (custody.encoded != custody.settled + custody.abandoned)
                    reject("Iced final sample release preceded encoded reader settlement");
                custody.released = true;
            } else if (custody.settled + custody.abandoned >= custody.encoded) {
                reject("Iced batch settlement is missing, duplicate, or reordered");
            } else if (event == "iced.frame.draw_settled") {
                ++custody.settled;
            } else {
                ++custody.abandoned;
            }
        }
        if (!event.starts_with("iced.surface.")) return;
        const auto generation = scalar(record, "generation");
        if (generation == 0U || !record.contains("width") || !record.contains("height") ||
            (state.iced_generation != 0U && state.iced_generation != generation))
            reject("Iced surface provenance is missing or inconsistent");
        state.iced_generation = generation;
        if (event == "iced.surface.texture_create") {
            if (state.created != 0U || state.retired != 0U) reject("Iced physical texture created twice");
            state.created = browser_ordinal;
        } else if (event == "iced.surface.sample_acquired" || event == "iced.surface.sample_draw_selected") {
            if (state.retired != 0U)
                reject("Iced surface used after texture retirement");
            else if (state.created == 0U || state.firefox_stage != 5U)
                reject("Iced sampled surface lacks texture or complete Firefox import");
            const auto publication = scalar(record, "presentation_revision");
            if (state.samples.size() >= kAcceptanceRecordLimit && !state.samples.contains(publication)) {
                reject("Iced sample identity inventory exhausted");
                return;
            }
            const auto [sample, inserted] = state.samples.emplace(publication, scalar(record, "frame_revision"));
            if (!inserted && sample->second != scalar(record, "frame_revision")) reject("sample publication changed content identity");
            auto& custody = state.custody[scalar(record, "presentation_revision")];
            if (custody.released) reject("Iced reacquired a released sample");
            if (event == "iced.surface.sample_acquired") {
                if (custody.acquired) reject("Iced acquired the same publication twice");
                custody.acquired = true;
                state.acquired = browser_ordinal;
            } else {
                if (!custody.acquired) reject("Iced selected image without an exact sample lease");
                ++custody.selected;
                if (!valid_identity(record.value("requested_surface", "")))
                    reject("Iced selected draw lacks requested capability identity");
                draws.push_back({id, record.value("requested_surface", ""), browser_ordinal});
            }
        } else if (event == "iced.surface.pending_discarded") {
            if (state.created == 0U || state.discarded != 0U) reject("Iced pending discard does not name a unique live import");
            state.discarded = browser_ordinal;
        } else if (event == "iced.surface.renderer_reconstructed") {
            const std::string requested = record.value("requested_surface", "");
            if (state.retired != 0U)
                reject("Iced surface used after texture retirement");
            else if (!valid_identity(requested) || requested == id || state.acquired == 0U)
                reject("pipeline reconstruction lacks its retained sample");
            else {
                if (!admit(requested)) return;
                auto& pending = surfaces[requested];
                if (pending.reconstruction || pending.created == 0U || state.acquired >= pending.created || pending.acquired != 0U ||
                    pending.discarded != 0U || pending.retired != 0U)
                    reject("renderer reconstruction is duplicate or outside its exact pending ownership window");
                else
                    pending.reconstruction = Reconstruction{id, requested, browser_ordinal};
            }
        } else if (event == "iced.surface.import_dropped") {
            if (state.created == 0U || state.import_dropped != 0U) reject("Iced import drop lacks its unique owner");
            state.import_dropped = browser_ordinal;
        } else if (event == "iced.surface.texture_destroyed") {
            if (state.created == 0U || state.retired != 0U) reject("Iced retirement lacks texture creation");
            for (const auto& [publication, custody] : state.custody) {
                if ((custody.acquired && !custody.released) || custody.selected != custody.encoded ||
                    custody.encoded != custody.settled + custody.abandoned)
                    reject("Iced texture destruction preceded its exact final readers");
            }
            state.retired = browser_ordinal;
        }
    }

    [[nodiscard]] bool source_joined(const SourceState& source) const {
        // A withdrawn native request consumes its terminal acknowledgement
        // without installing a timeline. The receiver still proves whether
        // initialization completed or the unclaimed import was cancelled.
        const bool initialized = (source.native.ready() || source.native.retired()) && source.browser.ready();
        const bool cancelled = source.native.retired() && source.browser.retired() && source.browser.cancelled() && !source.native.ready();
        return !source.failed && (initialized || cancelled) && source.generation != 0U &&
               source.width == source.browser_width && source.height == source.browser_height && source.allocation != 0U &&
               source.allocation == source.browser_allocation && valid_identity(source.arena);
    }

    [[nodiscard]] bool receipts_joined(const std::string& id, const SurfaceState& state) const {
        for (const auto& [publication, receipt] : state.receipts) {
            const auto transfer = state.transfers.find({publication, receipt.transfer});
            const auto source = sources.find(receipt.source);
            if (receipt.stage != 3U || transfer == state.transfers.end() || !transfer->second.released || source == sources.end() ||
                !source_joined(source->second) || source->second.arena != id)
                return false;
            const auto& read = transfer->second.read;
            if (read.source != receipt.source || read.allocation != source->second.allocation || read.session != receipt.session ||
                read.frame != receipt.frame || read.width != receipt.width || read.height != receipt.height)
                return false;
        }
        for (const auto& [key, read] : state.releases) {
            const auto transfer = state.transfers.find(key);
            const auto source = sources.find(read.source);
            if (transfer == state.transfers.end() || !transfer->second.released || transfer->second.read.source != read.source ||
                transfer->second.read.allocation != read.allocation || transfer->second.read.session != read.session ||
                transfer->second.read.frame != read.frame || source == sources.end() || !source_joined(source->second) ||
                source->second.arena != id)
                return false;
        }
        for (const auto& [key, transfer] : state.transfers) {
            if (state.failed_publications.contains(key)) continue;
            const auto copy = state.receipts.find(key.first);
            const bool copied = copy != state.receipts.end() && copy->second.transfer == key.second;
            if (!transfer.released || copied == state.releases.contains(key)) return false;
        }
        for (const auto& [publication, custody] : state.custody) {
            if (custody.selected != custody.encoded) return false;
            // Process exit ends the page's remaining readers without delivering
            // JavaScript callbacks. It does not establish a successful draw.
            // A live page or an explicitly destroyed texture still needs every
            // ordinary submission receipt; bridge closure alone is insufficient.
            if (custody.encoded != custody.settled + custody.abandoned && !(browser_exited && state.retired == 0U)) return false;
            if (state.retired != 0U && custody.acquired && !custody.released) return false;
        }
        for (const auto& [publication, frame] : state.samples) {
            const auto receipt = state.receipts.find(publication);
            if (receipt == state.receipts.end() || receipt->second.stage != 3U || receipt->second.frame != frame) return false;
        }
        return true;
    }

    [[nodiscard]] std::string joined_failure() const {
        if (!failure.empty()) return failure;
        for (const auto& [id, state] : surfaces) {
            if (const auto reason = joined_surface_failure(id, state); !reason.empty()) return reason;
        }
        return {};
    }

    [[nodiscard]] std::string joined_surface_failure(const std::string& id, const SurfaceState& state) const {
        if (!receipts_joined(id, state)) return "surface " + id + " lacks exact source, dispatch, or physical copy completion evidence";
        if (state.created == 0U && state.samples.empty() && !state.candidate_withdrawn) return {};
        if (state.firefox_import_failed) {
            const bool native_rejection = state.native_stage == 4U && state.import_failed;
            if (!native_rejection || state.generation != state.iced_generation || state.width != state.browser_width ||
                state.height != state.browser_height || !state.native_retired)
                return "rejected import " + id + " lacks matching native admission and retirement";
            if (state.firefox_stage != 2U || state.created == 0U || state.acquired != 0U || state.discarded <= state.created ||
                state.retired <= state.discarded || !state.samples.empty() || state.firefox_withdrawn || state.firefox_retired)
                return "rejected import " + id + " lacks exact receiver discard and texture retirement";
            return {};
        }
        if (state.native_stage != 4U || state.generation != state.iced_generation || state.width != state.browser_width ||
            state.height != state.browser_height || !state.native_retired)
            return "surface " + id + " lacks matching native/browser import and retirement";
        if (!state.withdrawn && !state.firefox_withdrawn && !state.import_failed && !native_shutdown)
            return "surface " + id + " lacks native or receiver withdrawal before physical retirement";
        if (state.import_failed && state.samples.empty()) return {};
        if (state.import_failed || state.firefox_stage != 5U) return "surface " + id + " sampled or withdrew an incomplete import";
        if ((!state.firefox_retired || state.retired == 0U) && !(native_shutdown && browser_shutdown))
            return "surface " + id + " lacks receiver retirement";
        for (const auto& [publication, frame] : state.samples) {
            const auto native = state.publications.find(publication);
            const bool native_match = native != state.publications.end() && native->second == frame;
            if (publication == 0U || frame == 0U || !native_match)
                return "sampled surface " + id + " lacks matching native frame publication";
        }
        if (state.discarded != 0U && (state.retired == 0U || (!state.withdrawn && !state.firefox_withdrawn)))
            return "discarded pending surface " + id + " lacks withdrawal and texture retirement";
        return {};
    }

    [[nodiscard]] bool evidence_settled() const {
        if (!failure.empty()) return false;
        for (const auto& [id, source] : sources) {
            if (!source_joined(source)) return false;
            if (source.native.retired() && !source.browser.retired() && !browser_shutdown) return false;
        }
        for (const auto& [id, state] : surfaces) {
            if (state.native_stage != 4U || !receipts_joined(id, state)) return false;
            for (const auto& [identity, transfer] : state.transfers)
                if (!transfer.released && !state.failed_publications.contains(identity)) return false;
            if (state.native_retired && !joined_surface_failure(id, state).empty()) return false;
            for (const auto& [publication, frame] : state.samples) {
                const auto produced = state.publications.find(publication);
                if (produced == state.publications.end() || produced->second != frame) return false;
            }
            for (const auto& [operation, stage] : state.source_steps)
                // A successfully borrowed source may be superseded before any
                // copy is submitted. Actual publications still require stage 3.
                if (stage == 1U && !state.failed_source_operations.contains(operation)) return false;
            for (const auto& [publication, stage] : state.publication_steps)
                if (stage != 3U && !state.failed_publications.contains(publication)) return false;
        }
        return true;
    }

    void SettleScenario() {
        for (auto iterator = surfaces.begin(); iterator != surfaces.end();) {
            auto& [id, state] = *iterator;
            if (state.native_retired && (state.firefox_retired || state.firefox_import_failed) && state.retired != 0U) {
                if (const auto reason = joined_surface_failure(id, state); !reason.empty()) { return; }
                iterator = surfaces.erase(iterator);
                continue;
            }
            // A scenario boundary is not a release of the receiver-owned image.
            // Retain bounded physical publication/copy provenance, including
            // incomplete independent-stream joins, until actual retirement.
            ++iterator;
        }
        std::erase_if(sources, [&](const auto& item) {
            const auto& [id, source] = item;
            return source.native.retired() && source.browser.retired() && source_joined(source) && !surfaces.contains(source.arena);
        });
        draws.clear();
    }

    [[nodiscard]] bool pending_supersession_completed() const {
        if (!failure.empty()) return false;
        for (const auto& [b, pending] : surfaces) {
            if (!pending.candidate_withdrawn || pending.created == 0U || pending.acquired != 0U || !pending.publications.empty() ||
                pending.discarded <= pending.created || pending.retired <= pending.discarded || !pending.reconstruction ||
                pending.reconstruction->requested != b || pending.reconstruction->ordinal <= pending.created ||
                pending.reconstruction->ordinal >= pending.discarded || !pending.firefox_retired || !pending.native_retired)
                continue;
            const auto& active_identity = pending.reconstruction->completed;
            const auto& active = surfaces.at(active_identity);
            std::size_t latest_prior_acquisition = 0U;
            for (const auto& [_, candidate] : surfaces)
                if (candidate.acquired < pending.created) latest_prior_acquisition = std::max(latest_prior_acquisition, candidate.acquired);
            if (active.generation >= pending.generation || active.acquired == 0U || active.acquired != latest_prior_acquisition) continue;
            for (const auto& draw : draws) {
                const auto& latest = surfaces.at(draw.selected);
                if (latest.generation > pending.generation && latest.created > pending.created && latest.acquired > pending.retired &&
                    draw.ordinal > latest.acquired && draw.requested == draw.selected)
                    return true;
            }
        }
        return false;
    }
};

struct PixelBoundaryAudit final {
    explicit PixelBoundaryAudit(const bool pixel_enabled = true) : enabled(pixel_enabled) {}
    bool enabled = true;
    using Key = std::pair<std::string, std::uint64_t>;
    struct Sample final {
        std::uint64_t x = 0U, y = 0U;
        std::uint32_t rgba = 0U;
        bool operator==(const Sample&) const = default;
    };
    struct Boundary final {
        nlohmann::json identity;
        nlohmann::json publication_fact;
        std::array<std::optional<Sample>, 25> values{};
        [[nodiscard]] bool complete() const {
            return std::ranges::all_of(values, [](const auto& value) { return value.has_value(); });
        }
    };
    struct Publication final {
        // Reoffers retain the Presentation revision but own different timeline
        // transfers. The Firefox physical receipt selects its native attempt.
        std::map<std::pair<std::string, std::uint64_t>, Boundary> native;
        std::array<Boundary, 4> receivers;
        nlohmann::json forwarded;
        std::array<bool, 3> counted{};
        bool viewer = false;
    };
    std::map<Key, Publication> samples;
    std::vector<nlohmann::json> probe_failures;
    std::set<std::string> failed_probe_retirements;

    struct ProbeFailureEvidence final {
        bool forwarded = false;
        bool recovered = false;
        [[nodiscard]] bool complete() const noexcept { return forwarded && recovered; }
    };

    [[nodiscard]] ProbeFailureEvidence probe_failure_evidence(std::string_view expected) const {
        if (expected.empty() || !failure.empty() || probe_failures.size() != 1U) return {};
        const auto& failed = probe_failures.front();
        std::string boundary{expected};
        boundary.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(boundary.front())));
        if (failed.value("boundary", "") != boundary) return {};
        const auto source = failed.value("source", "");
        if (!SurfaceAudit::valid_identity(source)) return {};
        const bool allocation = expected == "allocation";
        bool forwarded = false, recovered = false;
        for (const auto& [key, publication] : samples) {
            if (publication.forwarded.empty()) continue;
            const auto& receipt = publication.receivers[0].identity;
            const bool same_source = publication.forwarded.value("workspace_source", "") == source;
            if (same_source) {
                for (const auto receiver : {0U, 1U}) {
                    const auto& evidence = publication.receivers[receiver].identity;
                    if (!evidence.empty() && (allocation || scalar(evidence, "transfer_sequence") == scalar(failed, "transfer_sequence")))
                        return {};
                }
                const bool same_attempt = key.second == scalar(failed, "presentation_revision") &&
                                          scalar(publication.forwarded, "transfer_sequence") == scalar(failed, "transfer_sequence");
                if (!publication.forwarded.empty() && (allocation || same_attempt)) {
                    if (publication.forwarded.value("pixel_probe", true)) return {};
                    if (!allocation) {
                        for (const auto* field : {"layer", "slot", "content_session", "content_sequence"})
                            if (scalar(publication.forwarded, field) != scalar(failed, field)) return {};
                    }
                    forwarded = true;
                }
            }
            // The failed import may be retired and replaced. Correlate recovery
            // by the continuing logical content stream rather than requiring
            // the replacement to reuse a physical surface or mailbox slot.
            if (!allocation && !receipt.empty() && scalar(receipt, "layer") == scalar(failed, "layer") &&
                scalar(receipt, "content_session") == scalar(failed, "content_session") &&
                scalar(receipt, "content_sequence") == scalar(failed, "content_sequence") &&
                scalar(receipt, "presentation_revision") > scalar(failed, "presentation_revision") &&
                std::ranges::all_of(publication.counted, [](bool value) { return value; }))
                recovered = true;
            if (allocation && !same_source && !receipt.empty() &&
                scalar(receipt, "presentation_revision") > scalar(failed, "presentation_revision") &&
                std::ranges::all_of(publication.counted, [](bool value) { return value; }))
                recovered = true;
        }
        return {.forwarded = forwarded || (allocation && failed_probe_retirements.contains(source)), .recovered = recovered};
    }

    [[nodiscard]] bool probe_failure_complete(std::string_view expected) const {
        if (expected.empty()) return probe_failures.empty();
        return probe_failure_evidence(expected).complete() && raw_complete() && viewer_nonblack_complete();
    }
    std::array<std::size_t, 3> joined{};
    struct Composition final {
        nlohmann::json identity;
        std::map<std::uint64_t, std::array<std::optional<nlohmann::json>, 4>> cards;
        std::set<std::uint64_t> expected;
        bool summary = false;
        [[nodiscard]] bool complete() const {
            return summary && !expected.empty() && cards.size() == expected.size() && std::ranges::all_of(cards, [&](const auto& card) {
                       return expected.contains(card.first) &&
                              std::ranges::all_of(card.second, [](const auto& value) { return value.has_value(); });
                   });
        }
    };
    using CompositionKey = std::tuple<std::uint64_t, std::string, std::uint64_t>;
    std::map<CompositionKey, Composition> compositions;
    std::string failure;
    bool retained_logical_content = false;
    bool upscale_growth = false;
    bool canvas_seen = false;
    std::size_t viewer_canvas_joins = 0U;
    std::set<std::uint64_t> cached_methods;
    std::vector<std::uint64_t> stop_observations;
    std::uint64_t departure_begin = 0U;
    std::uint64_t departure_end = 0U;
    bool settings_preserved = false;
    bool basic_reentry = false;
    bool reconnected = false;
    unsigned navigation_stage = 0U;
    bool route_persistence = false;
    bool authoritative_route = false;
    std::uint64_t route_revision = 0U;
    std::array<std::uint64_t, 3> reentry_product{};
    std::optional<std::pair<std::uint64_t, std::uint64_t>> successful_viewer;

    void consume_continuity(const nlohmann::json& record, std::string_view event) {
        if (!enabled) return;
        const auto control = textual(record, "control");
        const auto detail = textual(record, "detail");
        if (event == "integration.viewer_departure_started") {
            navigation_stage = 1U;
            route_persistence = scalar(record, "b") != 0U;
            route_revision = scalar(record, "c");
            authoritative_route = false;
        } else if (event == "integration.navigation_message" && (navigation_stage == 1U || navigation_stage == 5U)) {
            const bool train = navigation_stage == 1U;
            if (control != (train ? "navigation.train" : "navigation.explore") || detail != (train ? "Explore" : "Train"))
                reject("wrong mapped navigation message path");
            else
                ++navigation_stage;
        } else if (event == "integration.navigation_outcome" && navigation_stage > 0U && navigation_stage < 8U) {
            const bool train = navigation_stage == 2U;
            if ((!train && navigation_stage != 6U) || control != (train ? "navigation.train" : "navigation.explore") ||
                detail != (train ? "Train" : "Explore"))
                reject("navigation outcome lacks its mapped root/router message");
            else
                ++navigation_stage;
        } else if (event == "integration.route_state" && (navigation_stage == 3U || navigation_stage == 7U)) {
            if ((detail == "settings.reply" || detail == "settings.event") && scalar(record, "a") == 1U &&
                control == (navigation_stage == 3U ? "navigation.train" : "navigation.explore"))
                authoritative_route = true;
        } else if (event == "integration.viewer_route_confirmed") {
            const bool train = navigation_stage == 3U;
            if ((!train && navigation_stage != 7U) || control != (train ? "navigation.train" : "navigation.explore") ||
                (train ? detail != "None" : (detail != "Explore" && detail != "Upscale")) || scalar(record, "c") != 1U ||
                (scalar(record, "b") != 0U) != route_persistence ||
                (route_persistence && (!authoritative_route || scalar(record, "a") <= route_revision))) {
                reject("mapped route foreground or authoritative persistence is incomplete");
            } else {
                ++navigation_stage;
                route_revision = scalar(record, "a");
                authoritative_route = false;
            }
        } else if (event == "integration.viewer_abandoned") {
            if (navigation_stage != 4U)
                reject("viewer abandoned before mapped departure confirmation");
            else
                navigation_stage = 5U;
        } else if (event == "integration.viewer_basic_reentry") {
            if (navigation_stage != 8U || scalar(record, "a") == 0U || scalar(record, "b") == 0U)
                reject("automatic Basic lacks mapped reentry and completed draw");
            else {
                reentry_product = {scalar(record, "a"), scalar(record, "c"), scalar(record, "d")};
                navigation_stage = 9U;
            }
        } else if (event == "integration.viewer_reconnected") {
            if (navigation_stage != 9U || scalar(record, "b") == 0U ||
                reentry_product != std::array{scalar(record, "a"), scalar(record, "c"), scalar(record, "d")})
                reject("reconnect did not restore the same completed viewer product");
            else
                navigation_stage = 10U;
            successful_viewer = std::pair{scalar(record, "b"), scalar(record, "a")};
        } else if (event == "integration.explore_reopened" && navigation_stage == 10U) {
            if (detail == "usable-after-reopen" && scalar(record, "c") > 0U && scalar(record, "d") > 0U) navigation_stage = 11U;
        }
        if (event == "integration.viewer_complete" && detail != "copy")
            successful_viewer = std::pair{scalar(record, "a"), scalar(record, "b")};
    }

    [[nodiscard]] bool raw_complete() const {
        return failure.empty() && std::ranges::any_of(samples, [](const auto& entry) {
                   return std::ranges::all_of(entry.second.counted, [](bool counted) { return counted; });
               });
    }
    [[nodiscard]] bool viewer_nonblack_complete() const {
        if (!successful_viewer || !failure.empty()) return false;
        return std::ranges::any_of(samples, [&](const auto& entry) {
            const auto& publication = entry.second;
            if (entry.first.second != successful_viewer->first || !publication.viewer ||
                !std::ranges::all_of(publication.counted, [](bool counted) { return counted; }) ||
                scalar(publication.receivers[3].identity, "content_sequence") != successful_viewer->second)
                return false;
            const auto has_color = [](const auto& boundary) {
                return std::ranges::any_of(boundary.values, [](const auto& value) { return value && colored(value->rgba); });
            };
            const auto& canvas = publication.receivers[3];
            const auto& imported = publication.receivers[0];
            const auto& mailbox = publication.receivers[1];
            const auto& owned = publication.receivers[2];
            const auto attempt = publication.native.find(
                {publication.forwarded.value("workspace_source", ""), scalar(imported.identity, "transfer_sequence")});
            const bool canvas_sampled = std::ranges::any_of(canvas.values, [](const auto& value) { return value.has_value(); });
            if (attempt == publication.native.end() || !imported.complete() || !mailbox.complete() || !owned.complete() ||
                !canvas_sampled || canvas.identity != owned.identity)
                return false;
            const auto& native = attempt->second;
            return native.complete() && !native.publication_fact.empty() && native.identity == native.publication_fact &&
                   scalar(native.identity, "source_revision") == successful_viewer->second && has_color(native) && has_color(canvas);
        });
    }

    static bool colored(std::uint32_t value) {
        return (value >> 24U) != 0U && ((value & 255U) > 8U || ((value >> 8U) & 255U) > 8U || ((value >> 16U) & 255U) > 8U);
    }
    static bool black(std::uint32_t value) { return (value & 0x00ffffffU) == 0U; }
    void reject(std::string_view reason) {
        if (failure.empty()) failure = reason;
    }
    static nlohmann::json identity_of(const nlohmann::json& record, std::size_t boundary) {
        nlohmann::json identity = nlohmann::json::object();
        const auto copy = [&](const char* field) { identity[field] = scalar(record, field); };
        for (const auto* field : {"presentation_revision"})
            copy(field);
        if (boundary == 0U) {
            identity["workspace_source"] = SurfaceAudit::native_identity(record, true);
            copy("workspace_allocation");
            for (const auto* field :
                 {"source_session", "source_instance", "source_revision", "clean_revision", "source_observation_revision", "source_width",
                  "source_height", "content_x", "content_y", "content_width", "content_height", "allocation_generation", "capacity_width",
                  "capacity_height", "transfer_sequence", "timeline_ready"})
                copy(field);
        } else {
            for (const auto* field : {"content_session", "content_width", "content_height", "layer", "slot"})
                copy(field);
            identity["content_sequence"] = scalar(record, boundary < 3U ? "content_sequence" : "frame_revision");
            if (boundary < 3U) {
                for (const auto* field : {"transfer_sequence", "timeline_ready", "timeline_release"})
                    copy(field);
            } else {
                for (const auto* field : {"allocation_generation", "width", "height"})
                    copy(field);
            }
        }
        return identity;
    }
    void reconcile(Publication& publication) {
        const auto& imported = publication.receivers[0];
        if (imported.identity.empty() || publication.forwarded.empty()) return;
        for (const auto* field : {"content_session", "content_sequence", "presentation_revision", "content_width", "content_height",
                                  "layer", "slot", "transfer_sequence", "timeline_ready", "timeline_release"})
            if (scalar(imported.identity, field) != scalar(publication.forwarded, field))
                reject("Firefox pixel receipt differs from the forwarded physical mailbox");
        if (scalar(imported.identity, "layer") !=
            static_cast<std::uint64_t>(mmltk::controller::presentation::WorkspacePresentationLayer::Primary))
            reject("Firefox pixel receipt does not name the native Primary layer");
        const auto native =
            publication.native.find({publication.forwarded.value("workspace_source", ""), scalar(imported.identity, "transfer_sequence")});
        if (native == publication.native.end()) return;
        const auto& source = native->second;
        const auto& fact = source.identity;
        if (fact.empty()) return;
        // Independent streams can arrive in either order. Settlement requires
        // the native publication, even after a complete receiver probe batch.
        if (source.publication_fact.empty()) return;
        if (fact != source.publication_fact) reject("native pixels differ from the canonical publication fact");
        if (fact.value("workspace_source", "") != publication.forwarded.value("workspace_source", ""))
            reject("pixel bridge names a different producer source");
        const auto width = scalar(fact, "source_width"), height = scalar(fact, "source_height");
        if (width == 0U || height == 0U || width > scalar(fact, "capacity_width") || height > scalar(fact, "capacity_height") ||
            scalar(fact, "source_session") == 0U || scalar(fact, "source_instance") == 0U || scalar(fact, "source_revision") == 0U ||
            scalar(fact, "clean_revision") == 0U || scalar(fact, "source_observation_revision") == 0U ||
            scalar(fact, "allocation_generation") == 0U || scalar(fact, "content_x") + scalar(fact, "content_width") > width ||
            scalar(fact, "content_y") + scalar(fact, "content_height") > height ||
            scalar(fact, "timeline_ready") != scalar(fact, "transfer_sequence") * 2U - 1U) {
            reject("invalid native product, content, allocation, or transfer fact");
            return;
        }
        std::array<const Boundary*, 4> raw{&source, &publication.receivers[0], &publication.receivers[1], &publication.receivers[2]};
        for (std::size_t owner = 1U; owner < raw.size(); ++owner) {
            const auto& identity = raw[owner]->identity;
            if (identity.empty()) continue;
            if (scalar(identity, "content_session") != scalar(fact, "source_session") ||
                scalar(identity, "content_sequence") != scalar(fact, "source_revision") || scalar(identity, "content_width") != width ||
                scalar(identity, "content_height") != height || scalar(identity, "layer") != scalar(imported.identity, "layer") ||
                scalar(identity, "slot") != scalar(imported.identity, "slot") || scalar(identity, "layer") >= 3U ||
                scalar(identity, "slot") >= 2U)
                reject("receiver content or physical mailbox identity differs");
            if (owner < 3U && (scalar(identity, "transfer_sequence") != scalar(fact, "transfer_sequence") ||
                               scalar(identity, "timeline_ready") != scalar(fact, "timeline_ready") ||
                               scalar(identity, "timeline_release") != scalar(fact, "timeline_ready") + 1U))
                reject("Firefox physical timeline receipt differs");
            if (owner == 3U && (scalar(identity, "allocation_generation") != scalar(fact, "allocation_generation") ||
                                scalar(identity, "width") != scalar(fact, "capacity_width") ||
                                scalar(identity, "height") != scalar(fact, "capacity_height")))
                reject("Iced allocation differs from native publication");
        }
        const auto coordinate = [](std::size_t index, std::uint64_t size) {
            return std::array<std::uint64_t, 5>{0U, std::min(191UL, size - 1U), std::min(383UL, size - 1U), (size - 1U) / 2U,
                                                size - 1U}[index];
        };
        for (std::size_t owner = 0U; owner < raw.size(); ++owner) {
            for (std::size_t index = 0U; index < 25U; ++index) {
                const auto& sample = raw[owner]->values[index];
                if (!sample) continue;
                if (sample->x != coordinate(index % 5U, width) || sample->y != coordinate(index / 5U, height))
                    reject("raw probe coordinate differs from logical-content multiset");
                if (owner != 0U && raw[owner - 1U]->values[index] && sample != raw[owner - 1U]->values[index])
                    reject("ordered raw RGBA or alpha divergence");
            }
            if (owner != 0U && !publication.counted[owner - 1U] && raw[owner - 1U]->complete() && raw[owner]->complete() &&
                failure.empty()) {
                publication.counted[owner - 1U] = true;
                ++joined[owner - 1U];
            }
        }
        if (publication.viewer) {
            const auto& canvas = publication.receivers[3];
            if (!canvas.identity.empty() && !raw[3]->identity.empty() && canvas.identity != raw[3]->identity)
                reject("canvas does not name the sampled physical publication");
            for (std::size_t index = 0U; index < 25U; ++index) {
                if (canvas.values[index] &&
                    (canvas.values[index]->x != coordinate(index % 5U, width) || canvas.values[index]->y != coordinate(index / 5U, height)))
                    reject("canvas probe does not name its logical source sample");
                if (canvas.values[index] && raw[3]->values[index] && colored(raw[3]->values[index]->rgba) &&
                    black(canvas.values[index]->rgba))
                    reject("completed sample produced an unexplained black viewer canvas");
            }
        }
        if (std::ranges::all_of(publication.counted, [](const bool value) { return value; })) {
            // Automatic Basic may supersede the raw detail before display.
            // Any completely joined logical crop proves high-water sampling.
            if (width < scalar(fact, "capacity_width") || height < scalar(fact, "capacity_height")) retained_logical_content = true;
            if (width == 1536U && height == 1536U) upscale_growth = true;
        }
    }

    void consume(const nlohmann::json& record) {
        const std::string event = record.value("event", "");
        consume_continuity(record, event);
        if (event == "firefox.workspace.probe_failed") {
            if (!enabled || probe_failures.size() >= 16U)
                reject("unexpected or excessive probe preparation failures");
            else
                probe_failures.push_back(record);
            return;
        }
        if (event == "firefox.workspace.source.retired") {
            const auto surface = record.value("surface", "");
            if (!surface.empty() &&
                std::ranges::any_of(probe_failures, [&](const auto& failed) { return failed.value("source", "") == surface; }))
                failed_probe_retirements.insert(surface);
            return;
        }
        if (event == "upscale.stop.requested") {
            if (stop_observations.size() < kAcceptanceRecordLimit)
                stop_observations.push_back(scalar(record, "observation_revision"));
            else
                reject("Stop observations exceeded bounded acceptance capacity");
        }
        if (event == "integration.upscale_cached") {
            const auto method = scalar(record, "c");
            if (method < 3U)
                cached_methods.insert(method);
            else
                reject("unknown cached Upscale method");
        }
        if (event == "integration.viewer_departure_started") departure_begin = scalar(record, "a");
        if (event == "integration.viewer_abandoned") departure_end = scalar(record, "a");
        if (event == "integration.viewer_settings_preserved") settings_preserved = true;
        if (event == "integration.viewer_basic_reentry") basic_reentry = true;
        if (event == "integration.viewer_reconnected") reconnected = true;
        if (event == "integration.atlas_composition" || event == "integration.atlas_composition_complete") {
            const CompositionKey key{scalar(record, "columns"), record.value("surface", ""), scalar(record, "presentation_revision")};
            if ((std::get<0>(key) != 4U && std::get<0>(key) != 10U) || std::get<1>(key).empty() || std::get<2>(key) == 0U) {
                reject("invalid composition publication or columns");
                return;
            }
            if (!compositions.contains(key) && compositions.size() >= kAcceptanceRecordLimit) {
                reject("composition evidence exceeded bounded publication capacity");
                return;
            }
            auto& composition = compositions[key];
            const auto identity = identity_of(record, 3U);
            if (!composition.identity.empty() && composition.identity != identity)
                reject("composition mixed physical publications");
            else
                composition.identity = identity;
            if (event == "integration.atlas_composition_complete") {
                if (!record.contains("cards") || !record["cards"].is_array() || record["cards"].empty() || record["cards"].size() > 256U) {
                    reject("invalid expected composition card set");
                    return;
                }
                std::set<std::uint64_t> expected;
                for (const auto& card : record["cards"])
                    expected.insert(card.get<std::uint64_t>());
                if (expected.size() != record["cards"].size() || scalar(record, "emitted") != expected.size() * 4U ||
                    (composition.summary && composition.expected != expected))
                    reject("conflicting or incomplete composition summary");
                composition.expected = std::move(expected);
                composition.summary = true;
                for (const auto& [card, values] : composition.cards)
                    if (!composition.expected.contains(card)) reject("unexpected composition card");
            } else {
                const auto card = scalar(record, "card"), kind = scalar(record, "kind");
                if (kind >= 4U || (!composition.cards.contains(card) && composition.cards.size() >= 256U)) {
                    reject("composition card or kind exceeds bounded contract");
                    return;
                }
                if (composition.summary && !composition.expected.contains(card)) reject("extra composition card");
                const auto valid_channels = [&](const char* field) {
                    return record.contains(field) && record[field].is_array() && record[field].size() == 4U &&
                           std::ranges::all_of(record[field], [](const auto& value) {
                               return value.is_number() && value.template get<double>() >= 0.0 && value.template get<double>() <= 255.0;
                           });
                };
                if (!valid_channels("expected") || !valid_channels("observed")) {
                    reject("invalid composition RGBA arrays");
                    return;
                }
                for (std::size_t channel = 0U; channel < 4U; ++channel)
                    if (std::abs(record["expected"][channel].get<double>() - record["observed"][channel].get<double>()) > 4.0)
                        reject("deterministic composition RGBA differs");
                for (const auto* axis : {"x", "y"}) {
                    const std::string sample_field = std::string{"sample_"} + axis;
                    const std::string canvas_field = std::string{"canvas_"} + axis;
                    const std::string origin_field = std::string{"image_"} + axis;
                    const auto dimension = std::string_view{axis} == "x" ? "width" : "height";
                    const auto content =
                        std::string_view{axis} == "y" && scalar(record, "rows") != 0U && scalar(record, "card_extent") != 0U
                            ? scalar(record, "rows") * scalar(record, "card_extent")
                            : scalar(record, (std::string{"content_"} + dimension).c_str());
                    const auto extent = record.value(std::string{"image_"} + dimension, 0.0);
                    const auto sample = record.value(sample_field, -1.0);
                    const auto canvas = record.value(canvas_field, -1.0);
                    if (content == 0U || extent <= 0.0 || !std::isfinite(canvas) || sample < 0.0 ||
                        sample >= static_cast<double>(content) ||
                        std::abs(canvas - (record.value(origin_field, 0.0) + sample * extent / static_cast<double>(content))) > 0.01)
                        reject("composition transformed canvas coordinate differs");
                }
                auto& value = composition.cards[card][kind];
                auto stable_record = record;
                stable_record.erase("elapsed_ms");
                stable_record.erase("timeline_ready");
                if (value && *value != stable_record)
                    reject("duplicate-conflicting composition card sample");
                else
                    value = std::move(stable_record);
            }
            return;
        }
        std::size_t boundary = 0U;
        if (event == "presentation.pixel" || event == "presentation.frame.edge")
            boundary = 0U;
        else if (event == "firefox.workspace.frame_forwarded")
            boundary = 1U;
        else if (event == "firefox.workspace.pixel") {
            const auto owner = record.value("boundary", "");
            if (owner != "import" && owner != "mailbox") {
                reject("unknown Firefox pixel owner");
                return;
            }
            boundary = owner == "import" ? 1U : 2U;
        } else if (event == "iced.surface.pixel")
            boundary = 3U;
        else if (event == "iced.surface.canvas_pixel") {
            canvas_seen = true;
            if (record.value("control", "") != "explore.detail.workspace") return;
            boundary = 4U;
        } else
            return;
        if (!enabled && (event == "presentation.frame.edge" || event == "firefox.workspace.frame_forwarded")) return;
        const std::string surface = boundary == 0U ? SurfaceAudit::native_identity(record) : record.value("surface", "");
        const Key key{surface, scalar(record, "presentation_revision")};
        if (surface.empty() || key.second == 0U) {
            reject("invalid physical pixel publication");
            return;
        }
        if (!samples.contains(key) && samples.size() >= kAcceptanceRecordLimit) {
            if (failure.empty()) failure = "pixel evidence exceeded its bounded acceptance capacity";
            return;
        }
        auto& publication = samples[key];
        if (event == "firefox.workspace.frame_forwarded") {
            auto identity = identity_of(record, 1U);
            identity["workspace_source"] = record.value("source", "");
            if (!SurfaceAudit::valid_identity(identity["workspace_source"].get<std::string>())) {
                reject("forwarded pixel bridge omitted exact producer source");
                return;
            }
            identity["pixel_probe"] = record.value("pixel_probe", false);
            if (!publication.forwarded.empty() && publication.forwarded != identity)
                reject("forwarded physical mailbox changed within a publication");
            else
                publication.forwarded = identity;
            reconcile(publication);
            return;
        }
        const auto transfer = std::pair{SurfaceAudit::native_identity(record, true), scalar(record, "transfer_sequence")};
        if (boundary == 0U && (!SurfaceAudit::valid_identity(transfer.first) || scalar(record, "workspace_allocation") == 0U)) {
            reject("native pixel fact omitted exact workspace allocation");
            return;
        }
        if (boundary == 0U && !publication.native.contains(transfer) && publication.native.size() >= 16U) {
            reject("native publication reoffers exceeded bounded evidence capacity");
            return;
        }
        auto& target = boundary == 0U ? publication.native[transfer] : publication.receivers[boundary - 1U];
        const auto identity = identity_of(record, boundary);
        if (event == "presentation.frame.edge") {
            target.publication_fact = identity;
            reconcile(publication);
            return;
        }
        if (!target.identity.empty() && target.identity != identity)
            reject("immutable owner identity changed");
        else
            target.identity = identity;
        const auto index = scalar(record, "sample_index");
        if (index >= 25U) {
            reject("pixel index exceeds fixed sample contract");
            return;
        }
        const Sample value{scalar(record, "sample_x"), scalar(record, "sample_y"),
                           static_cast<std::uint32_t>(scalar(record, "sample_rgba"))};
        if (boundary != 4U && target.values[index] && target.values[index] != value) reject("immutable owner pixel changed");
        target.values[index] = value;
        publication.viewer = publication.viewer || boundary == 4U;
        if (publication.viewer && boundary == 4U) ++viewer_canvas_joins;
        reconcile(publication);
    }

    [[nodiscard]] bool composition_complete() const {
        return failure.empty() && std::ranges::all_of(std::array{4U, 10U}, [&](const auto columns) {
                   return std::ranges::any_of(
                       compositions, [&](const auto& value) { return std::get<0>(value.first) == columns && value.second.complete(); });
               });
    }
    [[nodiscard]] bool continuity_complete(bool require_gallery = true) const {
        return failure.empty() && (!enabled || navigation_stage >= (require_gallery ? 11U : 10U)) && cached_methods.size() == 3U &&
               settings_preserved && basic_reentry && reconnected && departure_begin != 0U && departure_end > departure_begin &&
               std::ranges::count_if(stop_observations,
                                     [&](const auto revision) { return revision >= departure_begin && revision < departure_end; }) == 1;
    }
};

TEST_CASE("pixel evidence joins exact physical samples and includes alpha", "[workspace][audit][pixel]") {
    const nlohmann::json native{{"event", "presentation.pixel"}, {"surface_high", 1U},         {"surface_low", 2U},
                                {"presentation_revision", 7U},   {"source_session", 1U},       {"source_instance", 3U},
                                {"source_revision", 9U},         {"clean_revision", 8U},       {"source_observation_revision", 11U},
                                {"source_width", 384U},          {"source_height", 384U},      {"content_width", 384U},
                                {"content_height", 384U},        {"capacity_width", 894U},     {"capacity_height", 1080U},
                                {"allocation_generation", 2U},   {"transfer_sequence", 4U},    {"timeline_ready", 7U},
                                {"workspace_source_high", 3U},   {"workspace_source_low", 4U}, {"workspace_allocation", 8U},
                                {"sample_rgba", 0xff705030U}};
    const std::string surface = SurfaceAudit::native_identity(native);
    const auto pixel = [&](const char* event, const char* boundary) {
        return nlohmann::json{{"event", event},
                              {"boundary", boundary},
                              {"surface", surface},
                              {"source", "00000000000000030000000000000004"},
                              {"presentation_revision", 7U},
                              {"content_session", 1U},
                              {"content_sequence", 9U},
                              {"frame_revision", 9U},
                              {"content_width", 384U},
                              {"content_height", 384U},
                              {"width", 894U},
                              {"height", 1080U},
                              {"allocation_generation", 2U},
                              {"transfer_sequence", 4U},
                              {"timeline_ready", 7U},
                              {"timeline_release", 8U},
                              {"layer", 0U},
                              {"slot", 1U},
                              {"sample_rgba", 0xff705030U}};
    };
    auto imported = pixel("firefox.workspace.pixel", "import");
    auto mailbox = pixel("firefox.workspace.pixel", "mailbox");
    auto owned = pixel("iced.surface.pixel", "");
    const auto fill = [&](PixelBoundaryAudit& audit, nlohmann::json changed, bool missing = false, bool all_black = false,
                          std::string_view missing_owner = {}, std::uint64_t publication = 7U) {
        constexpr std::array<unsigned, 5> coordinates{0U, 191U, 383U, 191U, 383U};
        for (std::size_t index = 0U; index < 25U; ++index) {
            for (auto record : {changed, mailbox, imported, native}) {
                if (!missing_owner.empty() && (record.value("boundary", "") == missing_owner || record.value("event", "") == missing_owner))
                    continue;
                if (missing && index == 24U && record.value("event", "") == "iced.surface.pixel") continue;
                record["presentation_revision"] = publication;
                record["sample_index"] = index;
                if (!record.contains("sample_x")) record["sample_x"] = coordinates[index % 5U];
                record["sample_y"] = coordinates[index / 5U];
                if (all_black) record["sample_rgba"] = 0xff000000U;
                audit.consume(record);
            }
        }
        auto edge = native;
        edge["event"] = "presentation.frame.edge";
        edge["presentation_revision"] = publication;
        if (missing_owner != "presentation.frame.edge") audit.consume(edge);
        auto forwarded = imported;
        forwarded["event"] = "firefox.workspace.frame_forwarded";
        forwarded["presentation_revision"] = publication;
        audit.consume(forwarded);
    };
    PixelBoundaryAudit valid;
    fill(valid, owned);
    CHECK(valid.failure.empty());
    CHECK(std::ranges::all_of(valid.joined, [](auto count) { return count != 0U; }));
    CHECK(valid.retained_logical_content);
    for (const auto field : {"sample_rgba", "content_session", "frame_revision", "allocation_generation", "layer", "slot", "sample_x"}) {
        auto changed = owned;
        changed[field] = field == std::string_view{"sample_rgba"} ? 0x00705030U : 8U;
        PixelBoundaryAudit audit;
        fill(audit, changed);
        CHECK_FALSE(audit.failure.empty());
    }
    PixelBoundaryAudit partial;
    fill(partial, owned, true);
    CHECK(partial.joined.back() == 0U);
    CHECK_FALSE(partial.retained_logical_content);
    for (const auto field :
         {"source_instance", "clean_revision", "source_observation_revision", "content_x", "capacity_width", "timeline_ready"}) {
        PixelBoundaryAudit audit;
        fill(audit, owned);
        auto edge = native;
        edge["event"] = "presentation.frame.edge";
        edge[field] = 99U;
        audit.consume(edge);
        CHECK_FALSE(audit.raw_complete());
    }
    auto canvas = pixel("iced.surface.canvas_pixel", "");
    canvas["control"] = "explore.detail.workspace";
    canvas["sample_index"] = 0U;
    canvas["sample_x"] = 0U;
    canvas["sample_y"] = 0U;
    canvas["sample_rgba"] = 0xff000000U;
    PixelBoundaryAudit black_viewer;
    fill(black_viewer, owned);
    black_viewer.consume(canvas);
    CHECK(black_viewer.viewer_canvas_joins == 1U);
    CHECK_FALSE(black_viewer.failure.empty());
    canvas["sample_rgba"] = 0xff060503U;
    PixelBoundaryAudit dimmed_viewer;
    fill(dimmed_viewer, owned);
    dimmed_viewer.consume(canvas);
    CHECK(dimmed_viewer.failure.empty());
    PixelBoundaryAudit all_black;
    canvas["sample_rgba"] = 0xff000000U;
    fill(all_black, owned, false, true);
    all_black.consume(canvas);
    all_black.consume({{"event", "integration.viewer_complete"}, {"detail", "square"}, {"a", 7U}, {"b", 9U}});
    CHECK(all_black.failure.empty());
    CHECK(all_black.raw_complete());
    CHECK_FALSE(all_black.viewer_nonblack_complete());
    canvas["sample_rgba"] = 0xff705030U;
    const nlohmann::json selected{{"event", "integration.viewer_complete"}, {"detail", "square"}, {"a", 7U}, {"b", 9U}};
    PixelBoundaryAudit colored_viewer;
    fill(colored_viewer, owned);
    colored_viewer.consume(canvas);
    colored_viewer.consume(selected);
    CHECK(colored_viewer.viewer_nonblack_complete());
    canvas["control"] = "workflow.visual.workspace";
    canvas["sample_rgba"] = 0xff000000U;
    colored_viewer.consume(canvas);
    CHECK(colored_viewer.failure.empty());
    canvas["control"] = "explore.detail.workspace";
    canvas["sample_rgba"] = 0xff705030U;
    for (const bool missing_sample : {false, true}) {
        PixelBoundaryAudit dropped_edge;
        fill(dropped_edge, owned, missing_sample, false, "presentation.frame.edge");
        dropped_edge.consume(canvas);
        dropped_edge.consume(selected);
        CHECK_FALSE(dropped_edge.viewer_nonblack_complete());
        CHECK_FALSE(dropped_edge.raw_complete());
        CHECK(dropped_edge.samples.at({surface, 7U}).native.at({"00000000000000030000000000000004", 4U}).publication_fact.empty());
        auto contradictory_edge = native;
        contradictory_edge["event"] = "presentation.frame.edge";
        contradictory_edge["source_instance"] = 4U;
        dropped_edge.consume(contradictory_edge);
        CHECK_FALSE(dropped_edge.failure.empty());
        CHECK_FALSE(dropped_edge.viewer_nonblack_complete());
    }
    for (const auto* boundary : {"Allocation", "Reset", "Begin", "End"}) {
        const bool allocation = std::string_view{boundary} == "Allocation";
        std::string target{boundary};
        target.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(target.front())));
        auto audit = colored_viewer;
        auto failed = pixel("firefox.workspace.probe_failed", boundary);
        failed["presentation_revision"] = 6U;
        failed["transfer_sequence"] = 3U;
        if (allocation) failed["source"] = "00000000000000030000000000000005";
        audit.consume(failed);
        CHECK_FALSE(audit.probe_failure_complete(target));
        auto ordinary = failed;
        ordinary["event"] = "firefox.workspace.frame_forwarded";
        ordinary["pixel_probe"] = false;
        audit.consume(ordinary);
        CHECK(audit.probe_failure_complete(target));
        CHECK(audit.probe_failure_evidence(target).complete());
        auto reused_counter = audit;
        reused_counter.samples.try_emplace(PixelBoundaryAudit::Key{surface, 10U});
        CHECK(reused_counter.probe_failure_evidence(target).complete());
        auto unrelated = audit.samples.at({surface, 7U});
        unrelated.forwarded["workspace_source"] = "00000000000000030000000000000006";
        for (auto& receiver : unrelated.receivers)
            receiver.identity["transfer_sequence"] = scalar(failed, "transfer_sequence");
        reused_counter.samples.emplace(PixelBoundaryAudit::Key{surface, 8U}, std::move(unrelated));
        CHECK(reused_counter.probe_failure_evidence(target).complete());
        auto before_viewer = audit;
        before_viewer.successful_viewer.reset();
        CHECK(before_viewer.probe_failure_evidence(target).complete());
        CHECK_FALSE(before_viewer.probe_failure_complete(target));
        if (!allocation) {
            auto unrelated_content = audit;
            auto& publication = unrelated_content.samples.at({surface, 7U});
            const auto other_sequence = scalar(publication.receivers[0].identity, "content_sequence") + 1U;
            for (auto& [_, receipt_boundary] : publication.native) {
                receipt_boundary.identity["content_sequence"] = other_sequence;
                receipt_boundary.publication_fact["content_sequence"] = other_sequence;
            }
            for (auto& receipt_boundary : publication.receivers)
                receipt_boundary.identity["content_sequence"] = other_sequence;
            publication.forwarded["content_sequence"] = other_sequence;
            CHECK_FALSE(unrelated_content.probe_failure_complete(target));
            CHECK_FALSE(unrelated_content.probe_failure_evidence(target).complete());
            CHECK(audit.probe_failure_complete(target));
        }
        auto wrong_slot = audit;
        wrong_slot.probe_failures.front()["slot"] = 0U;
        if (!allocation) CHECK_FALSE(wrong_slot.probe_failure_complete(target));
        auto stale_receipt = audit;
        auto successful = failed;
        successful["event"] = "firefox.workspace.pixel";
        successful["boundary"] = "mailbox";
        successful["sample_index"] = 0U;
        successful["sample_x"] = 0U;
        successful["sample_y"] = 0U;
        stale_receipt.consume(successful);
        CHECK_FALSE(stale_receipt.probe_failure_complete(target));
        CHECK_FALSE(stale_receipt.probe_failure_evidence(target).forwarded);
        if (!allocation) {
            auto no_recovery = audit;
            no_recovery.samples.at({surface, 7U}).counted.fill(false);
            CHECK_FALSE(no_recovery.probe_failure_complete(target));
            CHECK(no_recovery.probe_failure_evidence(target).forwarded);
            CHECK_FALSE(no_recovery.probe_failure_evidence(target).recovered);
        }
    }
    for (const auto missing_owner : {"import", "mailbox", "iced.surface.pixel"}) {
        PixelBoundaryAudit audit;
        fill(audit, owned, false, false, {}, 8U);
        fill(audit, owned, false, false, missing_owner);
        audit.consume(canvas);
        audit.consume(selected);
        CHECK(audit.raw_complete());
        CHECK_FALSE(audit.viewer_nonblack_complete());
    }
    PixelBoundaryAudit wrong_transfer;
    fill(wrong_transfer, owned);
    auto reoffer = imported;
    reoffer["transfer_sequence"] = 5U;
    reoffer["timeline_ready"] = 9U;
    reoffer["timeline_release"] = 10U;
    reoffer["sample_index"] = 0U;
    reoffer["sample_x"] = 0U;
    reoffer["sample_y"] = 0U;
    wrong_transfer.consume(reoffer);
    wrong_transfer.consume(canvas);
    wrong_transfer.consume(selected);
    CHECK_FALSE(wrong_transfer.viewer_nonblack_complete());
}

TEST_CASE("composition evidence requires every card and kind in each column publication", "[workspace][audit][pixel]") {
    const auto fill = [](PixelBoundaryAudit& audit, std::string_view defect) {
        for (const auto columns : {4U, 10U}) {
            for (const auto card : {3U, 8U}) {
                for (unsigned kind = 0U; kind < 4U; ++kind) {
                    if (defect == "missing" && columns == 10U && card == 8U && kind == 3U) continue;
                    nlohmann::json record{{"event", "integration.atlas_composition"},
                                          {"surface", "surface"},
                                          {"columns", columns},
                                          {"presentation_revision", columns},
                                          {"card", card},
                                          {"kind", kind},
                                          {"content_width", 100U},
                                          {"content_height", 100U},
                                          {"image_x", 0.0},
                                          {"image_y", 0.0},
                                          {"image_width", 100.0},
                                          {"image_height", 100.0},
                                          {"sample_x", 10.5},
                                          {"sample_y", 10.5},
                                          {"canvas_x", 10.5},
                                          {"canvas_y", 10.5},
                                          {"expected", {120, 80, 40, 255}},
                                          {"observed", {120, 80, 40, 255}}};
                    if (defect == "mask-as-box" && kind == 2U) record["observed"] = {74, 80, 86, 255};
                    if (defect == "mixed" && kind == 3U) record["presentation_revision"] = 99U;
                    audit.consume(record);
                }
            }
            audit.consume({{"event", "integration.atlas_composition_complete"},
                           {"surface", "surface"},
                           {"columns", columns},
                           {"presentation_revision", columns},
                           {"cards", {3U, 8U}},
                           {"emitted", 8U},
                           {"content_width", 100U},
                           {"content_height", 100U}});
        }
    };
    PixelBoundaryAudit valid;
    fill(valid, "");
    CHECK(valid.composition_complete());
    for (const auto defect : {"missing", "mask-as-box", "mixed"}) {
        PixelBoundaryAudit audit;
        fill(audit, defect);
        CHECK_FALSE(audit.composition_complete());
    }
}

TEST_CASE("viewer continuity requires mapped routes foreground persistence and the same restored product", "[workspace][audit][pixel]") {
    const auto fill = [](PixelBoundaryAudit& audit, std::string_view defect, bool persistence) {
        const auto event = [&](const char* name, const char* control = "", const char* detail = "", unsigned a = 0U, unsigned b = 0U,
                               unsigned c = 0U, unsigned d = 0U) {
            audit.consume({{"event", name}, {"control", control}, {"detail", detail}, {"a", a}, {"b", b}, {"c", c}, {"d", d}});
        };
        for (unsigned kernel = 0U; kernel < 3U; ++kernel)
            event("integration.upscale_cached", "", "", 0U, 0U, kernel);
        event("integration.viewer_settings_preserved");
        event("integration.viewer_departure_started", "", "", 20U, persistence ? 1U : 0U, 30U);
        audit.consume({{"event", "upscale.stop.requested"}, {"observation_revision", 20U}});
        if (defect == "duplicate-stop") audit.consume({{"event", "upscale.stop.requested"}, {"observation_revision", 20U}});
        for (const bool train : {true, false}) {
            const auto* route = train ? "navigation.train" : "navigation.explore";
            if (defect != "direct") event("integration.navigation_message", route, train ? "Explore" : "Train");
            if (defect != "missing-outcome") event("integration.navigation_outcome", route, train ? "Train" : "Explore");
            if (persistence && defect != "missing-persistence") event("integration.route_state", route, "settings.reply", 1U);
            event("integration.viewer_route_confirmed", route,
                  defect == "foreground" ? "Annotation"
                  : train                ? "None"
                                         : "Upscale",
                  train ? 31U : 32U, persistence ? 1U : 0U, 1U);
            if (train) event("integration.viewer_abandoned", "", "", 21U);
        }
        if (defect != "missing-basic") event("integration.viewer_basic_reentry", "", "automatic-completed-draw", 41U, 50U, 3U, 40U);
        event("integration.viewer_reconnected", "", "matching-completed-draw", defect == "different-product" ? 42U : 41U, 51U, 3U, 40U);
        if (defect != "missing-gallery") event("integration.explore_reopened", "", "usable-after-reopen", 60U, 61U, 10U, 128U);
    };
    for (const bool persistence : {false, true}) {
        PixelBoundaryAudit valid;
        fill(valid, "", persistence);
        CHECK(valid.continuity_complete());
    }
    for (const auto defect : {"direct", "missing-outcome", "foreground", "missing-persistence", "duplicate-stop", "missing-basic",
                              "different-product", "missing-gallery"}) {
        PixelBoundaryAudit audit;
        fill(audit, defect, true);
        CHECK_FALSE(audit.continuity_complete());
    }
}

struct NativeAudit final {
    struct FinalCursorGenerations final {
        std::uint64_t material = 0U;
        std::uint64_t cursor = 0U;
    };

    struct ViewportAcceptance final {
        std::uint64_t endpoint = 0U;
        std::uint64_t generation = 0U;
        std::size_t ordinal = 0U;
    };
    bool server_started = false;
    bool peer_opened = false;
    bool shutdown_requested = false;
    bool firefox_terminal = false;
    bool shutdown_complete = false;
    bool shutdown_incomplete = false;
    bool explore_rendered = false;
    bool annotation_copied = false;
    bool annotation_opened = false;
    bool annotation_edited = false;
    bool presentation_ready = false;
    bool worker_failed = false;
    bool invalid_message = false;
    bool peer_replaced = false;
    // CLEANUP-IGNORE: Native audit flags and browser UI state are separate acceptance evidence records.
    bool interaction_rejected = false;
    bool peer_closed_after_shutdown = false;
    bool explore_placeholder = false;
    bool explore_partial_patch = false;
    bool explore_ready_batch = false;
    bool explore_stale_discard = false;
    bool explore_tile_regressed = false;
    bool explore_nproc_changed = false;
    bool explore_stale_patch = false;
    bool admission_seen = false;
    bool admission_priority_valid = true;
    std::set<std::pair<std::uint64_t, std::uint64_t>> admitted_reads;
    bool acceptance_first_patch_exact = false;
    bool acceptance_held_read = false;
    bool acceptance_held_completed = false;
    bool acceptance_held_released = false;
    bool acceptance_held_stale = false;
    bool causal_inconsistent = false;
    std::string_view causal_failure;
    // CLEANUP-IGNORE: Native process custody begins a distinct evidence group from CUDA render-layout records.
    pid_t firefox_pid = -1;
    // CLEANUP-IGNORE: Presentation and Explore runtime counters are acceptance evidence, not kernel ABI geometry.
    std::uint64_t presentation_timeline = 0U;
    std::uint64_t explore_generation = 0U;
    std::uint64_t explore_nproc = 0U;
    std::uint64_t partial_generation = 0U;
    std::uint64_t explore_stale_count = 0U;
    std::size_t explore_placeholder_ordinal = 0U;
    // CLEANUP-IGNORE: Partial-patch ordering counters are distinct from rendered-card geometry fields.
    std::size_t partial_placeholder_ordinal = 0U;
    std::size_t partial_first_patch_ordinal = 0U;
    std::uint64_t partial_first_tile_count = 0U;
    // CLEANUP-IGNORE: Acceptance scheduling and hold-custody fields are not the reflected diagnostic envelope schema.
    std::size_t explore_max_pinned = 0U;
    std::size_t ordinal = 0U;
    std::size_t peer_open_count = 0U;
    std::size_t peer_close_count = 0U;
    std::uint64_t held_generation = 0U;
    std::uint64_t held_slot = 0U;
    std::uint64_t held_compiled_index = 0U;
    std::uint64_t held_capacity = 0U;
    std::size_t held_ordinal = 0U;
    std::size_t held_release_ordinal = 0U;
    std::size_t held_discard_ordinal = 0U;
    std::map<std::uint64_t, std::size_t> accepted_generations;
    std::vector<ViewportAcceptance> accepted_viewports;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> placeholder_slots;
    std::map<std::uint64_t, std::size_t> placeholder_cardinalities;
    std::map<std::uint64_t, std::uint64_t> placeholder_digests;
    std::map<std::uint64_t, std::size_t> placeholder_ordinals;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> patched_slots;
    std::map<std::uint64_t, std::size_t> last_patch_ordinals;
    std::map<std::uint64_t, std::size_t> first_publication_ordinals;
    std::map<std::uint64_t, std::size_t> last_publication_ordinals;
    // CLEANUP-IGNORE: Tile, augmentation, and frame evidence have distinct identities from placeholder/patch inventories.
    std::map<std::uint64_t, std::uint64_t> first_published_tiles;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> first_patched_slots;
    std::map<std::uint64_t, std::uint64_t> published_tiles;
    std::map<std::uint64_t, std::uint64_t> augmentation_seeds;
    std::map<std::uint64_t, std::uint64_t> augmentation_pixel_seeds;
    using FramePublication = std::pair<std::size_t, std::uint64_t>;
    std::map<std::uint64_t, std::vector<FramePublication>> published_frames;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> presented_explore_frames;
    bool overlay_descriptors = false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> transformed_overlay_slots;
    std::set<std::pair<std::uint64_t, std::uint64_t>> semantic_overlay_slots;
    enum class PaddingOrientation : std::uint8_t {
        Vertical,
        Horizontal,
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> padded_card_slots;
    std::map<std::pair<std::uint64_t, std::uint64_t>, PaddingOrientation> padding_orientations;
    using OverlayDescriptorIdentity = std::pair<std::uint64_t, std::uint64_t>;
    std::map<std::pair<std::uint64_t, std::uint64_t>, OverlayDescriptorIdentity> selected_overlay_slots;
    std::map<std::pair<std::uint64_t, std::uint64_t>, OverlayDescriptorIdentity> hidden_overlay_slots;
    using ProbeKey = std::pair<std::uint64_t, std::uint64_t>;
    using ProbeSlots = std::map<ProbeKey, std::uint64_t>;
    using ProbeOrdinals = std::map<ProbeSlots::key_type, std::size_t>;
    using ProbeFrames = std::map<ProbeKey, std::vector<std::uint64_t>>;
    ProbeSlots rendered_probe_slots;
    ProbeSlots transition_probe_slots;
    ProbeOrdinals rendered_probe_ordinals;
    ProbeOrdinals transition_probe_ordinals;
    ProbeFrames rendered_probe_frames;
    bool donor_descriptors = false;

    void reject_causal_evidence(const std::string_view reason) noexcept {
        causal_inconsistent = true;
        if (causal_failure.empty()) causal_failure = reason;
    }

    void reconcile_incremental_publication(const std::uint64_t generation) {
        const auto cardinality = placeholder_cardinalities.find(generation);
        const auto placeholder = placeholder_slots.find(generation);
        const auto first_ordinal = first_publication_ordinals.find(generation);
        const auto last_ordinal = last_publication_ordinals.find(generation);
        const auto first_count = first_published_tiles.find(generation);
        const auto latest_count = published_tiles.find(generation);
        const auto first_slots = first_patched_slots.find(generation);
        const auto placeholder_ordinal = placeholder_ordinals.find(generation);
        if (cardinality == placeholder_cardinalities.end() || placeholder == placeholder_slots.end() ||
            placeholder->second.size() != cardinality->second || first_ordinal == first_publication_ordinals.end() ||
            last_ordinal == last_publication_ordinals.end() || first_count == first_published_tiles.end() ||
            latest_count == published_tiles.end() || first_slots == first_patched_slots.end() ||
            placeholder_ordinal == placeholder_ordinals.end())
            return;
        const bool exact_observed_slots = !first_slots->second.empty() && first_slots->second.size() <= first_count->second &&
                                          std::ranges::includes(placeholder->second, first_slots->second);
        if (partial_generation == 0U && first_count->second != 0U && first_count->second < cardinality->second && exact_observed_slots) {
            partial_generation = generation;
            partial_placeholder_ordinal = placeholder_ordinal->second;
            partial_first_patch_ordinal = first_ordinal->second;
            partial_first_tile_count = first_count->second;
            explore_partial_patch = true;
            acceptance_first_patch_exact = true;
        }
        if (generation == partial_generation && last_ordinal->second > partial_first_patch_ordinal &&
            latest_count->second > partial_first_tile_count)
            explore_ready_batch = true;
    }

    void reconcile_rendered_probes(const std::uint64_t generation) {
        const auto frames = published_frames.find(generation);
        const auto placeholder = placeholder_slots.find(generation);
        if (frames == published_frames.end() || frames->second.empty() || placeholder == placeholder_slots.end()) return;
        for (const auto& [key, compiled_index] : rendered_probe_slots) {
            if (key.first != generation) continue;
            const auto transition = transition_probe_slots.find(key);
            const auto slot = placeholder->second.find(key.second);
            if (transition == transition_probe_slots.end() || slot == placeholder->second.end()) continue;
            if (transition->second != compiled_index || slot->second != compiled_index) {
                reject_causal_evidence("rendered probe frame identity");
                continue;
            }
            const auto patched_generation = patched_slots.find(generation);
            if (patched_generation != patched_slots.end()) {
                const auto patched = patched_generation->second.find(key.second);
                if (patched != patched_generation->second.end() && patched->second != compiled_index) {
                    reject_causal_evidence("rendered probe patch identity");
                    continue;
                }
            }
            std::vector<std::uint64_t> frame_revisions;
            const auto rendered_ordinal = rendered_probe_ordinals.find(key);
            const auto transition_ordinal = transition_probe_ordinals.find(key);
            if (rendered_ordinal != rendered_probe_ordinals.end() && transition_ordinal != transition_probe_ordinals.end()) {
                const auto first_probe = std::min(rendered_ordinal->second, transition_ordinal->second);
                const auto last_probe = std::max(rendered_ordinal->second, transition_ordinal->second);
                for (const auto& frame : frames->second)
                    if (frame.first > last_probe) frame_revisions.push_back(frame.second);
                if (frame_revisions.empty()) {
                    for (auto frame = frames->second.rbegin(); frame != frames->second.rend(); ++frame)
                        if (frame->first < first_probe) {
                            frame_revisions.push_back(frame->second);
                            break;
                        }
                }
            } else if (frames->second.size() == 1U) {
                // Direct audit fixtures can provide already-correlated probes.
                frame_revisions.push_back(frames->second.front().second);
            }
            if (frame_revisions.empty()) continue;
            if (!rendered_probe_frames.contains(key) && rendered_probe_frames.size() == kAcceptanceRecordLimit) {
                reject_causal_evidence("rendered probe frame evidence capacity");
                continue;
            }
            rendered_probe_frames.insert_or_assign(key, std::move(frame_revisions));
        }
    }

    void record_frame(const std::uint64_t generation, const std::uint64_t revision, const std::size_t publication_ordinal) {
        if (revision == 0U) {
            reject_causal_evidence("rendered probe frame revision");
            return;
        }
        auto& frames = published_frames[generation];
        if (std::ranges::any_of(frames, [revision](const auto& frame) { return frame.second == revision; })) return;
        if (frames.size() == kAcceptanceRecordLimit) {
            reject_causal_evidence("Explore frame evidence capacity");
            return;
        }
        const auto position = std::ranges::lower_bound(frames, publication_ordinal, {}, &FramePublication::first);
        frames.emplace(position, publication_ordinal, revision);
        reconcile_rendered_probes(generation);
    }

    void record_tile_publication(const std::uint64_t generation, const std::uint64_t count, const std::size_t publication_ordinal) {
        const auto previous = published_tiles.find(generation);
        explore_tile_regressed = explore_tile_regressed || (previous != published_tiles.end() && count < previous->second);
        if (count == 0U || !placeholder_ordinals.contains(generation)) return;
        const auto [_, first] = first_publication_ordinals.try_emplace(generation, publication_ordinal);
        if (first) {
            first_published_tiles.emplace(generation, count);
            first_patched_slots.emplace(generation, patched_slots[generation]);
        }
        last_publication_ordinals.insert_or_assign(generation, publication_ordinal);
        published_tiles.insert_or_assign(generation, count);
        reconcile_incremental_publication(generation);
    }

    void join_gallery_publication(const std::uint64_t generation, const std::map<std::uint64_t, std::uint64_t>& complete_slots) {
        if (complete_slots.empty() || complete_slots.size() > kAcceptanceSlotLimit ||
            complete_slots.rbegin()->first != complete_slots.size() - 1U || generation == 0U)
            return;
        const auto cardinality = placeholder_cardinalities.find(generation);
        if (cardinality == placeholder_cardinalities.end()) return;
        if (complete_slots.size() != cardinality->second) {
            reject_causal_evidence("joined placeholder cardinality");
            return;
        }
        const auto observed = placeholder_slots.find(generation);
        if (observed == placeholder_slots.end()) return;
        const auto& received = observed->second;
        const bool conflict = std::ranges::any_of(received, [&complete_slots](const auto& slot) {
            const auto found = complete_slots.find(slot.first);
            return found == complete_slots.end() || found->second != slot.second;
        });
        if (conflict) {
            reject_causal_evidence("joined placeholder identity");
            return;
        }
        // The two writers are independent. Missing native slots must arrive
        // from the native cursor; a browser inventory never creates them.
        if (received.size() != complete_slots.size()) return;
        const auto patched = patched_slots.find(generation);
        if (patched != patched_slots.end())
            explore_stale_patch = explore_stale_patch || std::ranges::any_of(patched->second, [&received](const auto& slot) {
                                      const auto found = received.find(slot.first);
                                      return found == received.end() || found->second != slot.second;
                                  });
        // Frame and tile publication are recorded exclusively by their native
        // producers. Presentation and frontend snapshots retain separate facts.
        reconcile_incremental_publication(generation);
        reconcile_rendered_probes(generation);
    }

    void record_probe(ProbeSlots& probes, ProbeOrdinals& ordinals, const ProbeSlots::key_type& key,
                      const ProbeSlots::mapped_type compiled_index, const bool valid, const std::string_view failure) {
        if (probes.contains(key) && probes.at(key) != compiled_index) {
            reject_causal_evidence("rendered probe identity changed");
            return;
        }
        if (!valid) {
            reject_causal_evidence(failure);
            return;
        }
        if (!probes.contains(key) && probes.size() == kAcceptanceRecordLimit) {
            reject_causal_evidence("rendered probe evidence capacity");
            return;
        }
        probes.insert_or_assign(key, compiled_index);
        ordinals.try_emplace(key, ordinal);
        reconcile_rendered_probes(key.first);
    }

    void record_card_geometry(const std::uint64_t generation, const std::uint64_t slot, const std::uint64_t compiled_index,
                              const std::uint64_t card_width, const std::uint64_t card_height, const std::uint64_t content_x,
                              const std::uint64_t content_y, const std::uint64_t content_width, const std::uint64_t content_height) {
        const auto right_padding = card_width >= content_x && card_width - content_x >= content_width
                                       ? card_width - content_x - content_width
                                       : std::numeric_limits<std::uint64_t>::max();
        const auto bottom_padding = card_height >= content_y && card_height - content_y >= content_height
                                        ? card_height - content_y - content_height
                                        : std::numeric_limits<std::uint64_t>::max();
        const bool vertical_padding = content_x == 0U && content_width == card_width && content_y != 0U && bottom_padding != 0U &&
                                      std::max(content_y, bottom_padding) - std::min(content_y, bottom_padding) <= 1U;
        const bool horizontal_padding = content_y == 0U && content_height == card_height && content_x != 0U && right_padding != 0U &&
                                        std::max(content_x, right_padding) - std::min(content_x, right_padding) <= 1U;
        if (card_width == 0U || card_width != card_height || content_width == 0U || content_height == 0U ||
            vertical_padding == horizontal_padding)
            return;
        const auto key = std::pair{generation, slot};
        const auto orientation = vertical_padding ? PaddingOrientation::Vertical : PaddingOrientation::Horizontal;
        if ((padded_card_slots.contains(key) && padded_card_slots.at(key) != compiled_index) ||
            (padding_orientations.contains(key) && padding_orientations.at(key) != orientation))
            reject_causal_evidence("padded card identity changed");
        if (padded_card_slots.contains(key) || padded_card_slots.size() < kAcceptanceRecordLimit) {
            padded_card_slots.insert_or_assign(key, compiled_index);
            padding_orientations.insert_or_assign(key, orientation);
        } else {
            reject_causal_evidence("padded card evidence capacity");
        }
    }

    void consume_explore_evidence(const char* const event, const std::uint64_t value, const std::uint64_t detail = 0U,
                                  const std::uint64_t staging_bytes = 0U) {
        consume({{"kind", "gui_runtime"},
                 {"owner", "explore"},
                 {"event", event},
                 {"sequence", 2U},
                 {"value", value},
                 {"detail", detail},
                 {"staging_bytes", staging_bytes}});
    }

    void consume(const nlohmann::json& record) {
        if (record.value("kind", "") != "gui_runtime") return;
        ++ordinal;
        const std::string owner = record.value("owner", "");
        const std::string event = record.value("event", "");
        const std::uint64_t sequence = scalar(record, "sequence");
        const std::uint64_t value = scalar(record, "value");
        if (event == "gallery.read.scheduled") {
            admission_seen = true;
            const auto columns = scalar(record, "admission_columns");
            const auto first = scalar(record, "admission_first_row");
            const auto count = scalar(record, "admission_row_count");
            const auto position = scalar(record, "admission_position");
            const auto tier = scalar(record, "admission_tier");
            const bool forward = record.value("admission_forward", true);
            const auto row = columns == 0U ? 0U : position / columns;
            const bool visible = row >= first && row - first < count;
            const bool after = row >= first && row - first >= count;
            const auto expected = visible ? 0U : (after == forward ? 1U : 2U);
            const auto preferred = scalar(record, forward ? "admission_forward_eligible" : "admission_backward_eligible");
            admission_priority_valid =
                admission_priority_valid && columns != 0U && count != 0U && tier == expected &&
                (visible || (after ? row - first - count < 4U : first - row <= 4U)) &&
                (tier == 0U || (record.contains("admission_immediate_eligible") && scalar(record, "admission_immediate_eligible") == 0U)) &&
                (tier != 2U || preferred == 0U);
            if (admitted_reads.size() >= kAcceptanceRecordLimit)
                admission_priority_valid = false;
            else
                admitted_reads.emplace(sequence, scalar(record, "detail"));
        }
        server_started = server_started || event == "browser.server.started";
        peer_opened = peer_opened || event == "browser.server.peer_opened";
        if (event == "browser.server.peer_opened") ++peer_open_count;
        shutdown_requested = shutdown_requested || event == "shutdown.requested";
        firefox_terminal = firefox_terminal || event == "shutdown.firefox_terminal";
        shutdown_complete = shutdown_complete || event == "shutdown.complete";
        shutdown_incomplete = shutdown_incomplete || event == "shutdown.incomplete";
        worker_failed = worker_failed || event == "worker.failure";
        invalid_message = invalid_message || event == "browser.server.invalid_message";
        peer_replaced = peer_replaced || event == "browser.server.peer_replaced";
        if (event == "browser.server.peer_closed") {
            peer_closed_after_shutdown = peer_closed_after_shutdown || shutdown_requested;
            ++peer_close_count;
        }
        interaction_rejected = interaction_rejected || event == "browser.interaction.rejected";
        if (event == "browser.interaction.accepted" && record.value("participant", "") == "UpdateViewport") {
            const bool new_generation = !accepted_generations.contains(value);
            const bool valid = sequence == kUpdateViewportEndpoint && value != 0U &&
                               (!new_generation || accepted_viewports.size() < kAcceptanceGenerationLimit) &&
                               (accepted_viewports.empty() || accepted_viewports.back().generation <= value);
            if (!valid) reject_causal_evidence("viewport acceptance order");
            if (valid && new_generation) {
                accepted_generations.emplace(value, ordinal);
                accepted_viewports.push_back({.endpoint = sequence, .generation = value, .ordinal = ordinal});
            }
        }
        explore_rendered = explore_rendered || (owner == "explore" && event == "render.completed");
        if (owner == "explore" && event == "render.completed" && value != 0U) {
            explore_nproc_changed = explore_nproc_changed || (explore_nproc != 0U && explore_nproc != value);
            explore_nproc = explore_nproc == 0U ? value : explore_nproc;
        }
        if (owner == "explore" && event == "placeholder.published") {
            explore_placeholder = sequence != 0U;
            explore_generation = sequence;
            explore_placeholder_ordinal = ordinal;
            placeholder_ordinals.try_emplace(sequence, ordinal);
            explore_max_pinned = std::max(explore_max_pinned, static_cast<std::size_t>(scalar(record, "staging_bytes")));
        }
        if (owner == "explore" && event == "acceptance.placeholder.slot") {
            if (!placeholder_slots.contains(sequence) && placeholder_slots.size() == kAcceptanceGenerationLimit) {
                reject_causal_evidence("placeholder generation capacity");
                return;
            }
            auto& slots = placeholder_slots[sequence];
            const auto compiled_index = scalar(record, "detail");
            if (value >= kAcceptanceSlotLimit || (slots.contains(value) && slots.at(value) != compiled_index))
                reject_causal_evidence("placeholder slot identity");
            if (value < kAcceptanceSlotLimit) slots.insert_or_assign(value, compiled_index);
        }
        if (owner == "explore" && event == "acceptance.placeholder.complete") {
            const auto cardinality = static_cast<std::size_t>(value);
            const auto digest = scalar(record, "detail");
            if (!placeholder_slots.contains(sequence) && placeholder_slots.size() == kAcceptanceGenerationLimit) {
                reject_causal_evidence("placeholder generation capacity");
                return;
            }
            const auto& slots = placeholder_slots[sequence];
            const bool invalid_slot = std::ranges::any_of(slots, [cardinality](const auto& slot) { return slot.first >= cardinality; });
            if (cardinality > kAcceptanceSlotLimit || slots.size() > cardinality || invalid_slot ||
                (placeholder_cardinalities.contains(sequence) && placeholder_cardinalities.at(sequence) != cardinality) ||
                (placeholder_digests.contains(sequence) && placeholder_digests.at(sequence) != digest))
                reject_causal_evidence("placeholder cardinality");
            if (cardinality <= kAcceptanceSlotLimit) {
                placeholder_cardinalities.insert_or_assign(sequence, cardinality);
                placeholder_digests.insert_or_assign(sequence, digest);
                // Gallery emits its inventory even when retained pixels make
                // Explore's PlaceholderPublished/DiagnoseFrame conditional.
                // This is the inventory's own entered boundary, not a
                // reconstruction of either omitted product event.
                if (sequence >= explore_generation) {
                    explore_placeholder = sequence != 0U;
                    explore_generation = sequence;
                    explore_placeholder_ordinal = ordinal;
                    placeholder_ordinals.try_emplace(sequence, ordinal);
                }
                reconcile_incremental_publication(sequence);
            }
        }
        if (owner == "explore" && event == "acceptance.slot.patched") {
            const auto compiled_index = scalar(record, "detail");
            if (!patched_slots.contains(sequence) && patched_slots.size() == kAcceptanceGenerationLimit) {
                reject_causal_evidence("patched generation capacity");
                return;
            }
            auto& slots = patched_slots[sequence];
            if (value >= kAcceptanceSlotLimit || (slots.contains(value) && slots.at(value) != compiled_index))
                reject_causal_evidence("patched slot identity");
            if (value < kAcceptanceSlotLimit) slots.insert_or_assign(value, compiled_index);
            last_patch_ordinals.insert_or_assign(sequence, ordinal);
            const auto placeholder = placeholder_slots[sequence].find(value);
            explore_stale_patch =
                explore_stale_patch || (placeholder != placeholder_slots[sequence].end() && placeholder->second != compiled_index);
            reconcile_rendered_probes(sequence);
        }
        if (owner == "explore" && event == "acceptance.completion.held") {
            const auto compiled_index = scalar(record, "detail");
            const auto capacity = scalar(record, "staging_bytes");
            if (acceptance_held_read &&
                (held_generation != sequence || held_slot != value || held_compiled_index != compiled_index || held_capacity != capacity))
                reject_causal_evidence("held read identity changed");
            acceptance_held_read = true;
            acceptance_held_completed = true;
            held_generation = sequence;
            held_slot = value;
            held_compiled_index = compiled_index;
            held_capacity = capacity;
            held_ordinal = ordinal;
        }
        if (owner == "explore" && event == "acceptance.completion.released" && sequence == held_generation && value == held_slot &&
            scalar(record, "detail") == held_compiled_index) {
            acceptance_held_released = true;
            held_release_ordinal = ordinal;
        }
        if (owner == "explore" && event == "tile.batch.published") {
            record_tile_publication(sequence, value, ordinal);
            explore_max_pinned = std::max(explore_max_pinned, static_cast<std::size_t>(scalar(record, "staging_bytes")));
        }
        explore_stale_discard =
            explore_stale_discard || (owner == "explore" && event == "thumbnail.stale.discarded" && sequence != 0U && value != 0U);
        if (owner == "explore" && event == "acceptance.stale.read.discarded" && sequence == held_generation && value == held_slot &&
            scalar(record, "detail") == held_compiled_index && scalar(record, "staging_bytes") == held_capacity &&
            held_discard_ordinal == 0U) {
            held_discard_ordinal = ordinal;
            acceptance_held_stale = true;
        }
        if (owner == "explore" && event == "thumbnail.stale.discarded") explore_stale_count += value;
        if (owner == "explore" && event == "explore.augmentation.batch.prepared") {
            const bool usable_batch = value != 0U && scalar(record, "capacity_width") != 0U && scalar(record, "capacity_height") != 0U;
            if (usable_batch && (augmentation_seeds.contains(sequence) || augmentation_seeds.size() < kAcceptanceGenerationLimit))
                augmentation_seeds.insert_or_assign(sequence, scalar(record, "detail"));
            else if (usable_batch)
                reject_causal_evidence("augmentation seed capacity");
        }
        if (owner == "explore" && event == "explore.image.pixel_checksum" && (scalar(record, "staging_bytes") & 7U) == 7U) {
            const auto seed = scalar(record, "capacity_width");
            const bool has_pixels = scalar(record, "detail") != 0U;
            if (has_pixels && (seed == 0U || seed == 1U)) {
                if (augmentation_pixel_seeds.contains(sequence) && augmentation_pixel_seeds.at(sequence) != seed)
                    reject_causal_evidence("augmentation pixel seed changed");
                augmentation_pixel_seeds.insert_or_assign(sequence, seed);
            }
        }
        overlay_descriptors = overlay_descriptors || (owner == "explore" && event == "explore.overlay.descriptors.prepared" &&
                                                      value != 0U && scalar(record, "detail") != 0U);
        if (owner == "explore" && event == "explore.overlay.transformed_bounds") {
            const auto detail = scalar(record, "detail");
            const auto minimum_x = scalar(record, "capacity_width");
            const auto minimum_y = scalar(record, "capacity_height");
            const auto maximum_x = scalar(record, "staging_bytes") >> 32U;
            const auto maximum_y = scalar(record, "staging_bytes") & 0xffffffffU;
            if ((detail >> 32U) != 0U && (detail & 0xffffffffU) != 0U && maximum_x > minimum_x && maximum_y > minimum_y &&
                maximum_x <= 65'535U && maximum_y <= 65'535U) {
                const auto key = std::pair{sequence, value};
                if (transformed_overlay_slots.contains(key) || transformed_overlay_slots.size() < kAcceptanceRecordLimit)
                    transformed_overlay_slots.insert(key);
                else
                    reject_causal_evidence("transformed overlay evidence capacity");
            }
        }
        if (owner == "explore" && event == "explore.semantic.nonzero_pixels" && scalar(record, "detail") != 0U &&
            scalar(record, "capacity_width") != 0U && scalar(record, "capacity_height") != 0U) {
            const auto key = std::pair{sequence, value};
            if (semantic_overlay_slots.contains(key) || semantic_overlay_slots.size() < kAcceptanceRecordLimit)
                semantic_overlay_slots.insert(key);
            else
                reject_causal_evidence("semantic overlay evidence capacity");
        }
        if (owner == "explore" && event == "explore.card.geometry_probe") {
            const auto packed = scalar(record, "staging_bytes");
            record_card_geometry(sequence, value, scalar(record, "detail"), scalar(record, "capacity_width"),
                                 scalar(record, "capacity_height"), (packed >> 48U) & 0xffffU, (packed >> 32U) & 0xffffU,
                                 (packed >> 16U) & 0xffffU, packed & 0xffffU);
        }
        if (owner == "explore" && event == "explore.overlay.selection_probe") {
            const auto packed = scalar(record, "staging_bytes");
            const auto slot = packed >> 32U;
            const auto selected_class = (packed >> 16U) & 0xffffU;
            const auto hidden_class = packed & 0xffffU;
            const auto key = std::pair{sequence, slot};
            const auto compiled_index = scalar(record, "detail");
            if (value != 0U && scalar(record, "capacity_height") != 0U && selected_class != 0U) {
                if (selected_overlay_slots.contains(key) || selected_overlay_slots.size() < kAcceptanceRecordLimit)
                    selected_overlay_slots.insert_or_assign(key, OverlayDescriptorIdentity{selected_class, compiled_index});
                else
                    reject_causal_evidence("selected overlay evidence capacity");
            }
            if (scalar(record, "capacity_width") != 0U && hidden_class != 0U) {
                if (hidden_overlay_slots.contains(key) || hidden_overlay_slots.size() < kAcceptanceRecordLimit)
                    hidden_overlay_slots.insert_or_assign(key, OverlayDescriptorIdentity{hidden_class, compiled_index});
                else
                    reject_causal_evidence("hidden overlay evidence capacity");
            }
        }
        if (owner == "explore" && event == "explore.card.rendered_probe") {
            const auto key = std::pair{sequence, value};
            const auto packed = scalar(record, "staging_bytes");
            const auto padding_pixels = packed >> 32U;
            const auto compiled_index = scalar(record, "detail");
            record_card_geometry(sequence, value, compiled_index, scalar(record, "source_width"), scalar(record, "source_height"),
                                 scalar(record, "content_x"), scalar(record, "content_y"), scalar(record, "content_width"),
                                 scalar(record, "content_height"));
            const bool complete_probe = scalar(record, "capacity_width") == 31U && scalar(record, "capacity_height") != 0U &&
                                        padded_card_slots.contains(key) && padded_card_slots.at(key) == compiled_index &&
                                        padding_pixels != 0U && ((packed >> 16U) & 0xffffU) != 0U && (packed & 0xffffU) != 0U;
            record_probe(rendered_probe_slots, rendered_probe_ordinals, key, compiled_index, complete_probe,
                         "rendered card probe incomplete");
        }
        if (owner == "explore" && event == "explore.card.transition_probe") {
            const auto key = std::pair{sequence, value};
            const auto compiled_index = scalar(record, "detail");
            const auto expected = scalar(record, "staging_bytes");
            const bool complete_transition = expected != 0U && scalar(record, "capacity_width") >= expected / 2U &&
                                             scalar(record, "capacity_width") <= expected && scalar(record, "capacity_height") == expected;
            record_probe(transition_probe_slots, transition_probe_ordinals, key, compiled_index, complete_transition,
                         "rendered transition probe incomplete");
        }
        if (owner == "explore" && event == "explore.frame.published") { record_frame(sequence, value, ordinal); }
        donor_descriptors = donor_descriptors || (owner == "explore" && event == "explore.donor.descriptors.prepared" && value != 0U &&
                                                  scalar(record, "detail") != 0U && scalar(record, "capacity_width") > value &&
                                                  scalar(record, "capacity_height") > scalar(record, "detail"));
        const bool document_opened = owner == "annotation" && event == "document.opened";
        annotation_copied = annotation_copied || (owner == "annotation" && event == "copy.completed");
        annotation_opened = annotation_opened || document_opened;
        annotation_edited = annotation_edited || (owner == "annotation" && event == "document.edited");
        if (owner == "presentation" && event == "timeline.ready" && value != 0U) {
            presentation_ready = true;
            presentation_timeline = std::max(presentation_timeline, value);
        }
        if (owner == "presentation" && event == "presentation.frame.edge" &&
            scalar(record, "source_session") ==
                mmltk::controller::presentation_source_session(mmltk::controller::PresentationSourceKind::Explore) &&
            scalar(record, "source_instance") != 0U && scalar(record, "source_observation_revision") != 0U &&
            scalar(record, "source_revision") != 0U && scalar(record, "frame_revision") == scalar(record, "source_revision")) {
            const auto frame = std::pair{scalar(record, "source_observation_revision"), scalar(record, "source_revision")};
            if (presented_explore_frames.contains(frame) || presented_explore_frames.size() < kAcceptanceRecordLimit)
                presented_explore_frames.try_emplace(frame, ordinal);
            else
                reject_causal_evidence("presented Explore frame evidence capacity");
        }
        if (owner == "firefox_process" && event == "child.spawned" &&
            sequence <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
            firefox_pid = static_cast<pid_t>(sequence);
    }

    [[nodiscard]] std::optional<FinalCursorGenerations> final_generations_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots,
                                                                              const std::uint64_t generation,
                                                                              const std::uint64_t frame_revision) const noexcept {
        if (rendered_slots.empty() || generation == 0U || frame_revision == 0U) return std::nullopt;
        const auto placeholder = placeholder_slots.find(generation);
        if (placeholder == placeholder_slots.end() || !placeholder_cardinalities.contains(generation) ||
            placeholder_cardinalities.at(generation) != placeholder->second.size() || placeholder->second != rendered_slots)
            return std::nullopt;
        const auto native_frame = published_frames.find(generation);
        if (native_frame == published_frames.end() || std::ranges::none_of(native_frame->second, [frame_revision](const auto& publication) {
                return publication.second == frame_revision;
            }))
            return std::nullopt;
        return FinalCursorGenerations{.material = generation, .cursor = generation};
    }

    [[nodiscard]] std::optional<std::uint64_t> generation_for(const std::map<std::uint64_t, std::uint64_t>& rendered_slots) const noexcept {
        for (auto accepted = accepted_viewports.rbegin(); accepted != accepted_viewports.rend(); ++accepted) {
            const auto placeholder = placeholder_slots.find(accepted->generation);
            if (placeholder != placeholder_slots.end() && placeholder_cardinalities.contains(accepted->generation) &&
                placeholder_cardinalities.at(accepted->generation) == placeholder->second.size() && placeholder->second == rendered_slots)
                return accepted->generation;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool held_stale_read_discarded() const noexcept { return acceptance_held_stale; }

    [[nodiscard]] bool stale_thumbnail_discarded() const noexcept { return explore_stale_discard || held_stale_read_discarded(); }

    [[nodiscard]] bool superseding_placeholder_observed() const noexcept {
        return acceptance_held_read && std::ranges::any_of(placeholder_slots, [this](const auto& placeholder) {
                   return placeholder.first > held_generation && placeholder_cardinalities.contains(placeholder.first) &&
                          placeholder_cardinalities.at(placeholder.first) == placeholder.second.size();
               });
    }

    void RecordHeldControlObservation(const ExploreAcceptanceGate::ControlObservation observation) noexcept {
        using Event = ExploreAcceptanceGate::ControlEvent;
        if (observation.event == Event::InitialWait) return;
        const bool valid_identity = observation.generation != 0U && observation.slot < kAcceptanceSlotLimit &&
                                    observation.compiled_index <= std::numeric_limits<std::uint32_t>::max() &&
                                    observation.staging_bytes != 0U;
        if (!valid_identity) {
            reject_causal_evidence("held control identity");
            return;
        }
        if (acceptance_held_read && (held_generation != observation.generation || held_slot != observation.slot ||
                                     held_compiled_index != observation.compiled_index || held_capacity != observation.staging_bytes)) {
            reject_causal_evidence("held control identity changed");
            return;
        }
        acceptance_held_read = true;
        acceptance_held_completed = true;
        held_generation = observation.generation;
        held_slot = observation.slot;
        held_compiled_index = observation.compiled_index;
        held_capacity = observation.staging_bytes;
        if (observation.event == Event::HeldProceed)
            acceptance_held_released = true;
        else if (observation.event == Event::HeldStale)
            acceptance_held_stale = true;
        else if (observation.event != Event::HeldWait)
            reject_causal_evidence("held control event");
    }

    [[nodiscard]] std::string_view causal_stale_blocker(const FinalCursorGenerations final) const noexcept {
        if (!acceptance_held_read) return "held read";
        if (!acceptance_held_completed) return "held read completion";
        if (!held_stale_read_discarded()) return "released held read discard";
        if (final.material <= held_generation || final.cursor <= held_generation) return "final generation order";
        if (!placeholder_slots.contains(final.material)) return "final material placeholder";
        if (!placeholder_cardinalities.contains(final.material) ||
            placeholder_cardinalities.at(final.material) != placeholder_slots.at(final.material).size())
            return "final material placeholder completeness";
        if (!superseding_placeholder_observed()) return "superseding placeholder";
        return {};
    }

    [[nodiscard]] bool causal_stale_chain(const FinalCursorGenerations final) const noexcept { return causal_stale_blocker(final).empty(); }

    [[nodiscard]] bool exact_partial_slot_identity() const noexcept {
        const auto placeholder = placeholder_slots.find(partial_generation);
        const auto patched = patched_slots.find(partial_generation);
        // A newer viewport may supersede unfinished slots or an unpublished batch.
        return placeholder != placeholder_slots.end() && patched != patched_slots.end() && !patched->second.empty() &&
               std::ranges::includes(placeholder->second, patched->second);
    }

    [[nodiscard]] std::optional<std::uint64_t> augmentation_generation_for(const std::uint64_t seed,
                                                                           const std::uint64_t frame_revision) const noexcept {
        if (frame_revision == 0U) return std::nullopt;
        const auto generation = std::ranges::find_if(augmentation_seeds, [this, seed, frame_revision](const auto& candidate) {
            const auto frames = published_frames.find(candidate.first);
            return candidate.second == seed && frames != published_frames.end() &&
                   std::ranges::any_of(frames->second, [frame_revision](const auto& frame) { return frame.second == frame_revision; });
        });
        return generation == augmentation_seeds.end() ? std::nullopt : std::optional{generation->first};
    }

    [[nodiscard]] bool augmentation_pixels_observed(const std::uint64_t seed) const noexcept {
        return std::ranges::any_of(augmentation_seeds, [this, seed](const auto& generation) {
            const auto pixels = augmentation_pixel_seeds.find(generation.first);
            const auto frames = published_frames.find(generation.first);
            return generation.second == seed && pixels != augmentation_pixel_seeds.end() && pixels->second == seed &&
                   frames != published_frames.end() && !frames->second.empty();
        });
    }

    [[nodiscard]] bool aligned_rendered_probe(const std::pair<std::uint64_t, std::uint64_t> key) const noexcept {
        const auto rendered = rendered_probe_slots.find(key);
        const auto selected = selected_overlay_slots.find(key);
        const auto placeholder = placeholder_slots.find(key.first);
        return rendered != rendered_probe_slots.end() && padded_card_slots.contains(key) && padding_orientations.contains(key) &&
               padded_card_slots.at(key) == rendered->second && transition_probe_slots.contains(key) &&
               transition_probe_slots.at(key) == rendered->second && rendered_probe_frames.contains(key) &&
               std::ranges::any_of(rendered_probe_frames.at(key), [](const auto revision) { return revision != 0U; }) &&
               placeholder != placeholder_slots.end() && placeholder->second.contains(key.second) &&
               placeholder->second.at(key.second) == rendered->second && selected != selected_overlay_slots.end() &&
               selected->second.second == rendered->second && transformed_overlay_slots.contains(key) &&
               semantic_overlay_slots.contains(key);
    }

    [[nodiscard]] bool filtered_overlay_identity() const noexcept {
        return std::ranges::any_of(selected_overlay_slots, [this](const auto& selected) {
            return std::ranges::any_of(hidden_overlay_slots, [&selected](const auto& hidden) {
                return hidden.first.first != selected.first.first && hidden.second.first == selected.second.first;
            });
        });
    }

    [[nodiscard]] bool aligned_padding_orientation(const PaddingOrientation orientation) const noexcept {
        return std::ranges::any_of(rendered_probe_slots, [this, orientation](const auto& probe) {
            return padding_orientations.contains(probe.first) && padding_orientations.at(probe.first) == orientation &&
                   aligned_rendered_probe(probe.first);
        });
    }

    [[nodiscard]] bool aligned_overlay_pixels() const noexcept {
        return filtered_overlay_identity() && aligned_padding_orientation(PaddingOrientation::Vertical) &&
               aligned_padding_orientation(PaddingOrientation::Horizontal);
    }

    [[nodiscard]] std::string_view overlay_readiness_blocker(const bool require_pixel_probes) const noexcept {
        if (!overlay_descriptors) return "overlay descriptors";
        if (!donor_descriptors) return "donor descriptors";
        if (!require_pixel_probes) return {};
        if (!filtered_overlay_identity()) return "filtered overlay identity";
        if (!aligned_padding_orientation(PaddingOrientation::Vertical)) return "vertical rendered overlay";
        if (!aligned_padding_orientation(PaddingOrientation::Horizontal)) return "horizontal rendered overlay";
        return {};
    }

    [[nodiscard]] std::string_view readiness_blocker(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                                     const bool require_overlay_pixel_probes) const noexcept {
        const auto overlay_blocker = overlay_readiness_blocker(require_overlay_pixel_probes);
        const std::array checks{
            std::pair{server_started && peer_opened && firefox_pid > 0 && active_peer(), std::string_view{"browser peer"}},
            std::pair{explore_rendered && explore_placeholder, std::string_view{"Explore publication"}},
            std::pair{annotation_copied && annotation_opened && annotation_edited, std::string_view{"Annotation lifecycle"}},
            std::pair{presentation_ready, std::string_view{"Presentation timeline"}},
            std::pair{explore_partial_patch && acceptance_first_patch_exact && explore_ready_batch,
                      std::string_view{"incremental Explore publication"}},
            std::pair{stale_thumbnail_discarded(), std::string_view{"stale thumbnail discard"}},
            std::pair{causal_stale_chain(final),
                      causal_stale_blocker(final).empty() ? std::string_view{"causal stale-read chain"} : causal_stale_blocker(final)},
            std::pair{exact_partial_slot_identity(), std::string_view{"partial-slot identity"}},
            std::pair{overlay_blocker.empty(), overlay_blocker},
            std::pair{seeded_augmentation_ready, std::string_view{"seeded augmentation pixel publication"}},
            std::pair{!worker_failed && !invalid_message && !peer_replaced && !interaction_rejected, std::string_view{"runtime validity"}},
            std::pair{!explore_tile_regressed && !explore_nproc_changed && !explore_stale_patch && !causal_inconsistent,
                      std::string_view{"Explore causal validity"}},
        };
        const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return !check.first; });
        return blocker == checks.end() ? std::string_view{} : blocker->second;
    }

    [[nodiscard]] bool product_completed(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                         const bool require_overlay_pixel_probes) const noexcept {
        return server_started && peer_opened && firefox_pid > 0 && explore_rendered && annotation_copied && annotation_opened &&
               annotation_edited && presentation_ready && explore_placeholder && explore_partial_patch && acceptance_first_patch_exact &&
               explore_ready_batch && stale_thumbnail_discarded() && causal_stale_chain(final) && exact_partial_slot_identity() &&
               overlay_descriptors && donor_descriptors && (!require_overlay_pixel_probes || aligned_overlay_pixels()) &&
               seeded_augmentation_ready && !worker_failed && !explore_tile_regressed && !explore_nproc_changed && !explore_stale_patch &&
               !invalid_message && !peer_replaced && !interaction_rejected && !causal_inconsistent;
    }

    [[nodiscard]] bool product_ready(const FinalCursorGenerations final, const bool seeded_augmentation_ready,
                                     const bool require_overlay_pixel_probes) const noexcept {
        return product_completed(final, seeded_augmentation_ready, require_overlay_pixel_probes) && active_peer();
    }

    [[nodiscard]] bool failed_before_termination() const noexcept {
        return worker_failed || invalid_message || peer_replaced || interaction_rejected || causal_inconsistent;
    }

    [[nodiscard]] std::string_view failure_blocker() const noexcept {
        const std::array checks{
            std::pair{worker_failed, std::string_view{"worker failure"}},
            std::pair{invalid_message, std::string_view{"invalid browser message"}},
            std::pair{peer_replaced, std::string_view{"browser peer replacement"}},
            std::pair{interaction_rejected, std::string_view{"browser interaction rejected"}},
            std::pair{causal_inconsistent, causal_failure.empty() ? std::string_view{"causal evidence inconsistency"} : causal_failure},
        };
        const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return check.first; });
        return blocker == checks.end() ? std::string_view{} : blocker->second;
    }

    [[nodiscard]] bool active_peer() const noexcept { return peer_open_count == peer_close_count + 1U; }
};

struct AtlasDrawAudit final {
    using SourceKey = std::array<std::uint64_t, 4U>;
    using SampleKey = std::tuple<std::string, std::uint64_t, std::uint64_t>;
    using AllocationKey = std::tuple<std::string, std::uint64_t, std::uint64_t, std::uint64_t>;
    static constexpr std::initializer_list<const char*> source_fields{
        "content_session", "source_kind",    "source_instance",  "source_revision",
        "content_width",   "content_height", "dataset_identity",
        "columns",         "rows",           "first_row",        "matching_count",
        "visible_indices", "row_capacity",   "row_origin",       "card_extent"};
    static constexpr std::initializer_list<const char*> image_fields{
        "surface",         "generation",  "width",           "height",          "presentation_revision", "frame_revision",
        "content_session", "source_kind", "source_instance", "source_revision", "content_width",         "content_height",
        "columns",         "rows",        "first_row",       "matching_count",  "visible_indices",       "row_capacity",
        "row_origin",      "card_extent"};
    std::map<SourceKey, nlohmann::json> sources;
    std::map<std::pair<std::uint64_t, std::uint64_t>, SourceKey> sessions;
    std::map<SampleKey, nlohmann::json> acquisitions;
    std::set<std::uint64_t> drawn_rows;
    std::optional<AllocationKey> staged_allocation;
    std::uint64_t previous_staged_rows = 0U;
    unsigned staged_transitions = 0U;
    std::optional<nlohmann::json> last_draw;
    std::set<std::string> stages;
    bool grid_round_trip = false;
    std::optional<nlohmann::json> overlap_draw;
    std::optional<nlohmann::json> return_baseline;
    unsigned return_stage = 0U;
    bool return_round_trip = false;
    bool away_return = false;
    std::map<SampleKey, std::set<std::uint64_t>> ready_cell_samples;
    std::map<std::uint64_t, bool> retained_ready;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::uint64_t> retained_cell_pixels;
    std::optional<nlohmann::json> pixel_meaning;
    std::map<std::string, nlohmann::json> held_stages;
    bool seen = false;
    bool valid = true;

    [[nodiscard]] static SourceKey source_key(const nlohmann::json& record) {
        return {scalar(record, "content_session"), scalar(record, "source_kind"), scalar(record, "source_instance"),
                scalar(record, "source_revision")};
    }

    [[nodiscard]] static SampleKey sample_key(const nlohmann::json& record) {
        return {record.value("surface", ""), scalar(record, "generation"), scalar(record, "presentation_revision")};
    }

    [[nodiscard]] static AllocationKey allocation_key(const nlohmann::json& record) {
        return {record.value("surface", ""), scalar(record, "generation"), scalar(record, "width"), scalar(record, "height")};
    }

    void observe_staged_rows(const AllocationKey& allocation, const std::uint64_t rows) {
        if (!staged_allocation) {
            staged_allocation = allocation;
        } else if (*staged_allocation != allocation) {
            return;
        } else {
            if (rows == previous_staged_rows + 1U) staged_transitions |= 1U;
            if (previous_staged_rows == rows + 1U) staged_transitions |= 2U;
        }
        grid_round_trip = staged_transitions == 3U;
        previous_staged_rows = rows;
    }

    [[nodiscard]] static bool same_fields(const nlohmann::json& left, const nlohmann::json& right,
                                          const std::initializer_list<const char*> fields) {
        return std::ranges::all_of(
            fields, [&](const auto* field) { return left.contains(field) && right.contains(field) && left[field] == right[field]; });
    }

    [[nodiscard]] bool ready_pixels_complete(const nlohmann::json& record) const {
        const auto samples = ready_cell_samples.find(sample_key(record));
        if (samples == ready_cell_samples.end() || !visible(record) || !record.contains("ready_slots") ||
            !record["ready_slots"].is_array() || !record.contains("visible_indices") || !record["visible_indices"].is_array())
            return false;
        const auto columns = scalar(record, "columns");
        if (columns == 0U) return false;
        const double side = record["image"][2].get<double>() / static_cast<double>(columns);
        for (std::size_t slot = 0U; slot < record["visible_indices"].size(); ++slot) {
            if (slot >= record["ready_slots"].size() || record["ready_slots"][slot] != true) continue;
            const double left = record["image"][0].get<double>() + (static_cast<double>(slot % columns) + .2) * side;
            const double top = record["image"][1].get<double>() + (static_cast<double>(slot / columns) + .2) * side;
            const double width = std::min(left + .6 * side, record["clip"][0].get<double>() + record["clip"][2].get<double>()) -
                                 std::max(left, record["clip"][0].get<double>());
            const double height = std::min(top + .6 * side, record["clip"][1].get<double>() + record["clip"][3].get<double>()) -
                                  std::max(top, record["clip"][1].get<double>());
            if (width >= 2.0 && height >= 2.0 && !samples->second.contains(record["visible_indices"][slot].get<std::uint64_t>()))
                return false;
        }
        return true;
    }

    void stage(const nlohmann::json& record) {
        constexpr std::array names{"fractional", "row1", "row2", "row10", "end", "restored"};
        const auto name = record.value("control", "");
        if (name == "away-return") {
            const bool matching = !away_return && last_draw && same_fields(record, *last_draw, source_fields) &&
                                  sample_key(record) == sample_key(*last_draw) && ready_pixels_complete(record);
            valid = valid && matching;
            away_return = matching;
            last_draw.reset();
            return;
        }
        if (name.starts_with("held-")) {
            constexpr std::array held_names{"held-visible", "held-return", "held-aligned", "held-extra", "held-restored", "held-complete"};
            bool matching = held_stages.size() < held_names.size() && name == held_names[held_stages.size()] && last_draw &&
                            same_fields(record, *last_draw, source_fields) && sample_key(record) == sample_key(*last_draw) &&
                            ready_pixels_complete(record);
            if (matching && held_stages.contains("held-visible")) {
                const auto& baseline = held_stages.at("held-visible");
                matching = same_fields(record, baseline, {"columns", "card_extent", "dataset_identity", "first_row"}) &&
                           record["clip"][3] == baseline["clip"][3];
                if (name == "held-aligned" || name == "held-extra" || name == "held-restored")
                    matching = matching && return_baseline &&
                               scalar(record, "rows") == scalar(*return_baseline, "rows") + (name == "held-extra" ? 1U : 0U);
            }
            valid = valid && matching;
            if (matching) held_stages.emplace(name, record);
            last_draw.reset();
            return;
        }
        if (name.starts_with("return-")) {
            constexpr std::array return_names{"return-cached", "return-aligned", "return-extra", "return-restored"};
            bool matching = return_stage < return_names.size() && name == return_names[return_stage] && last_draw &&
                            same_fields(record, *last_draw, source_fields) && sample_key(record) == sample_key(*last_draw) &&
                            ready_pixels_complete(record);
            if (matching && return_baseline) {
                matching = same_fields(record, *return_baseline, {"columns", "card_extent", "dataset_identity"}) &&
                           record["clip"][3] == (*return_baseline)["clip"][3] && scalar(record, "first_row") == 0U &&
                           scalar(record, "rows") == scalar(*return_baseline, "rows") + (return_stage == 2U ? 1U : 0U);
            }
            valid = valid && matching;
            if (matching) {
                if (!return_baseline) return_baseline = record;
                ++return_stage;
                return_round_trip = return_stage == return_names.size();
            }
            last_draw.reset();
            return;
        }
        bool matching = stages.size() < names.size() && name == names[stages.size()] && last_draw &&
                        same_fields(record, *last_draw, image_fields) && same_fields(record, *last_draw, {"bounds", "image", "clip"});
        const auto allocation = allocation_key(record);
        matching = matching && (!staged_allocation || *staged_allocation == allocation);
        if (matching) {
            const auto row = scalar(record, "first_row");
            const double image_top = record["image"][1].get<double>();
            const double clip_top = record["clip"][1].get<double>();
            if (name == "fractional") matching = row == 0U && image_top < clip_top;
            if (name == "row1") matching = row == 1U;
            if (name == "row2") matching = row == 2U;
            if (name == "row10") matching = row == 10U;
            if (name == "restored") matching = row == 0U && std::abs(image_top - clip_top) < 1.0;
            if (name == "end") {
                const auto columns = scalar(record, "columns");
                const auto total = scalar(record, "matching_count");
                matching = columns != 0U && row + scalar(record, "rows") == (total + columns - 1U) / columns &&
                           std::abs(image_top + record["image"][3].get<double>() - clip_top - record["clip"][3].get<double>()) < 1.0;
            }
        }
        valid = valid && matching;
        if (matching) {
            stages.emplace(name);
            observe_staged_rows(allocation, scalar(record, "rows"));
        }
        last_draw.reset();
    }

    [[nodiscard]] static bool visible(const nlohmann::json& record) {
        const auto rectangle = [&](const char* field) {
            if (!record.contains(field) || !record[field].is_array() || record[field].size() != 4U)
                return std::optional<std::array<double, 4U>>{};
            std::array<double, 4U> result{};
            for (std::size_t i = 0U; i != result.size(); ++i) {
                if (!record[field][i].is_number()) return std::optional<std::array<double, 4U>>{};
                result[i] = record[field][i].get<double>();
                if (!std::isfinite(result[i])) return std::optional<std::array<double, 4U>>{};
            }
            if (result[2] <= 0.0 || result[3] <= 0.0) return std::optional<std::array<double, 4U>>{};
            return std::optional{result};
        };
        const auto image = rectangle("image"), clip = rectangle("clip"), bounds = rectangle("bounds");
        if (!image || !clip || !bounds || (*clip)[0] < 0.0 || (*clip)[1] < 0.0) return false;
        return std::max((*image)[0], (*clip)[0]) < std::min((*image)[0] + (*image)[2], (*clip)[0] + (*clip)[2]) &&
               std::max((*image)[1], (*clip)[1]) < std::min((*image)[1] + (*image)[3], (*clip)[1] + (*clip)[3]);
    }

    void consume(const nlohmann::json& record) {
        const std::string event = record.value("event", "");
        if (event == "integration.atlas_ready_cell") {
            const auto acquisition = acquisitions.find(sample_key(record));
            bool matching = acquisition != acquisitions.end() && record.value("matched", false) && scalar(record, "sampled_pixels") != 0U &&
                            same_fields(record, acquisition->second, source_fields);
            const auto index = scalar(record, "compiled_index");
            if (matching) {
                const auto& indices = record["visible_indices"];
                const auto found = std::ranges::find(indices, nlohmann::json(index));
                const auto slot = static_cast<std::size_t>(found - indices.begin());
                matching = found != indices.end() && record.contains("ready_slots") && slot < record["ready_slots"].size() &&
                           record["ready_slots"][slot] == true;
            }
            matching =
                matching && record.contains("cell_sample_x") && record.contains("cell_sample_y") && record.contains("cell_sample_rgba");
            if (matching) {
                if (pixel_meaning && !same_fields(record, *pixel_meaning,
                                                  {"dataset_identity", "card_extent", "augmentation_enabled", "augmentation_seed",
                                                   "overlay_boxes", "overlay_masks", "overlay_labels"}))
                    retained_cell_pixels.clear();
                pixel_meaning = record;
                const auto point = std::tuple{index, scalar(record, "cell_sample_x"), scalar(record, "cell_sample_y")};
                const auto prior = retained_cell_pixels.find(point);
                if (prior != retained_cell_pixels.end() && return_stage > 0U && stages.empty() && !away_return &&
                    prior->second != scalar(record, "cell_sample_rgba"))
                    matching = false;
                if (retained_cell_pixels.size() >= kAcceptanceRecordLimit && prior == retained_cell_pixels.end())
                    matching = false;
                else
                    retained_cell_pixels[point] = scalar(record, "cell_sample_rgba");
            }
            valid = valid && matching;
            if (matching) {
                if (ready_cell_samples.size() >= kAcceptanceRecordLimit && !ready_cell_samples.contains(sample_key(record)))
                    valid = false;
                else
                    ready_cell_samples[sample_key(record)].emplace(index);
            }
            return;
        }
        if (event == "iced.gallery.source") {
            const auto key = source_key(record);
            const auto session = std::pair{key[0], key[3]};
            const auto existing = sources.find(key);
            if (std::ranges::any_of(key, [](const auto value) { return value == 0U; })) {
                valid = false;
            } else if (existing != sources.end()) {
                // Cached pixels retain their source revision when a new work
                // generation observes them. Only their physical source facts
                // are immutable; readiness is checked on each actual draw.
                valid = valid && same_fields(record, existing->second, source_fields);
            } else if (sources.size() >= kAcceptanceRecordLimit || sessions.contains(session)) {
                valid = false;
            } else {
                sources.emplace(key, record);
                sessions.emplace(session, key);
            }
            return;
        }
        if (event == "iced.surface.sample_acquired") {
            const auto key = sample_key(record);
            if (acquisitions.size() >= kAcceptanceRecordLimit || acquisitions.contains(key)) {
                valid = false;
                return;
            }
            acquisitions.emplace(key, record);
            return;
        }
        if (event == "iced.surface.scroll_stage") {
            stage(record);
            return;
        }
        if (record.value("control", "") != kExploreGalleryControl) return;
        if (event == "iced.surface.sample_draw_clipped" || event == "iced.surface.sample_draw_rejected") {
            last_draw.reset();
            return;
        }
        if (event != "iced.surface.draw_encoded") return;
        const auto source = sources.find(source_key(record));
        const auto capture = acquisitions.find(sample_key(record));
        const auto columns = scalar(record, "columns"), rows = scalar(record, "rows");
        const auto width = scalar(record, "content_width"), height = scalar(record, "content_height");
        bool matching = source != sources.end() && capture != acquisitions.end() && visible(record) &&
                        SurfaceAudit::valid_identity(record.value("surface", "")) && scalar(record, "generation") != 0U &&
                        scalar(record, "presentation_revision") != 0U && width <= scalar(record, "width") &&
                        height <= scalar(record, "height") && scalar(record, "source_revision") == scalar(record, "frame_revision") &&
                        columns != 0U && rows != 0U && width != 0U && height != 0U && scalar(record, "card_extent") != 0U &&
                        scalar(record, "row_capacity") >= rows && scalar(record, "row_origin") < scalar(record, "row_capacity") &&
                        width / columns == scalar(record, "card_extent") && width % columns == 0U &&
                        height == scalar(record, "row_capacity") * scalar(record, "card_extent");
        if (matching) {
            matching = same_fields(record, source->second, source_fields) && same_fields(record, capture->second, image_fields) &&
                       same_fields(record, capture->second, {"layer", "slot"});
            const auto& image = record["image"];
            const auto& bounds = record["bounds"];
            matching = matching &&
                       std::abs(image[2].get<double>() / static_cast<double>(columns) -
                                image[3].get<double>() / static_cast<double>(rows)) < 0.01 &&
                       image[0] == bounds[0] && image[1] == bounds[1] && image[2] == bounds[2];
        }
        if (matching) {
            matching = record.contains("visible_indices") && record["visible_indices"].is_array() && record.contains("ready_slots") &&
                       record["ready_slots"].is_array() && record["ready_slots"].size() == record["visible_indices"].size();
        }
        if (matching && overlap_draw &&
            !same_fields(record, *overlap_draw, {"dataset_identity", "card_extent", "augmentation_enabled", "augmentation_seed"}))
            retained_ready.clear();
        if (matching) {
            for (std::size_t slot = 0U; slot < record["visible_indices"].size(); ++slot) {
                if (!record["visible_indices"][slot].is_number_unsigned() || !record["ready_slots"][slot].is_boolean()) {
                    matching = false;
                    break;
                }
                const auto index = record["visible_indices"][slot].get<std::uint64_t>();
                const bool ready = record["ready_slots"][slot].get<bool>();
                const auto found = retained_ready.find(index);
                // The return window is deliberately retained. Other distant
                // scrolls can legitimately evict cells from the bounded cache.
                if (return_stage > 0U && !away_return && stages.empty() && !held_stages.contains("held-complete") &&
                    found != retained_ready.end() && found->second && !ready)
                    matching = false;
                if (retained_ready.size() >= kAcceptanceRecordLimit && found == retained_ready.end())
                    matching = false;
                else
                    retained_ready[index] = ready;
            }
        }
        valid = valid && matching;
        if (matching) {
            overlap_draw = record;
            seen = true;
            if (staged_allocation) observe_staged_rows(allocation_key(record), rows);
            last_draw = record;
            drawn_rows.emplace(scalar(record, "first_row"));
        } else {
            last_draw.reset();
        }
    }
};

struct BrowserAudit final {
    AtlasDrawAudit atlas_draws;
    bool owned_atlas_seen = false;
    bool owned_atlas_current = false;
    bool owned_atlas_interrupted = false;
    std::map<std::string, std::size_t, std::less<>> renderer_reconstructions;

    struct Bounds final {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;

        [[nodiscard]] bool valid() const noexcept { return width > 0.0 && height > 0.0; }

        [[nodiscard]] bool contains_horizontally(const Bounds& inner, const double tolerance = 1.0) const noexcept {
            return inner.x >= x - tolerance && inner.x + inner.width <= x + width + tolerance;
        }

        [[nodiscard]] bool contains(const Bounds& inner, const double tolerance = 1.0) const noexcept {
            return contains_horizontally(inner, tolerance) && inner.y >= y - tolerance && inner.y + inner.height <= y + height + tolerance;
        }
    };
    struct SurfaceGeometry final {
        std::uint64_t presentation_revision = 0U;
        std::uint64_t source_revision = 0U;
        double width = 0.0;
        double height = 0.0;
    };
    struct SurfaceContainer final {
        std::uint64_t presentation_revision = 0U;
        std::uint64_t source_revision = 0U;
        double width = 0.0;
        double height = 0.0;
        std::string control;
    };

    std::set<std::string, std::less<>> controls;
    std::map<std::string, Bounds, std::less<>> page_bounds;
    Bounds train_setup;
    Bounds train_center;
    Bounds train_workspace;
    Bounds train_advanced;
    Bounds train_diagnostics;
    Bounds explore_dataset;
    Bounds explore_gallery;
    Bounds explore_details;
    Bounds compile_action;
    Bounds compile_progress;
    Bounds model_card;
    Bounds model_progress;
    std::array<Bounds, 6> model_parts;
    Bounds settings_modal;
    Bounds settings_footer;
    Bounds settings_reset;
    Bounds settings_close;
    Bounds settings_appearance;
    Bounds settings_typography;
    Bounds settings_environment;
    Bounds settings_show_fps;
    Bounds error_modal;
    Bounds error_copy;
    Bounds error_dismiss;
    Bounds benchmark_override;
    std::array<Bounds, 8> advanced_fixed;
    Bounds advanced_assignment;
    std::array<Bounds, 3> advanced_match_free;
    Bounds advanced_denoising_toggle;
    std::array<Bounds, 4> advanced_denoising;
    Bounds annotation_sidebar;
    Bounds annotation_timeline;
    Bounds annotation_operation;
    Bounds annotation_stop;
    Bounds annotation_brush;
    Bounds annotation_tool_control;
    std::array<Bounds, 5> settings_numeric_controls;
    std::array<Bounds, 5> settings_numeric_labels;
    std::array<Bounds, 5> settings_numeric_values;
    bool bounds_valid = true;
    bool fluent = false;
    std::set<std::string, std::less<>> shared_primary;
    std::map<std::string, std::array<double, 4>, std::less<>> shared_primary_colors;
    std::map<std::string, std::uint64_t, std::less<>> rendered_style_keys;
    std::set<std::string, std::less<>> rendered_controls;
    std::map<std::string, std::uint64_t, std::less<>> rendered_control_keys;
    std::map<std::string, double, std::less<>> rendered_control_scales;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> explore_slots;
    struct GalleryGenerationEvidence final {
        std::map<std::uint64_t, std::uint64_t> slots;
        std::uint64_t digest = 0U;
        std::uint64_t source_revision = 0U;
        std::uint64_t snapshot_revision = 0U;
        std::vector<bool> ready_slots;
    };
    std::map<std::uint64_t, GalleryGenerationEvidence> gallery_generations;
    // CLEANUP-IGNORE: Rendered slot-frame identity precedes cursor facts; later booleans track workflow outcomes.
    std::map<std::uint64_t, std::uint64_t> explore_slot_frames;
    std::uint64_t final_cursor_revision = 0U;
    std::uint64_t final_cursor_frame_revision = 0U;
    std::uint64_t final_cursor_generation = 0U;
    // CLEANUP-IGNORE: Rendered cursor evidence and UI assertions are separate from native lifecycle audit flags.
    std::uint64_t final_cursor_slot_count = 0U;
    bool benchmark_purple = false;
    bool model_copy = false;
    bool spinnerless_integer = false;
    bool spinnerless_floating = false;
    bool advanced_integer_persisted = false;
    bool advanced_floating_persisted = false;
    bool show_fps_round_trip = false;
    bool benchmark_round_trip = false;
    bool bootstrap = false;
    bool dataset_configured = false;
    bool progress = false;
    bool compile_metrics = false;
    std::uint64_t compile_completed = 0U;
    std::uint64_t compile_total = 0U;
    // CLEANUP-IGNORE: Browser workflow outcomes do not duplicate the native process audit's lifecycle flags.
    std::uint64_t compile_dropped = 0U;
    bool dataset_complete = false;
    bool explore_ready = false;
    bool sweep = false;
    bool scrolled = false;
    bool detail = false;
    bool augmentation_enabled = false;
    bool augmentation_rerolled = false;
    std::map<std::uint64_t, std::uint64_t> augmentation_frames;
    bool reshuffle_order_only = false;
    bool detail_source = false;
    bool reopened = false;
    bool detail_fit = false;
    bool viewer_complete = false;
    bool gallery_no_input_complete = false;
    bool initial_atlas_complete = false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> atlas_canvas_pixels;
    std::set<std::uint64_t> atlas_visibility_modes;
    std::set<std::string> atlas_scroll_stages;
    std::set<std::string> atlas_window_draws;
    std::set<double> atlas_device_scales;
    std::set<std::string> atlas_themes;
    std::set<std::string> atlas_notices;
    std::set<std::pair<std::uint64_t, std::uint64_t>> square_atlas_frames;
    bool atlas_geometry_valid = true;
    bool atlas_native_capacity = false;
    std::uint64_t capacity_retry_publication = 0U;
    std::uint64_t capacity_retry_frame = 0U;
    std::uint64_t pending_focus_index = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t pending_selection_index = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t pending_read_generation = 0U;
    std::set<std::pair<std::uint64_t, std::uint64_t>> atlas_scaled_frames;
    std::set<std::string> viewer_import_edits;
    std::set<std::string> annotation_shapes;
    std::set<std::pair<std::uint64_t, std::uint64_t>> annotation_pixel_frames;
    std::map<std::string, std::size_t> annotation_product_operations;
    std::set<std::string> annotation_layouts;
    std::set<std::string> annotation_capabilities;
    std::size_t annotation_swatches = 0U;
    std::size_t annotation_previews = 0U;
    bool annotation_pixels_valid = true;
    bool annotation_layout_valid = true;
    std::uint64_t viewer_presentation = 0U;
    std::set<std::uint64_t> viewer_overlay_modes;
    std::map<std::uint64_t, std::array<double, 3U>> viewer_label_colors;
    bool viewer_class_colors = true;
    bool viewer_labels_without_boxes = false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> viewer_label_products;
    bool original_detail_drawn = false;
    bool ui_scale_drag = false;
    bool ui_scale_released = false;
    bool ui_scale_restored = false;
    std::set<std::string, std::less<>> ui_scale_pointer_stages;
    // CLEANUP-IGNORE: Modal usability and following rendered-grid facts are independent end-to-end UI evidence,
    // not streaming-algorithm control flags.
    bool error_modal_usable = false;
    bool exact_grid = false;
    std::uint64_t exact_grid_revision = 0U;
    std::uint64_t exact_grid_frame_revision = 0U;
    std::uint64_t exact_grid_capacity_width = 0U;
    std::uint64_t exact_grid_capacity_height = 0U;
    std::uint64_t exact_grid_width = 0U;
    std::uint64_t exact_grid_height = 0U;
    bool newest_placeholder = false;
    bool pointer_inverse = false;
    bool pointer_dispatched = false;
    bool pointer_selected = false;
    std::uint64_t pointer_revision = 0U;
    std::uint64_t pointer_frame_revision = 0U;
    std::uint64_t pending_pointer_frame_revision = 0U;
    std::uint64_t pointer_slot = 0U;
    std::uint64_t pointer_compiled_index = 0U;
    bool upscale_growth = false;
    bool upscale_presentation = false;
    bool upscale_later_frame = false;
    std::set<std::string, std::less<>> upscale_modes;
    std::set<std::string, std::less<>> upscale_presentations;
    std::set<std::string, std::less<>> upscale_completed_pixels;
    std::set<std::string, std::less<>> upscale_same_method;
    bool atlas_identities = false;
    bool annotation_ready = false;
    bool annotation_tool = false;
    bool annotation_pointer = false;
    std::set<std::uint64_t> surface_draws;
    std::set<std::uint64_t> surface_redraws;
    std::map<std::uint64_t, std::size_t> surface_redraw_counts;
    std::vector<SurfaceGeometry> surface_geometries;
    std::vector<SurfaceContainer> surface_containers;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> surface_contents;
    std::map<std::pair<std::uint64_t, std::uint64_t>, double> surface_scales;
    bool complete = false;
    bool failed = false;
    bool firefox_import = false;
    bool firefox_claim = false;
    bool firefox_ready = false;
    bool workspace_protocol_failure = false;
    bool panic = false;
    std::size_t ordinal = 0U;
    std::size_t phase_progress_revision = 0U;
    std::size_t work_progress_revision = 0U;
    std::uint64_t reopen_snapshot_progress = 0U;
    std::uint64_t reopen_draw_progress = 0U;
    std::string phase_progress_class;
    std::string phase_progress_name;
    std::size_t progress_ordinal = 0U;
    std::size_t dataset_complete_ordinal = 0U;
    std::uint64_t compiled_images = 0U;
    std::uint64_t compiled_width = 0U;
    std::uint64_t compiled_height = 0U;
    std::uint64_t presentation_receipt = 0U;

    void consume_gallery_generation(const nlohmann::json& record) {
        const auto generation = scalar(record, "gallery_generation");
        const auto indices = record.find("visible_indices");
        if (generation == 0U || indices == record.end() || !indices->is_array() || indices->size() > kAcceptanceSlotLimit) {
            bounds_valid = false;
            return;
        }
        std::vector<std::uint32_t> visible_indices;
        visible_indices.reserve(indices->size());
        std::map<std::uint64_t, std::uint64_t> slots;
        for (std::size_t slot = 0U; slot != indices->size(); ++slot) {
            const auto& compiled_index = (*indices)[slot];
            if (!compiled_index.is_number_unsigned() || compiled_index.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
                bounds_valid = false;
                return;
            }
            const auto value = compiled_index.get<std::uint32_t>();
            visible_indices.push_back(value);
            slots.emplace(slot, value);
        }
        std::vector<bool> ready_slots;
        if (const auto readiness = record.find("ready_slots"); readiness != record.end()) {
            if (!readiness->is_array() || readiness->size() != slots.size() ||
                !std::ranges::all_of(*readiness, [](const auto& ready) { return ready.is_boolean(); })) {
                bounds_valid = false;
                return;
            }
            ready_slots = readiness->get<std::vector<bool>>();
        }
        const GalleryGenerationEvidence evidence{
            .slots = std::move(slots),
            .digest = mmltk::controller::explore_visible_indices_digest(visible_indices),
            .source_revision = scalar(record, "source_revision"),
            .snapshot_revision = scalar(record, "source_observation_revision"),
            .ready_slots = std::move(ready_slots),
        };
        const auto existing = gallery_generations.find(generation);
        if (existing != gallery_generations.end()) {
            bounds_valid = bounds_valid && existing->second.slots == evidence.slots && existing->second.digest == evidence.digest;
            if (evidence.source_revision >= existing->second.source_revision) {
                existing->second.source_revision = evidence.source_revision;
                existing->second.snapshot_revision = evidence.snapshot_revision;
                existing->second.ready_slots = evidence.ready_slots;
            }
        } else if (gallery_generations.size() == kAcceptanceGenerationLimit) {
            bounds_valid = false;
        } else {
            gallery_generations.emplace(generation, evidence);
        }
    }

    void consume(const nlohmann::json& record) {
        atlas_draws.consume(record);
        const auto surface_event = record.value("event", "");
        if (surface_event == "iced.surface.renderer_reconstructed")
            ++renderer_reconstructions[record.value("requested_surface", "")];
        else if (surface_event == "iced.surface.draw_encoded") {
            owned_atlas_seen = atlas_draws.seen;
            const auto control = record.value("control", "");
            if (control == kExploreGalleryControl && atlas_draws.last_draw)
                owned_atlas_current = true;
            else if (control == EXPLORE_DETAIL_WORKSPACE || control == "workflow.visual.workspace")
                owned_atlas_current = false;
        } else if (surface_event == "iced.surface.sample_draw_missing") {
            const auto control = record.value("control", "");
            owned_atlas_interrupted =
                owned_atlas_interrupted || (owned_atlas_current && (control.empty() || control == kExploreGalleryControl));
        }
        ++ordinal;
        const std::string event = record.value("event", "");
        if (event == "iced.gallery.source") consume_gallery_generation(record);
        if (event == "integration.phase_progress") {
            const std::string deadline_class = record.value("control", "");
            const std::string phase = record.value("detail", "");
            const bool valid_class = deadline_class == "startup" || deadline_class == "interaction" || deadline_class == "work";
            bounds_valid = bounds_valid && valid_class && !phase.empty();
            if (valid_class && !phase.empty()) {
                if (phase == phase_progress_name) {
                    bounds_valid = bounds_valid && phase_progress_class == deadline_class;
                } else {
                    phase_progress_class = deadline_class;
                    phase_progress_name = phase;
                    ++phase_progress_revision;
                }
            }
        } else if (event == "integration.explore_reopen_wait") {
            const auto revision = scalar(record, "a");
            if (revision > reopen_snapshot_progress) {
                reopen_snapshot_progress = revision;
                ++work_progress_revision;
            }
        } else if (event == "integration.explore_reopen_draw") {
            const auto presentation_revision = scalar(record, "d");
            if (presentation_revision > reopen_draw_progress) {
                reopen_draw_progress = presentation_revision;
                ++work_progress_revision;
            }
        } else if (event == "integration.control_bounds") {
            const std::string control = record.value("control", "");
            const double width = numeric(record, "c");
            const double height = numeric(record, "d");
            const Bounds bounds{
                .x = numeric(record, "a"),
                .y = numeric(record, "b"),
                .width = width,
                .height = height,
            };
            bounds_valid = bounds_valid && !control.empty() && width > 0.0 && height > 0.0;
            if (!control.empty()) controls.insert(control);
            // CLEANUP-IGNORE: Each stable visual control maps to a distinct acceptance-evidence slot.
            if (control == EXPLORE_DATASET_PANE)
                explore_dataset = bounds;
            else if (control == kExploreGalleryControl)
                explore_gallery = bounds;
            else if (control == EXPLORE_DETAILS_PANE)
                explore_details = bounds;
            else if (control == COMPILE_DATASET)
                compile_action = bounds;
            else if (control == COMPILE_PROGRESS)
                compile_progress = bounds;
            else if (control == TRAIN_MODEL_CARD)
                model_card = bounds;
            else if (control == TRAIN_MODEL_PROGRESS)
                model_progress = bounds;
            else if (control == TRAIN_MODEL_SELECTOR)
                model_parts[0] = bounds;
            else if (control == TRAIN_MODEL_PRESETS)
                model_parts[1] = bounds;
            else if (control == TRAIN_MODEL_DIVIDER)
                model_parts[2] = bounds;
            else if (control == TRAIN_MODEL_CUSTOM)
                model_parts[3] = bounds;
            else if (control == TRAIN_MODEL_STATUS)
                model_parts[4] = bounds;
            else if (control == TRAIN_MODEL_ACTION)
                model_parts[5] = bounds;
            else if (control == SETTINGS_MODAL)
                settings_modal = bounds;
            else if (control == SETTINGS_FOOTER)
                settings_footer = bounds;
            else if (control == SETTINGS_RESET)
                settings_reset = bounds;
            else if (control == SETTINGS_CLOSE)
                settings_close = bounds;
            else if (control == "settings.group.appearance")
                settings_appearance = bounds;
            else if (control == "settings.group.typography")
                settings_typography = bounds;
            else if (control == "settings.group.environment")
                settings_environment = bounds;
            else if (control == "settings.show_fps")
                settings_show_fps = bounds;
            else if (control == ERROR_MODAL)
                error_modal = bounds;
            else if (control == ERROR_COPY)
                error_copy = bounds;
            else if (control == ERROR_DISMISS)
                error_dismiss = bounds;
            else if (control == BENCHMARK_OVERRIDE)
                benchmark_override = bounds;
            else if (control == ANNOTATION_SIDEBAR)
                annotation_sidebar = bounds;
            else if (control == ANNOTATION_TIMELINE)
                annotation_timeline = bounds;
            else if (control == ANNOTATION_OPERATION)
                annotation_operation = bounds;
            else if (control == ANNOTATION_STOP)
                annotation_stop = bounds;
            else if (control == ANNOTATION_BRUSH_RADIUS)
                annotation_brush = bounds;
            else if (control.starts_with("annotation.tool."))
                annotation_tool_control = bounds;
            for (std::size_t index = 0; index < SETTINGS_NUMERIC_CONTROLS.size(); ++index) {
                if (control == SETTINGS_NUMERIC_CONTROLS[index])
                    settings_numeric_controls[index] = bounds;
                else if (control == std::string{SETTINGS_NUMERIC_CONTROLS[index]} + ".label")
                    settings_numeric_labels[index] = bounds;
                else if (control == std::string{SETTINGS_NUMERIC_CONTROLS[index]} + ".value")
                    settings_numeric_values[index] = bounds;
            }
        } else if (event == "integration.advanced_field") {
            const Bounds bounds{
                .x = numeric(record, "a"),
                .y = numeric(record, "b"),
                .width = numeric(record, "c"),
                .height = numeric(record, "d"),
            };
            const std::string field_detail = record.value("detail", "");
            const auto indexed = [&field_detail, &bounds](const std::string_view prefix, auto& output) {
                if (!field_detail.starts_with(prefix)) return false;
                std::size_t index = 0U;
                const std::string_view suffix{field_detail.data() + prefix.size(), field_detail.size() - prefix.size()};
                const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
                if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || index >= output.size()) return false;
                output[index] = bounds;
                return true;
            };
            if (field_detail == "assignment")
                advanced_assignment = bounds;
            else if (field_detail == "dn-toggle")
                advanced_denoising_toggle = bounds;
            else if (!indexed("fixed-", advanced_fixed) && !indexed("match-free-", advanced_match_free))
                static_cast<void>(indexed("dn-", advanced_denoising));
        } else if (event == "integration.page_region") {
            const std::string control = record.value("control", "");
            const std::string page = record.value("detail", "");
            const Bounds bounds{
                .x = numeric(record, "a"),
                .y = numeric(record, "b"),
                .width = numeric(record, "c"),
                .height = numeric(record, "d"),
            };
            bounds_valid = bounds_valid && !control.empty() && !page.empty() && bounds.valid();
            const std::string identity = page + ":" + control;
            page_bounds.insert_or_assign(identity, bounds);
            if (page == "Train") {
                if (control == "workflow.setup")
                    train_setup = bounds;
                else if (control == "workflow.workspace_and_advanced")
                    train_center = bounds;
                else if (control == "workflow.workspace")
                    train_workspace = bounds;
                else if (control == "workflow.advanced")
                    train_advanced = bounds;
                else if (control == "workflow.diagnostics")
                    train_diagnostics = bounds;
            }
        } else if (event == "integration.fluent_shell") {
            fluent = record.value("control", "") == "navigation" &&
                     (record.value("detail", "") == "light" || record.value("detail", "") == "dark") && numeric(record, "a") > 0.0 &&
                     numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0 && numeric(record, "d") == 1.0;
        } else if (event == "integration.rendered_style") {
            if (record.value("detail", "") == "shared-primary" && numeric(record, "d") == 1.0 &&
                (numeric(record, "a") > 0.0 || numeric(record, "b") > 0.0 || numeric(record, "c") > 0.0)) {
                shared_primary.insert(record.value("control", ""));
                shared_primary_colors.insert_or_assign(record.value("control", ""), std::array{numeric(record, "a"), numeric(record, "b"),
                                                                                               numeric(record, "c"), numeric(record, "d")});
                rendered_style_keys.insert_or_assign(record.value("control", ""), scalar(record, "render_key"));
            }
            benchmark_purple =
                benchmark_purple ||
                (record.value("control", "") == BENCHMARK_OVERRIDE && record.value("detail", "") == "benchmark-purple" &&
                 std::abs(numeric(record, "a") - 138.0 / 255.0) < 0.001 && std::abs(numeric(record, "b") - 43.0 / 255.0) < 0.001 &&
                 std::abs(numeric(record, "c") - 226.0 / 255.0) < 0.001 && numeric(record, "d") == 1.0);
            if (record.value("detail", "") == "benchmark-purple")
                rendered_style_keys.insert_or_assign(record.value("control", ""), scalar(record, "render_key"));
        } else if (event == "integration.rendered_control") {
            const std::string control = record.value("control", "");
            const auto key = scalar(record, "a");
            const double scale = numeric(record, "d");
            bounds_valid = bounds_valid && !control.empty() && key != 0U && numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0 &&
                           scale > 0.0 && rendered_style_keys.contains(control) && rendered_style_keys.at(control) == key &&
                           (!rendered_control_keys.contains(control) || rendered_control_keys.at(control) == key);
            if (!control.empty() && key != 0U) {
                rendered_controls.insert(control);
                rendered_control_keys.insert_or_assign(control, key);
                rendered_control_scales.insert_or_assign(control, scale);
            }
        } else if (event == "integration.model_copy") {
            model_copy = record.value("control", "") == TRAIN_MODEL_CARD && record.value("detail", "") == "RF-DETR Weights" &&
                         numeric(record, "a") == 1.0;
        } else if (event == "integration.spinnerless") {
            const bool valid = numeric(record, "b") == 1.0 && numeric(record, "c") == 1.0 && numeric(record, "d") == 1.0;
            if (record.value("detail", "") == "integer-upper-lower-edges")
                spinnerless_integer = valid;
            else if (record.value("detail", "") == "floating-upper-lower-edges")
                spinnerless_floating = valid;
        } else if (event == "integration.advanced_edit") {
            const bool persisted = numeric(record, "c") > numeric(record, "b") && numeric(record, "d") == 1.0;
            if (record.value("detail", "") == "integer")
                advanced_integer_persisted = persisted;
            else if (record.value("detail", "") == "floating")
                advanced_floating_persisted = persisted;
        } else if (event == "integration.show_fps") {
            show_fps_round_trip = record.value("control", "") == "settings.show_fps" && record.value("detail", "") == "round-trip" &&
                                  numeric(record, "a") == 1.0 && numeric(record, "b") == 1.0 && numeric(record, "d") > numeric(record, "c");
        } else if (event == "integration.benchmark_override") {
            benchmark_round_trip = record.value("control", "") == BENCHMARK_OVERRIDE && record.value("detail", "") == "round-trip" &&
                                   numeric(record, "a") == 1.0 && numeric(record, "b") == 1.0 &&
                                   numeric(record, "d") > numeric(record, "c");
        } else if (event == "integration.bootstrap") {
            bootstrap = record.value("detail", "") == "typed-bootstrap";
        } else if (event == "integration.dataset_configured") {
            dataset_configured = record.value("detail", "") == "typed-settings" && record.value("control", "") == COMPILE_RESOLUTION &&
                                 scalar(record, "a") != 0U && scalar(record, "b") == 1U;
        } else if (event == "integration.compile_progress") {
            compile_completed = scalar(record, "b");
            compile_total = scalar(record, "c");
            compile_dropped = scalar(record, "d");
            progress = record.value("control", "") == COMPILE_PROGRESS && scalar(record, "a") != 0U &&
                       !record.value("detail", "").empty() && compile_total != 0U && compile_completed <= compile_total;
            if (progress && progress_ordinal == 0U) progress_ordinal = ordinal;
        } else if (event == "integration.compile_metrics") {
            const std::uint64_t elapsed = scalar(record, "a");
            const auto expected = mmltk::backend::data::estimate_progress(compile_completed, compile_total, elapsed);
            compile_metrics = record.value("control", "") == COMPILE_PROGRESS &&
                              record.value("detail", "") == "elapsed-eta-throughput-dropped" && compile_dropped != 0U &&
                              scalar(record, "b") == expected.remaining_seconds && scalar(record, "c") == expected.throughput_per_second &&
                              scalar(record, "d") == compile_dropped;
        } else if (event == "integration.dataset_complete") {
            dataset_complete = true;
            dataset_complete_ordinal = ordinal;
            compiled_images = scalar(record, "b");
            compiled_width = scalar(record, "c");
            compiled_height = scalar(record, "d");
        } else if (event == "integration.explore_ready") {
            explore_ready =
                scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.explore_sweep_observed") {
            sweep = record.value("detail", "") == "typed-viewport" && scalar(record, "b") > scalar(record, "a") &&
                    scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.explore_scrolled") {
            scrolled = scalar(record, "b") > scalar(record, "a");
        } else if (event == "integration.explore_detail") {
            detail = scalar(record, "b") != 0U;
        } else if (event == "integration.explore_augmentation") {
            const auto frame_revision = scalar(record, "d");
            const bool valid = scalar(record, "b") > scalar(record, "a") && frame_revision > scalar(record, "c");
            if (record.value("detail", "") == "enabled-rendered-seed-zero") {
                augmentation_enabled = augmentation_enabled || valid;
                if (valid) augmentation_frames.insert_or_assign(0U, frame_revision);
            } else if (record.value("detail", "") == "rerolled-distinct-seed") {
                augmentation_rerolled = augmentation_rerolled || valid;
                if (valid) augmentation_frames.insert_or_assign(1U, frame_revision);
            }
        } else if (event == "integration.explore_reshuffle") {
            reshuffle_order_only = record.value("detail", "") == "order-only" && scalar(record, "b") > scalar(record, "a") &&
                                   scalar(record, "c") == scalar(record, "d");
        } else if (event == "integration.annotation_pixel" || event == "integration.annotation_swatch" ||
                   event == "integration.annotation_capability") {
            const auto expected = record.value("expected", std::vector<double>{});
            const auto observed = record.value("observed", std::vector<double>{});
            double tolerance = event == "integration.annotation_pixel" ? 24.0 : 3.0;
            bool matched = record.value("matched", false) && expected.size() == 3U && observed.size() >= 3U;
            if (matched) {
                double gain = 1.0;
                double minimum = 0.0;
                const double peak = std::max({observed[0], observed[1], observed[2]});
                const double low = std::min({observed[0], observed[1], observed[2]});
                const double scale = event == "integration.annotation_pixel" ? record.value("source_to_screen", 0.0) : 1.0;
                matched = matched && std::isfinite(scale) && scale > 0.0;
                if (event == "integration.annotation_pixel" && scale > 0.0 && scale < 1.0 && std::ranges::max(expected) == 255.0 &&
                    std::ranges::min(expected) == 0.0 && peak - low >= 127.5) {
                    minimum = low;
                    gain = 255.0 / (peak - low);
                    tolerance = 48.0;
                }
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    matched = matched && std::isfinite(expected[channel]) && std::isfinite(observed[channel]) && expected[channel] >= 0.0 &&
                              expected[channel] <= 255.0 && observed[channel] >= 0.0 && observed[channel] <= 255.0 &&
                              std::abs(expected[channel] - (observed[channel] - minimum) * gain) <= tolerance;
            }
            annotation_pixels_valid = annotation_pixels_valid && matched;
            if (event == "integration.annotation_pixel") {
                annotation_pixels_valid = annotation_pixels_valid && scalar(record, "a") != 0U && scalar(record, "b") != 0U;
                if (annotation_pixel_frames.size() < kAcceptanceRecordLimit)
                    annotation_pixel_frames.emplace(scalar(record, "a"), scalar(record, "b"));
            } else if (event == "integration.annotation_swatch")
                ++annotation_swatches;
            else
                annotation_capabilities.insert(record.value("detail", ""));
        } else if (event == "integration.annotation_layout") {
            annotation_layouts.insert(record.value("detail", ""));
            annotation_layout_valid = annotation_layout_valid && numeric(record, "a") > 0.0 && numeric(record, "b") > 0.0 &&
                                      numeric(record, "a") <= numeric(record, "c") && numeric(record, "b") <= numeric(record, "c") &&
                                      std::abs(numeric(record, "d") - 12.0) <= 1.0;
        } else if (event == "integration.annotation_product") {
            ++annotation_product_operations[record.value("detail", "")];
            annotation_pixels_valid =
                annotation_pixels_valid && annotation_pixel_frames.contains({scalar(record, "a"), scalar(record, "b")});
        } else if (event == "integration.annotation_preview") {
            if (scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U) ++annotation_previews;
        } else if (event == "integration.annotation_shape") {
            annotation_shapes.insert(record.value("detail", ""));
        } else if (event == "integration.viewer_import_edit") {
            viewer_import_edits.insert(record.value("detail", ""));
        } else if (event == "integration.atlas_scale") {
            const double x = numeric(record, "c"), y = numeric(record, "d");
            if (x <= 0.0 || y <= 0.0 || std::abs(x - y) > 0.0001)
                atlas_geometry_valid = false;
            else if (atlas_scaled_frames.size() < kAcceptanceRecordLimit)
                atlas_scaled_frames.emplace(scalar(record, "a"), scalar(record, "b"));
        } else if (event == "integration.pending_focus") {
            pending_focus_index = scalar(record, "a");
            pending_read_generation = scalar(record, "b");
        } else if (event == "integration.pending_selection") {
            pending_selection_index = scalar(record, "a");
            bounds_valid = bounds_valid && pending_read_generation == scalar(record, "b");
        } else if (event == "integration.capacity_retry") {
            capacity_retry_frame = scalar(record, "a");
            capacity_retry_publication = scalar(record, "b");
        } else if (event == "integration.atlas_capacity") {
            atlas_native_capacity = record.value("detail", "") == "native-visible-capacity-exceeded";
        } else if (event == "integration.atlas_geometry") {
            const double width = numeric(record, "c"), height = numeric(record, "d");
            if (width <= 0.0 || height <= 0.0 || std::abs(width - height) > 0.01)
                atlas_geometry_valid = false;
            else if (square_atlas_frames.size() < kAcceptanceRecordLimit)
                square_atlas_frames.emplace(scalar(record, "a"), scalar(record, "b"));
        } else if (event == "integration.atlas_checkbox") {
            atlas_visibility_modes.emplace(scalar(record, "b"));
        } else if (event == "integration.atlas_notice") {
            if (numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0) atlas_notices.emplace(record.value("control", ""));
        } else if (event == "integration.atlas_window") {
            atlas_device_scales.emplace(numeric(record, "c"));
        } else if (event == "integration.atlas_window_draw") {
            if (numeric(record, "b") > 0.0 && numeric(record, "c") > 0.0) atlas_window_draws.emplace(record.value("detail", ""));
        } else if (event == "integration.atlas_visibility") {
            atlas_themes.emplace(record.value("detail", ""));
        } else if (event == "integration.atlas_scroll") {
            atlas_scroll_stages.emplace(record.value("detail", ""));
        } else if (event == "integration.gallery_no_input_complete") {
            gallery_no_input_complete =
                scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.atlas_canvas_pixels") {
            if (scalar(record, "c") != 0U && scalar(record, "c") == scalar(record, "d"))
                atlas_canvas_pixels.emplace(scalar(record, "a"), scalar(record, "b"));
        } else if (event == "integration.initial_atlas_complete") {
            initial_atlas_complete = record.value("detail", "") == "no-input-canvas-pixels" && scalar(record, "a") != 0U &&
                                     scalar(record, "b") != 0U && atlas_canvas_pixels.contains({scalar(record, "c"), scalar(record, "d")});
        } else if (event == "integration.viewer_complete") {
            viewer_presentation = scalar(record, "a");
            viewer_complete = scalar(record, "a") != 0U && scalar(record, "b") != 0U && scalar(record, "c") == 1U;
        } else if (event == "integration.viewer_overlay") {
            if (scalar(record, "c") != 0U && scalar(record, "d") != 0U) viewer_overlay_modes.insert(scalar(record, "b"));
        } else if (event == "integration.viewer_label_rgb") {
            if (viewer_label_colors.size() < 256U || viewer_label_colors.contains(scalar(record, "a")))
                viewer_label_colors.insert_or_assign(scalar(record, "a"),
                                                     std::array{numeric(record, "b"), numeric(record, "c"), numeric(record, "d")});
        } else if (event == "integration.viewer_label_frame") {
            if (record.value("detail", "") == "exact-scene-product" && scalar(record, "d") == 0U && scalar(record, "b") != 0U)
                viewer_label_products.emplace(scalar(record, "c"), scalar(record, "a"));
        } else if (event == "integration.viewer_label_catalog") {
            const auto found = viewer_label_colors.find(scalar(record, "a"));
            if (found != viewer_label_colors.end() && scalar(record, "b") != 0U) {
                std::uint8_t red = 0U, green = 0U, blue = 0U;
                mmltk::backend::imaging::raster::detail::color::class_color(static_cast<int>(scalar(record, "a")),
                                                                            static_cast<int>(scalar(record, "b")), red, green, blue);
                const std::array expected{red, green, blue};
                for (std::size_t channel = 0U; channel != expected.size(); ++channel)
                    viewer_class_colors =
                        viewer_class_colors && std::abs(found->second[channel] - expected[channel] / 255.0) <= 1.0 / 255.0;
                viewer_labels_without_boxes = viewer_labels_without_boxes || scalar(record, "c") == 0U;
            }
        } else if (event == "integration.explore_detail_source") {
            detail_source = record.value("detail", "") == "padded-to-original-sampling" && scalar(record, "a") == scalar(record, "c") &&
                            scalar(record, "b") > scalar(record, "d") && scalar(record, "d") != 0U;
        } else if (event == "integration.explore_detail_fit") {
            detail_fit = record.value("detail", "") == "centered-contained" && numeric(record, "c") <= numeric(record, "a") + 1.0 &&
                         numeric(record, "d") <= numeric(record, "b") + 1.0 &&
                         (std::abs(numeric(record, "c") - numeric(record, "a")) < 1.0 ||
                          std::abs(numeric(record, "d") - numeric(record, "b")) < 1.0);
        } else if (event == "integration.viewer_sample" && numeric(record, "d") > 0.0) {
            original_detail_drawn = original_detail_drawn || (scalar(record, "a") == 512U && scalar(record, "b") == 256U &&
                                                              std::abs(numeric(record, "c") / numeric(record, "d") - 2.0) < 0.01);
        } else if (event == "integration.explore_reopened") {
            reopened = record.value("detail", "") == "usable-after-reopen" && scalar(record, "a") != 0U && scalar(record, "b") != 0U &&
                       scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.ui_scale_drag") {
            if (record.value("detail", "") == "second-position")
                ui_scale_drag = numeric(record, "a") == numeric(record, "c") && numeric(record, "b") > 0.85 &&
                                numeric(record, "a") != numeric(record, "b") && numeric(record, "d") == 2.0;
            else if (record.value("detail", "") == "released-and-settled")
                ui_scale_released =
                    numeric(record, "b") > 0.85 && numeric(record, "b") == numeric(record, "c") && numeric(record, "d") != 0.0;
        } else if (event == "integration.ui_scale_pointer") {
            const std::string stage = record.value("detail", "");
            const bool pressed = stage != "released";
            if (record.value("control", "") == "settings.ui_scale" && numeric(record, "b") == (pressed ? 1.0 : 0.0) &&
                numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0)
                ui_scale_pointer_stages.insert(stage);
        } else if (event == "integration.ui_scale_restored") {
            ui_scale_restored = record.value("control", "") == "settings.ui_scale" && record.value("detail", "") == "baseline" &&
                                numeric(record, "a") == numeric(record, "b") && numeric(record, "b") == numeric(record, "c") &&
                                numeric(record, "d") != 0.0;
        } else if (event == "integration.error_modal") {
            error_modal_usable = record.value("detail", "") == "copy-and-dismiss" && numeric(record, "a") == 1.0 &&
                                 numeric(record, "b") == 1.0 && numeric(record, "c") == 1.0;
        } else if (event == "integration.explore_exact_grid") {
            const auto columns = scalar(record, "c");
            const auto rows = scalar(record, "d");
            exact_grid = record.value("detail", "") == "oversized-logical-fill" && columns != 0U && rows != 0U &&
                         scalar(record, "a") % columns == 0U && scalar(record, "b") % rows == 0U &&
                         scalar(record, "a") / columns == scalar(record, "b") / rows;
            exact_grid_width = scalar(record, "a");
            exact_grid_height = scalar(record, "b");
        } else if (event == "integration.explore_exact_grid_capacity") {
            exact_grid_revision = scalar(record, "a");
            exact_grid_frame_revision = scalar(record, "b");
            exact_grid_capacity_width = scalar(record, "c");
            exact_grid_capacity_height = scalar(record, "d");
        } else if (event == "integration.explore_scroll_placeholder") {
            newest_placeholder = record.value("detail", "") == "newest-without-wait-all" && scalar(record, "a") != 0U &&
                                 scalar(record, "b") != 0U && scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.explore_final_cursor") {
            final_cursor_revision = scalar(record, "a");
            final_cursor_frame_revision = scalar(record, "b");
            final_cursor_generation = scalar(record, "c");
            final_cursor_slot_count = scalar(record, "d");
        } else if (event == "integration.explore_pointer_inverse") {
            pointer_inverse = record.value("detail", "") == "rendered-grid-slot" && scalar(record, "a") == scalar(record, "b") &&
                              scalar(record, "d") != 0U;
            pointer_slot = scalar(record, "a");
            pointer_compiled_index = scalar(record, "c");
            pointer_revision = scalar(record, "d");
        } else if (event == "integration.explore_pointer_scheduled") {
            pending_pointer_frame_revision = scalar(record, "a");
        } else if (event == "integration.surface_click_dispatched" && record.value("control", "") == kExploreGalleryControl) {
            const auto dispatched_frame_revision = scalar(record, "a");
            const bool dispatched = record.value("detail", "") == "real-canvas-pointer" && pending_pointer_frame_revision != 0U &&
                                    dispatched_frame_revision >= pending_pointer_frame_revision &&
                                    surface_draws.contains(scalar(record, "b")) &&
                                    surface_scales.contains(std::pair{scalar(record, "b"), dispatched_frame_revision}) &&
                                    numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0;
            if (dispatched) {
                pointer_dispatched = true;
                pending_pointer_frame_revision = dispatched_frame_revision;
            }
        } else if (event == "integration.explore_pointer_selected") {
            const bool selected = record.value("detail", "") == "selected-from-dispatched-pointer" &&
                                  scalar(record, "a") == pointer_revision && scalar(record, "b") == pointer_slot &&
                                  scalar(record, "c") == pointer_compiled_index && scalar(record, "c") == scalar(record, "d") &&
                                  explore_slots.contains(pointer_revision) && explore_slots.at(pointer_revision).contains(pointer_slot) &&
                                  explore_slots.at(pointer_revision).at(pointer_slot) == pointer_compiled_index &&
                                  observed_frame_revision_for_slots(explore_slots.at(pointer_revision), pending_pointer_frame_revision);
            if (selected) {
                pointer_selected = true;
                pointer_frame_revision = pending_pointer_frame_revision;
            }
        } else if (event == "integration.explore_slot") {
            const auto revision = scalar(record, "a");
            const auto frame_revision = scalar(record, "b");
            const auto slot = scalar(record, "c");
            const auto compiled_index = scalar(record, "d");
            if (revision != 0U && frame_revision != 0U) {
                if (!explore_slots.contains(revision) && explore_slots.size() == kAcceptanceGenerationLimit) {
                    bounds_valid = false;
                    return;
                }
                auto& slots = explore_slots[revision];
                bounds_valid = bounds_valid && revision < (1ULL << 53U) && slot < kAcceptanceSlotLimit &&
                               (!slots.contains(slot) || slots.at(slot) == compiled_index) &&
                               (!explore_slot_frames.contains(revision) || explore_slot_frames.at(revision) == frame_revision);
                if (slot < kAcceptanceSlotLimit) slots.insert_or_assign(slot, compiled_index);
                explore_slot_frames.insert_or_assign(revision, frame_revision);
                atlas_identities = true;
            }
        } else if (event == "integration.upscale_growth") {
            const bool complete_growth =
                record.value("detail", "").ends_with("-four-times") && scalar(record, "c") == scalar(record, "a") * 4U &&
                scalar(record, "d") == scalar(record, "b") * 4U && (scalar(record, "c") > 1500U || scalar(record, "d") > 1125U);
            upscale_growth = upscale_growth || complete_growth;
            if (complete_growth) upscale_modes.insert(record.value("detail", ""));
        } else if (event == "integration.upscale_presentation") {
            const bool complete_presentation = record.value("detail", "") == "complete-four-times-exported-frame" &&
                                               scalar(record, "a") == scalar(record, "c") && scalar(record, "b") == scalar(record, "d") &&
                                               scalar(record, "a") > 1500U && scalar(record, "b") > 0U;
            upscale_presentation = upscale_presentation || complete_presentation;
            if (complete_presentation) upscale_presentations.insert(record.value("control", ""));
        } else if (event == "integration.upscale_completed_pixels") {
            if (record.value("detail", "") == "exact-completed-blue" && scalar(record, "a") > 0U && scalar(record, "b") > 0U &&
                scalar(record, "c") > 0U && scalar(record, "d") >= 32U)
                upscale_completed_pixels.insert(record.value("control", ""));
        } else if (event == "integration.upscale_same_method") {
            if (record.value("detail", "") == "same-completed-result" && scalar(record, "a") > 0U && scalar(record, "b") > 0U &&
                scalar(record, "c") > 0U && scalar(record, "d") == 1U)
                upscale_same_method.insert(record.value("control", ""));
        } else if (event == "integration.upscale_later_frame") {
            upscale_later_frame = record.value("detail", "") == "distinct-imported-frame" && scalar(record, "b") > scalar(record, "a") &&
                                  scalar(record, "c") != 0U && scalar(record, "d") != 0U;
        } else if (event == "integration.annotation_ready") {
            annotation_ready = record.value("detail", "") == "receiver-owned" && scalar(record, "a") != 0U;
        } else if (event == "integration.annotation_tool_observed") {
            annotation_tool = record.value("detail", "") == "typed-tool" && record.value("control", "").starts_with("annotation.tool.") &&
                              scalar(record, "b") > scalar(record, "a");
        } else if (event == "integration.annotation_pointer_observed") {
            annotation_pointer =
                record.value("detail", "") == "typed-interaction" && scalar(record, "b") > scalar(record, "a") && scalar(record, "c") != 0U;
        } else if (event == "integration.surface_draw") {
            const std::uint64_t revision = scalar(record, "a");
            if (revision != 0U && record.value("detail", "") == "draw") surface_draws.insert(revision);
            if (revision != 0U && record.value("detail", "") == "redraw") surface_redraws.insert(revision);
        } else if (event == "integration.surface_draw_ordinal") {
            const std::uint64_t revision = scalar(record, "a");
            if (revision != 0U && scalar(record, "b") != 0U && numeric(record, "d") == 1.0) ++surface_redraw_counts[revision];
        } else if (event == "integration.surface_geometry") {
            if (record.value("detail", "") == "shader-viewport" && surface_geometries.size() < kAcceptanceRecordLimit)
                surface_geometries.push_back({
                    .presentation_revision = scalar(record, "a"),
                    .source_revision = scalar(record, "b"),
                    .width = numeric(record, "c"),
                    .height = numeric(record, "d"),
                });
            else if (record.value("detail", "") == "shader-viewport")
                bounds_valid = false;
        } else if (event == "integration.surface_container") {
            if (record.value("detail", "") == "rendered-contain-container" && surface_containers.size() < kAcceptanceRecordLimit)
                surface_containers.push_back({
                    .presentation_revision = scalar(record, "a"),
                    .source_revision = scalar(record, "b"),
                    .width = numeric(record, "c"),
                    .height = numeric(record, "d"),
                    .control = record.value("control", ""),
                });
            else if (record.value("detail", "") == "rendered-contain-container")
                bounds_valid = false;
        } else if (event == "integration.surface_content") {
            const auto key = std::pair{scalar(record, "a"), scalar(record, "b")};
            if (record.value("detail", "") == "exported-native-frame" &&
                (surface_contents.contains(key) || surface_contents.size() < kAcceptanceRecordLimit))
                surface_contents.insert_or_assign(key, std::pair{scalar(record, "c"), scalar(record, "d")});
            else if (record.value("detail", "") == "exported-native-frame")
                bounds_valid = false;
        } else if (event == "integration.surface_scale") {
            const auto revision = scalar(record, "a");
            const auto source = scalar(record, "b");
            const double scale = numeric(record, "c");
            bounds_valid = bounds_valid && revision != 0U && source != 0U && scale > 0.0 && std::abs(scale - numeric(record, "d")) < 0.0001;
            if (revision != 0U && source != 0U && scale > 0.0) surface_scales.insert_or_assign({revision, source}, scale);
        } else if (event == "integration.complete") {
            const std::uint64_t annotation_revision = scalar(record, "a");
            const std::uint64_t exported_receipt = scalar(record, "b");
            const std::uint64_t frame_receipt = scalar(record, "c");
            const std::uint64_t browser_receipt = scalar(record, "d");
            complete = record.value("detail", "") == "typed-mvc-wayland" && annotation_revision != 0U && exported_receipt != 0U &&
                       exported_receipt == frame_receipt && frame_receipt == browser_receipt;
            presentation_receipt = exported_receipt;
        } else if (event == "integration.failed" || event == "browser.invalid_webgpu_texture") {
            failed = true;
        } else if (event == "firefox.workspace.admitted") {
            firefox_import = true;
        } else if (event == "firefox.workspace.claim_outcome") {
            firefox_claim = record.value("outcome", "") == "claimed";
        } else if (event == "firefox.workspace.ready" || event == "firefox.workspace.import_ready_emitted" ||
                   event == "firefox.workspace.registry_inserted") {
            firefox_ready = true;
        } else if (event == "firefox.workspace.channel_terminal" && record.value("terminal", "") == "protocol_failure") {
            workspace_protocol_failure = true;
        } else if (event.find("panic") != std::string::npos || event == "browser.panic") {
            panic = true;
        }
    }

    [[nodiscard]] std::string_view readiness_blocker() const {
        static const std::array expected{
            "navigation.train",
            "navigation.validate",
            "navigation.predict",
            "navigation.live",
            "navigation.annotate",
            "navigation.export",
            "navigation.explore",
            TRAIN_CARD,
            DATASET_SOURCE,
            COMPILED_DIRECTORY,
            COMPILE_DIMENSIONS,
            COMPILE_RESOLUTION,
            COMPILE_DATASET,
            COMPILE_PROGRESS,
            DATASET_STATUS,
            TRAIN_MODEL_CARD,
            TRAIN_MODEL_PROGRESS,
            TRAIN_MODEL_SELECTOR,
            TRAIN_MODEL_PRESETS,
            TRAIN_MODEL_DIVIDER,
            TRAIN_MODEL_CUSTOM,
            TRAIN_MODEL_STATUS,
            TRAIN_MODEL_ACTION,
            MATCH_FREE_ASSIGNMENT,
            "workflow.setup",
            "workflow.workspace_and_advanced",
            "workflow.workspace",
            "workflow.advanced",
            "workflow.diagnostics",
            EXPLORE_OPEN,
            EXPLORE_DATASET_PANE,
            EXPLORE_DETAILS_PANE,
            EXPLORE_CARD,
            kExploreGalleryControl,
            EXPLORE_LATER,
            EXPLORE_AUGMENTATION_TOGGLE,
            EXPLORE_AUGMENTATION_REROLL,
            EXPLORE_RESHUFFLE,
            EXPLORE_DETAIL_ORIGINAL,
            EXPLORE_DETAIL_FIT,
            EXPLORE_UPSCALE_BASIC,
            EXPLORE_UPSCALE_FAST,
            EXPLORE_UPSCALE_NEURAL,
            EXPLORE_NEXT,
            EXPLORE_PREVIOUS,
            EXPLORE_ANNOTATE,
            ANNOTATION_SURFACE,
            ANNOTATION_SIDEBAR,
            ANNOTATION_TIMELINE,
            ANNOTATION_OPERATION,
            ANNOTATION_STOP,
            ANNOTATION_BRUSH_RADIUS,
            SETTINGS_MODAL,
            ERROR_MODAL,
            ERROR_COPY,
            ERROR_DISMISS,
            "settings.group.appearance",
            "settings.group.typography",
            "settings.group.environment",
            "settings.show_fps",
            BENCHMARK_OVERRIDE,
            "settings.ui_scale",
            "settings.ui_scale.label",
            "settings.ui_scale.value",
            "settings.font_size",
            "settings.font_size.label",
            "settings.font_size.value",
            "settings.secondary_font_size",
            "settings.secondary_font_size.label",
            "settings.secondary_font_size.value",
            "settings.mono_font_size",
            "settings.mono_font_size.label",
            "settings.mono_font_size.value",
            "settings.text_input_font_size",
            "settings.text_input_font_size.label",
            "settings.text_input_font_size.value",
            SETTINGS_FOOTER,
            SETTINGS_RESET,
            SETTINGS_CLOSE,
        };
        static const std::array pages{"Train", "Validate", "Predict", "Live", "Annotate", "Export"};
        static const std::array regions{"workflow.setup", "workflow.workspace_and_advanced", "workflow.workspace", "workflow.advanced",
                                        "workflow.diagnostics"};
        const auto page_bound = [this](const std::string_view page, const std::string_view control) -> const Bounds* {
            const auto found = page_bounds.find(std::string{page} + ":" + std::string{control});
            return found == page_bounds.end() ? nullptr : &found->second;
        };
        const auto immediately_above = [](const Bounds& progress_bounds, const Bounds& action_bounds) {
            const double progress_bottom = progress_bounds.y + progress_bounds.height;
            return progress_bounds.valid() && action_bounds.valid() && progress_bottom <= action_bounds.y + 1.0 &&
                   action_bounds.y - progress_bottom <= 5.0;
        };
        const auto page_prefix = [](const std::string_view page) {
            return page == "Train"      ? "train"
                   : page == "Validate" ? "validate"
                   : page == "Predict"  ? "predict"
                   : page == "Live"     ? "live"
                   : page == "Annotate" ? "annotation"
                                        : "export";
        };
        const auto primary_action = [&page_prefix](const std::string_view page) {
            return std::string{page_prefix(page)} + (page == "Annotate" ? ".save" : ".primary");
        };
        const bool every_region = std::ranges::all_of(pages, [this, &page_prefix, &primary_action](const std::string_view page) {
            const std::string_view prefix = page_prefix(page);
            return std::ranges::all_of(regions,
                                       [this, page](const std::string_view region) {
                                           return page_bounds.contains(std::string{page} + ":" + std::string{region});
                                       }) &&
                   page_bounds.contains(std::string{page} + ":" + primary_action(page)) &&
                   page_bounds.contains(std::string{page} + ":" + prefix + ".status");
        });
        const bool primary_progress_placement =
            std::ranges::all_of(pages, [&page_bound, &immediately_above, &primary_action](const std::string_view page) {
                const std::string action = primary_action(page);
                const Bounds* const progress_bounds = page_bound(page, action + ".progress");
                const Bounds* const action_bounds = page_bound(page, action);
                return progress_bounds != nullptr && action_bounds != nullptr && immediately_above(*progress_bounds, *action_bounds);
            });
        const bool primary_action_geometry = std::ranges::all_of(pages, [&page_bound, &primary_action](const std::string_view page) {
            const std::string action = primary_action(page);
            const Bounds* const action_bounds = page_bound(page, action);
            const Bounds* const setup_bounds = page_bound(page, "workflow.setup");
            return action_bounds != nullptr && setup_bounds != nullptr && std::abs(action_bounds->height - 48.0) < 0.01 &&
                   std::abs(action_bounds->width - (setup_bounds->width - 20.0)) < 1.0;
        });
        const bool compile_progress_placement = immediately_above(compile_progress, compile_action);
        const bool model_progress_placement = model_card.valid() && model_progress.valid() && model_card.contains(model_progress);
        const bool model_composition = [&] {
            if (!std::ranges::all_of(model_parts, [](const Bounds& bounds) { return bounds.valid(); })) return false;
            const auto& [selector, presets, divider, custom, status, action] = model_parts;
            return std::ranges::all_of(model_parts, [this](const Bounds& part) { return model_card.contains(part); }) &&
                   model_card.contains(model_progress) && selector.contains(presets) && selector.contains(divider) &&
                   selector.contains(custom) && std::abs(custom.x - (selector.x + 2.0)) < 1.0 &&
                   std::abs(custom.width - (selector.width - 4.0)) < 1.0 && divider.y >= presets.y + presets.height - 1.0 &&
                   custom.y >= divider.y + divider.height - 1.0 && status.y >= selector.y + selector.height - 1.0 &&
                   model_progress.y >= status.y + status.height - 1.0 && action.y >= model_progress.y + model_progress.height - 1.0;
        }();
        const double page_width = train_setup.width + train_center.width + train_diagnostics.width;
        const bool reference_columns =
            train_setup.valid() && train_center.valid() && train_diagnostics.valid() && train_setup.x < train_center.x &&
            train_center.x < train_diagnostics.x && page_width > 0.0 && std::abs(train_setup.width / page_width - 0.19) < 0.002 &&
            std::abs(train_center.width / page_width - 0.62) < 0.002 && std::abs(train_diagnostics.width / page_width - 0.19) < 0.002;
        const bool vertical_composition =
            train_workspace.valid() && train_advanced.valid() && train_workspace.y + train_workspace.height <= train_advanced.y + 1.0;
        const bool explore_composition = explore_dataset.valid() && explore_gallery.valid() && explore_details.valid() &&
                                         explore_dataset.x < explore_gallery.x && explore_gallery.x < explore_details.x &&
                                         std::abs(explore_dataset.width - 280.0) < 1.0 && std::abs(explore_details.width - 300.0) < 1.0;
        const Bounds* const annotation_workspace = page_bound("Annotate", "workflow.workspace");
        const Bounds* const annotation_diagnostics = page_bound("Annotate", "workflow.diagnostics");
        const bool annotation_composition =
            annotation_workspace != nullptr && annotation_diagnostics != nullptr && annotation_sidebar.valid() &&
            annotation_timeline.valid() && annotation_operation.valid() && annotation_stop.valid() && annotation_brush.valid() &&
            annotation_tool_control.valid() && annotation_diagnostics->contains_horizontally(annotation_sidebar) &&
            annotation_operation.x >= annotation_sidebar.x - 1.0 && annotation_stop.x >= annotation_sidebar.x - 1.0 &&
            annotation_brush.x >= annotation_sidebar.x - 1.0 && annotation_tool_control.x >= annotation_sidebar.x - 1.0 &&
            annotation_diagnostics->contains_horizontally(annotation_timeline) &&
            (annotation_diagnostics->x >= annotation_workspace->x + annotation_workspace->width - 1.0 ||
             (annotation_diagnostics->y >= annotation_workspace->y + annotation_workspace->height - 1.0 &&
              annotation_diagnostics->x >= annotation_workspace->x - 1.0 &&
              annotation_diagnostics->x + annotation_diagnostics->width <= annotation_workspace->x + annotation_workspace->width + 1.0));
        const bool settings_composition =
            settings_modal.valid() && std::abs(settings_modal.width - 520.0) < 0.01 && settings_footer.valid() &&
            settings_appearance.valid() && settings_typography.valid() && settings_show_fps.valid() && settings_environment.valid() &&
            settings_appearance.y < settings_typography.y && settings_show_fps.y >= settings_appearance.y - 1.0 &&
            settings_show_fps.y + settings_show_fps.height <= settings_appearance.y + settings_appearance.height + 1.0 &&
            settings_typography.y < settings_environment.y && settings_environment.y < settings_footer.y && settings_reset.valid() &&
            settings_close.valid() && settings_reset.x < settings_close.x && settings_reset.y >= settings_footer.y - 1.0 &&
            settings_close.y >= settings_footer.y - 1.0 && settings_footer.contains_horizontally(settings_reset) &&
            settings_footer.contains_horizontally(settings_close);
        const bool settings_numeric_alignment = [&] {
            const Bounds& reference_label = settings_numeric_labels.front();
            const Bounds& reference_value = settings_numeric_values.front();
            for (std::size_t index = 0; index < SETTINGS_NUMERIC_CONTROLS.size(); ++index) {
                const Bounds& control = settings_numeric_controls[index];
                const Bounds& label = settings_numeric_labels[index];
                const Bounds& value = settings_numeric_values[index];
                if (!control.valid() || !label.valid() || !value.valid() || std::abs(label.x - reference_label.x) >= 1.0 ||
                    std::abs(label.width - reference_label.width) >= 1.0 || std::abs(value.x - reference_value.x) >= 1.0 ||
                    std::abs(value.width - reference_value.width) >= 1.0 || label.x < control.x - 1.0 ||
                    value.x + value.width > control.x + control.width + 1.0) {
                    return false;
                }
            }
            return true;
        }();
        const auto aligned_grid = [this](const auto& fields) {
            if (!std::ranges::all_of(fields, [](const Bounds& bounds) { return bounds.valid(); })) return false;
            const Bounds& reference = fields.front();
            for (std::size_t index = 0; index < fields.size(); ++index) {
                const Bounds& bounds = fields[index];
                if (std::abs(bounds.y - reference.y) >= 1.0 || std::abs(bounds.width - reference.width) >= 1.0 ||
                    bounds.x < train_advanced.x - 1.0 || bounds.x + bounds.width > train_advanced.x + train_advanced.width + 1.0 ||
                    bounds.y < train_advanced.y - 1.0 || bounds.y + bounds.height > train_advanced.y + train_advanced.height + 1.0 ||
                    (index != 0U && bounds.x < fields[index - 1U].x + fields[index - 1U].width - 1.0)) {
                    return false;
                }
            }
            return true;
        };
        const std::array advanced_general{advanced_fixed[0], advanced_fixed[1], advanced_fixed[2], advanced_fixed[3]};
        const std::array advanced_optimizer{advanced_fixed[4], advanced_fixed[5], advanced_fixed[6], advanced_fixed[7]};
        const bool advanced_composition =
            aligned_grid(advanced_general) && aligned_grid(advanced_optimizer) && aligned_grid(advanced_match_free) &&
            aligned_grid(advanced_denoising) && std::abs(advanced_general.front().width - advanced_optimizer.front().width) < 1.0 &&
            std::abs(advanced_general.front().width - advanced_match_free.front().width) < 1.0 &&
            std::abs(advanced_general.front().width - advanced_denoising.front().width) < 1.0 &&
            advanced_optimizer.front().y >= advanced_general.front().y + advanced_general.front().height - 1.0 &&
            advanced_assignment.valid() && advanced_denoising_toggle.valid() && advanced_assignment.x >= train_advanced.x - 1.0 &&
            advanced_assignment.x + advanced_assignment.width <= train_advanced.x + train_advanced.width + 1.0 &&
            advanced_assignment.y >= train_advanced.y - 1.0 &&
            advanced_assignment.y + advanced_assignment.height <= train_advanced.y + train_advanced.height + 1.0 &&
            advanced_assignment.y >= advanced_optimizer.front().y + advanced_optimizer.front().height - 1.0 &&
            advanced_denoising_toggle.x >= train_advanced.x - 1.0 &&
            advanced_denoising_toggle.x + advanced_denoising_toggle.width <= train_advanced.x + train_advanced.width + 1.0 &&
            advanced_denoising_toggle.y >= train_advanced.y - 1.0 &&
            advanced_denoising_toggle.y + advanced_denoising_toggle.height <= train_advanced.y + train_advanced.height + 1.0 &&
            advanced_match_free.front().y >= advanced_denoising_toggle.y + advanced_denoising_toggle.height - 1.0;
        const bool advanced_compact = [&] {
            const auto compact_row = [](const auto& fields) {
                if (fields.size() < 2U) return false;
                const double cell_pitch = fields[1].x - fields[0].x;
                return cell_pitch > 0.0 &&
                       std::ranges::all_of(fields, [cell_pitch](const Bounds& bounds) { return bounds.width < cell_pitch * 0.70; });
            };
            return compact_row(advanced_general) && compact_row(advanced_optimizer) && compact_row(advanced_match_free) &&
                   compact_row(advanced_denoising);
        }();
        const bool error_composition = error_modal.valid() && error_copy.valid() && error_dismiss.valid() &&
                                       error_copy.x < error_dismiss.x && error_copy.y >= error_modal.y - 1.0 &&
                                       error_dismiss.y >= error_modal.y - 1.0 && error_modal.contains_horizontally(error_copy) &&
                                       error_modal.contains_horizontally(error_dismiss);
        const bool gallery_shader_fill =
            explore_gallery.valid() && std::ranges::any_of(surface_geometries, [this](const auto& geometry) {
                const bool gallery_frame = std::ranges::any_of(
                    explore_slot_frames, [&geometry](const auto& frame) { return frame.second == geometry.source_revision; });
                const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
                if (!gallery_frame || !surface_scales.contains(key)) return false;
                const double scale = surface_scales.at(key);
                return std::abs(geometry.width / scale - explore_gallery.width) < 1.0 && square_atlas_frames.contains(key) &&
                       atlas_scaled_frames.contains(key);
            });
        const bool detail_containers = detail_fit;
        // Automatic Basic may supersede the padded source before the browser's
        // next frame. The exact source transition proves the padded product;
        // require the original crop to have been physically drawn.
        const bool padded_and_original_detail = detail_source && original_detail_drawn;
        static const std::set<std::string, std::less<>> expected_upscale_modes{"basic-four-times", "fast-four-times", "neural-four-times"};
        static const std::set<std::string, std::less<>> expected_upscale_presentations{EXPLORE_UPSCALE_BASIC, EXPLORE_UPSCALE_FAST,
                                                                                       EXPLORE_UPSCALE_NEURAL};
        const bool bounded_exact_grid = exact_grid && exact_grid_revision != 0U && exact_grid_frame_revision != 0U &&
                                        exact_grid_width <= exact_grid_capacity_width && exact_grid_height <= exact_grid_capacity_height &&
                                        explore_slot_frames.contains(exact_grid_revision) &&
                                        explore_slot_frames.at(exact_grid_revision) == exact_grid_frame_revision;
        const bool repeated_same_revision =
            std::ranges::any_of(surface_redraw_counts, [](const auto& draws) { return draws.second >= 3U; });
        const bool pointer_render_chain =
            pointer_frame_revision != 0U && std::ranges::any_of(surface_geometries, [this](const auto& geometry) {
                const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
                return geometry.source_revision == pointer_frame_revision && surface_scales.contains(key) &&
                       surface_draws.contains(geometry.presentation_revision);
            });
        static const std::array expected_drag_stages{"pressed", "moved-1", "moved-2", "released"};
        const bool complete_pointer_drag = std::ranges::all_of(
            expected_drag_stages, [this](const std::string_view stage) { return ui_scale_pointer_stages.contains(stage); });
        static const std::array expected_primary{
            DATASET_BROWSE, COMPILE_DATASET, TRAIN_MODEL_CUSTOM, TRAIN_MODEL_ACTION, EXPLORE_OPEN,
        };
        const bool uniform_primary =
            !shared_primary_colors.empty() && std::ranges::all_of(shared_primary_colors, [this](const auto& style) {
                return style.second == shared_primary_colors.begin()->second;
            });
        return first_failed_check(
            bootstrap && fluent && uniform_primary && benchmark_purple && rendered_controls.contains(BENCHMARK_OVERRIDE) &&
                std::ranges::all_of(
                    expected_primary,
                    [this](const std::string_view id) { return shared_primary.contains(id) && rendered_controls.contains(id); }),
            "shell and style", every_region, "ordinary workflow regions", primary_progress_placement, "primary progress placement",
            primary_action_geometry, "primary action geometry", reference_columns, "workflow column geometry", vertical_composition,
            "workflow vertical composition", advanced_composition, "Advanced composition", advanced_compact, "Advanced compact controls",
            spinnerless_integer, "integer spinner suppression", spinnerless_floating, "floating spinner suppression",
            advanced_integer_persisted, "Advanced integer persistence", advanced_floating_persisted, "Advanced floating persistence",
            compile_progress_placement, "Dataset progress placement", model_progress_placement, "Model progress containment",
            model_composition && model_copy, "Model card composition", benchmark_override.valid() && benchmark_round_trip,
            "benchmark override interaction", explore_composition, "Explore composition", annotation_composition, "annotation composition",
            settings_composition && settings_numeric_alignment && show_fps_round_trip && ui_scale_drag && ui_scale_released &&
                ui_scale_restored && complete_pointer_drag && error_composition && error_modal_usable,
            "Settings composition",
            dataset_configured && progress && compile_metrics && dataset_complete && progress_ordinal < dataset_complete_ordinal,
            "Dataset lifecycle", explore_ready, "ready snapshot", sweep, "viewport sweep", scrolled, "gallery scroll", detail,
            "detail selection", bounded_exact_grid, "bounded exact grid", newest_placeholder, "newest placeholder", pointer_inverse,
            "pointer inverse", pointer_dispatched, "pointer dispatch", pointer_selected, "pointer selection", gallery_shader_fill,
            "gallery shader fill", pointer_render_chain, "pointer render chain", atlas_identities, "atlas identities", augmentation_enabled,
            "augmentation enabled", augmentation_rerolled, "augmentation rerolled", reshuffle_order_only, "reshuffle order only",
            detail_source, "detail source", detail_fit, "detail fit", detail_containers, "detail containers", padded_and_original_detail,
            "padded and original detail", upscale_growth, "upscale growth", upscale_presentation, "upscale presentation",
            upscale_modes == expected_upscale_modes, "upscale modes", upscale_presentations == expected_upscale_presentations,
            "upscale presentations", upscale_completed_pixels == expected_upscale_presentations, "upscale completed blue pixels",
            upscale_same_method == expected_upscale_presentations, "upscale exact re-click", upscale_later_frame, "upscale later frame",
            reopened, "dataset reopen", repeated_same_revision, "same-revision redraw",
            annotation_ready && annotation_tool && annotation_pointer, "annotation lifecycle",
            complete && surface_draws.contains(presentation_receipt) && surface_redraws.contains(presentation_receipt),
            "presentation completion", bounds_valid && !failed_before_termination(), "browser validity",
            firefox_import && firefox_claim && firefox_ready, "Firefox integration",
            std::ranges::all_of(expected, [this](const std::string_view id) { return controls.contains(id); }), "expected controls");
    }

    [[nodiscard]] bool product_ready() const { return readiness_blocker().empty(); }

    [[nodiscard]] bool terminal_evidence_settled() const noexcept {
        return complete && presentation_receipt != 0U && surface_redraw_counts.contains(presentation_receipt) &&
               surface_redraw_counts.at(presentation_receipt) >= 3U;
    }

    [[nodiscard]] bool rendered_frame_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots,
                                                const std::uint64_t source_revision, const std::size_t minimum_redraws) const noexcept {
        for (const auto& [snapshot_revision, slots] : explore_slots) {
            if (slots != native_slots || !explore_slot_frames.contains(snapshot_revision)) continue;
            const auto source = explore_slot_frames.at(snapshot_revision);
            if (source_revision != 0U && source != source_revision) continue;
            for (const auto& geometry : surface_geometries) {
                const auto key = std::pair{geometry.presentation_revision, geometry.source_revision};
                if (geometry.source_revision != source || !surface_scales.contains(key) ||
                    !surface_draws.contains(geometry.presentation_revision) ||
                    (minimum_redraws != 0U && (!surface_redraw_counts.contains(geometry.presentation_revision) ||
                                               surface_redraw_counts.at(geometry.presentation_revision) < minimum_redraws)))
                    continue;
                const double scale = surface_scales.at(key);
                const bool normalized =
                    geometry.width > 0.0 && geometry.height > 0.0 && scale > 0.0 &&
                    (!explore_gallery.valid() || (std::abs(geometry.width / scale - explore_gallery.width) < 1.0 &&
                                                  square_atlas_frames.contains(key) && atlas_scaled_frames.contains(key)));
                if (normalized) return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool observed_frame_revision_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots,
                                                         const std::uint64_t source_revision) const noexcept {
        return source_revision != 0U && std::ranges::any_of(explore_slots, [this, &native_slots, source_revision](const auto& snapshot) {
                   return snapshot.second == native_slots && explore_slot_frames.contains(snapshot.first) &&
                          explore_slot_frames.at(snapshot.first) == source_revision;
               });
    }

    [[nodiscard]] const std::map<std::uint64_t, std::uint64_t>* final_cursor_slots() const noexcept {
        const auto slots = explore_slots.find(final_cursor_revision);
        if (final_cursor_revision == 0U || final_cursor_frame_revision == 0U || final_cursor_generation == 0U ||
            final_cursor_slot_count == 0U || final_cursor_slot_count > kAcceptanceSlotLimit || slots == explore_slots.end() ||
            slots->second.size() != final_cursor_slot_count || !explore_slot_frames.contains(final_cursor_revision) ||
            explore_slot_frames.at(final_cursor_revision) != final_cursor_frame_revision)
            return nullptr;
        for (std::uint64_t slot = 0U; slot != final_cursor_slot_count; ++slot)
            if (!slots->second.contains(slot)) return nullptr;
        return &slots->second;
    }

    [[nodiscard]] const std::map<std::uint64_t, std::uint64_t>* pointer_slots() const noexcept {
        const auto slots = explore_slots.find(pointer_revision);
        return pointer_selected && slots != explore_slots.end() ? &slots->second : nullptr;
    }

    [[nodiscard]] bool failed_before_termination() const noexcept { return failed || panic || workspace_protocol_failure; }

    [[nodiscard]] std::string_view failure_blocker() const noexcept {
        const std::array checks{
            std::pair{failed, std::string_view{"integration failure"}},
            std::pair{panic, std::string_view{"browser panic"}},
            std::pair{workspace_protocol_failure, std::string_view{"workspace protocol failure"}},
        };
        const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return check.first; });
        return blocker == checks.end() ? std::string_view{} : blocker->second;
    }

   private:
    static constexpr const char* TRAIN_CARD = "train.card.dataset";
    static constexpr const char* DATASET_SOURCE = "train.dataset.source";
    static constexpr const char* DATASET_BROWSE = "train.dataset.browse";
    static constexpr const char* COMPILED_DIRECTORY = "train.dataset.compiled_directory";
    static constexpr const char* COMPILE_RESOLUTION = "train.dataset.resolution";
    static constexpr const char* COMPILE_DIMENSIONS = "train.dataset.compile_dimensions";
    static constexpr const char* COMPILE_DATASET = "train.compile_dataset";
    static constexpr const char* COMPILE_PROGRESS = "train.compile_dataset.progress";
    static constexpr const char* DATASET_STATUS = "train.dataset.status";
    static constexpr const char* TRAIN_MODEL_CARD = "train.card.model";
    static constexpr const char* TRAIN_MODEL_PROGRESS = "train.card.model.progress";
    static constexpr const char* TRAIN_MODEL_SELECTOR = "train.model.selector";
    static constexpr const char* TRAIN_MODEL_PRESETS = "train.model.presets";
    static constexpr const char* TRAIN_MODEL_DIVIDER = "train.model.divider";
    static constexpr const char* TRAIN_MODEL_CUSTOM = "train.model.custom_weights";
    static constexpr const char* TRAIN_MODEL_STATUS = "train.model.status";
    static constexpr const char* TRAIN_MODEL_ACTION = "train.model.action";
    static constexpr const char* MATCH_FREE_ASSIGNMENT = "train.advanced.assignment.match_free";
    static constexpr const char* BENCHMARK_OVERRIDE = "train.dataset.benchmark_override";
    static constexpr const char* EXPLORE_OPEN = "explore.open";
    static constexpr const char* EXPLORE_STOP = "explore.stop";
    static constexpr const char* EXPLORE_DATASET_PANE = "explore.pane.dataset_filters";
    static constexpr const char* EXPLORE_DETAILS_PANE = "explore.pane.dataset_details";
    static constexpr const char* EXPLORE_CARD = "explore.card.status";
    static constexpr const char* EXPLORE_LATER = "explore.gallery.later";
    static constexpr const char* EXPLORE_AUGMENTATION_TOGGLE = "explore.gallery.augmentation";
    static constexpr const char* EXPLORE_AUGMENTATION_REROLL = "explore.gallery.augmentation.reroll";
    static constexpr const char* EXPLORE_RESHUFFLE = "explore.gallery.reshuffle";
    static constexpr const char* EXPLORE_DETAIL_ORIGINAL = "explore.detail.source.original";
    static constexpr const char* EXPLORE_DETAIL_FIT = "explore.detail.fit";
    static constexpr const char* EXPLORE_DETAIL_WORKSPACE = "explore.detail.workspace";
    static constexpr const char* EXPLORE_UPSCALE_BASIC = "explore.detail.upscale.basic";
    static constexpr const char* EXPLORE_UPSCALE_FAST = "explore.detail.upscale.fast";
    static constexpr const char* EXPLORE_UPSCALE_NEURAL = "explore.detail.upscale.neural";
    static constexpr const char* EXPLORE_NEXT = "explore.detail.next";
    static constexpr const char* EXPLORE_PREVIOUS = "explore.detail.previous";
    static constexpr const char* EXPLORE_ANNOTATE = "explore.detail.open_annotation";
    static constexpr const char* ANNOTATION_SURFACE = "annotation.workspace.surface";
    static constexpr const char* ANNOTATION_SIDEBAR = "annotation.sidebar";
    static constexpr const char* ANNOTATION_TIMELINE = "annotation.timeline";
    static constexpr const char* ANNOTATION_OPERATION = "annotation.operation";
    static constexpr const char* ANNOTATION_STOP = "annotation.stop";
    static constexpr const char* ANNOTATION_BRUSH_RADIUS = "annotation.brush_radius";
    static constexpr const char* SETTINGS_MODAL = "settings.modal";
    static constexpr const char* ERROR_MODAL = "error.modal";
    static constexpr const char* ERROR_COPY = "error.copy";
    static constexpr const char* ERROR_DISMISS = "error.dismiss";
    static constexpr const char* SETTINGS_FOOTER = "settings.footer";
    static constexpr const char* SETTINGS_RESET = "settings.reset";
    static constexpr const char* SETTINGS_CLOSE = "settings.close";
    static constexpr std::array<std::string_view, 5> SETTINGS_NUMERIC_CONTROLS{
        "settings.ui_scale",
        "settings.font_size",
        "settings.secondary_font_size",
        "settings.mono_font_size",
        "settings.text_input_font_size",
    };
};

class JsonLineCursor final {
   public:
    enum class Format { NativeJson, FirefoxText };
    JsonLineCursor(std::filesystem::path path, const std::uintmax_t offset, Format format = Format::NativeJson)
        : path_(std::move(path)), offset_(offset), format_(format) {}

    template <class Audit, class Observer>
    void consume(Audit& audit, Observer&& observer, const bool final = false) {
        std::ifstream input{path_, std::ios::binary | std::ios::ate};
        if (!input) throw std::runtime_error("evidence artifact unavailable: " + path_.string());
        const auto end = input.tellg();
        if (end < 0 || static_cast<std::uintmax_t>(end) < offset_)
            throw std::runtime_error("evidence artifact truncated: " + path_.string());
        input.seekg(static_cast<std::streamoff>(offset_));
        std::array<char, 16U * 1024U> chunk{};
        // Bound each allocation and consume one captured extent. Later appends
        // remain for the next notification; they cannot prolong this pass.
        const auto captured_end = static_cast<std::uintmax_t>(end);
        while (offset_ < captured_end) {
            const auto count = std::min<std::uintmax_t>(chunk.size(), captured_end - offset_);
            if (!input.read(chunk.data(), static_cast<std::streamsize>(count)))
                throw std::runtime_error("evidence transport read failed: " + path_.string());
            offset_ += count;
            for (const char byte : std::string_view{chunk.data(), static_cast<std::size_t>(count)}) {
                if (byte != '\n') {
                    if (pending_.size() == kLineLimit) throw std::runtime_error("evidence line exceeded capacity: " + path_.string());
                    pending_.push_back(byte);
                    continue;
                }
                consume_line(audit, observer);
                pending_.clear();
            }
        }
        if (final && format_ == Format::FirefoxText && !pending_.empty()) {
            consume_line(audit, observer);
            pending_.clear();
        }
    }

    void finish() const {
        if (format_ == Format::NativeJson && !pending_.empty())
            throw std::runtime_error("native evidence ended in an incomplete record: " + path_.string());
    }

   private:
    template <class Audit, class Observer>
    void consume_line(Audit& audit, Observer& observer) {
        const std::string_view line{pending_};
        if (format_ == Format::FirefoxText &&
            ((line.contains("Uncaptured WebGPU error: Texture") && line.contains("is invalid")) || line.contains("XPCOMGlueLoad error") ||
             line.contains("Couldn't load XPCOM") || line.contains("panicked at"))) {
            const nlohmann::json failure{{"event", "integration.failed"}, {"detail", std::string{line}}};
            audit.consume(failure);
            observer(failure);
        }
        const auto start = format_ == Format::NativeJson ? 0U : line.find('{');
        if (start == std::string_view::npos) return;
        const auto record = nlohmann::json::parse(line.substr(start), nullptr, false);
        if (!record.is_object()) {
            if (format_ == Format::NativeJson) throw std::runtime_error("malformed native JSONL evidence: " + path_.string());
            return;
        }
        audit.consume(record);
        observer(record);
    }

    static constexpr std::size_t kLineLimit = 64U * 1024U;
    std::filesystem::path path_;
    std::uintmax_t offset_;
    Format format_;
    std::string pending_;
};

TEST_CASE("acceptance artifacts have independent writers and one process-family archive identity", "[workspace][audit]") {
    ScopedTempDir temporary{"mmltk-evidence-ownership"};
    const auto native = temporary.path() / "capture.jsonl";
    const auto parent = artifact_sibling(native, "-acceptance.jsonl");
    const auto application = artifact_sibling(native, "-application.log");
    const auto mozilla = artifact_sibling(native, "-mozilla-child.12.log.child-4.moz_log.0");
    const auto mozilla_parent = artifact_sibling(native, "-mozilla-main.11.log.moz_log.3");
    const auto adjacent = temporary.path() / "capture-other-mozilla-child.13.log.child-5.moz_log.0";
    {
        std::ofstream output{adjacent};
        output << "adjacent capture\n";
    }
    for (const auto& path : {native, parent, application, mozilla, mozilla_parent}) {
        std::ofstream output{path};
        output << "{\"event\":\"previous\"}\n";
    }
    const std::array independent{native, parent, application, mozilla, mozilla_parent};
    require_independent_artifacts(independent);
    const auto alias = temporary.path() / "alias.jsonl";
    std::filesystem::create_hard_link(native, alias);
    const std::array competing{native, alias};
    CHECK_THROWS_AS(require_independent_artifacts(competing), std::runtime_error);
    std::filesystem::remove(alias);
    prepare_latest_log(native, "native", "17-123");
    prepare_latest_log(parent, "acceptance", "17-123");
    rotate_process_log_family(native, "17-123");
    for (const auto& path : {native, parent, application, mozilla, mozilla_parent}) {
        const auto archive =
            std::filesystem::path{path.string() + ".history"} / (path.extension() == ".jsonl" ? "17-123.jsonl" : "17-123.log");
        CHECK(read_tail(archive) == "{\"event\":\"previous\"}\n");
    }
    CHECK_FALSE(std::filesystem::exists(application));
    CHECK_FALSE(std::filesystem::exists(mozilla));
    CHECK_FALSE(std::filesystem::exists(mozilla_parent));
    CHECK(read_tail(adjacent) == "adjacent capture\n");
    append_acceptance_record(parent, {{"event", "acceptance.complete"}});
    CHECK(std::filesystem::file_size(native) == 0U);
    CHECK(read_tail(parent).contains("acceptance.complete"));
}

TEST_CASE("native evidence cursors reject malformed truncated oversized and incomplete records", "[workspace][audit]") {
    struct Count final {
        std::size_t records = 0U;
        void consume(const nlohmann::json&) { ++records; }
    };
    ScopedTempDir temporary{"mmltk-evidence-cursor"};
    const auto path = temporary.path() / "native.jsonl";
    const auto observer = [](const auto&) {};
    for (const std::string& content :
         {std::string{"not JSON\n"}, std::string{"[]\n"}, std::string{"\n"}, std::string(64U * 1024U + 1U, 'x')}) {
        {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            output << content;
        }
        Count audit;
        JsonLineCursor cursor{path, 0U};
        CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
        CHECK(audit.records == 0U);
    }
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output << "{\"event\":\"partial";
    }
    Count audit;
    JsonLineCursor cursor{path, 0U};
    cursor.consume(audit, observer);
    CHECK(audit.records == 0U);
    CHECK_THROWS_AS(cursor.finish(), std::runtime_error);
    {
        std::ofstream output{path, std::ios::binary | std::ios::app};
        output << "\"}\n";
    }
    cursor.consume(audit, observer);
    cursor.finish();
    CHECK(audit.records == 1U);
    { std::ofstream output{path, std::ios::binary | std::ios::trunc}; }
    CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
    std::filesystem::remove(path);
    CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
}

TEST_CASE("evidence reads cross chunk boundaries and failure tails select actual final bytes", "[workspace][audit]") {
    struct Count final {
        std::size_t records = 0U;
        void consume(const nlohmann::json&) { ++records; }
    };
    ScopedTempDir temporary{"mmltk-evidence-chunks"};
    const auto path = temporary.path() / "native.jsonl";
    constexpr std::size_t count = 12000U;
    {
        std::ofstream output{path};
        for (std::size_t index = 0U; index != count; ++index)
            output << "{\"event\":\"chunk\"}\n";
    }
    Count audit;
    JsonLineCursor cursor{path, 0U};
    cursor.consume(audit, [](const auto&) {});
    cursor.finish();
    CHECK(audit.records == count);
    {
        std::ofstream output{path, std::ios::app};
        output << "FINAL TAIL\n";
    }
    const auto tail = read_tail(path);
    CHECK(tail.size() == 96U * 1024U);
    CHECK(tail.ends_with("FINAL TAIL\n"));
    const auto firefox = temporary.path() / "firefox.log";
    {
        std::ofstream output{firefox};
        output << "intentional Firefox text\nprefix {\"event\":\"browser\"}\n";
    }
    Count browser;
    JsonLineCursor browser_cursor{firefox, 0U, JsonLineCursor::Format::FirefoxText};
    browser_cursor.consume(browser, [](const auto&) {});
    CHECK(browser.records == 1U);
    const std::array failures{
        "Uncaptured WebGPU error: Texture TextureId(1,1) is invalid",
        "XPCOMGlueLoad error: dependency unavailable",
        "Couldn't load XPCOM",
        "thread 'main' panicked at failure",
        "{\"event\":\"integration.failed\"}",
        "{\"event\":\"browser.panic\"}",
        "{\"event\":\"browser.invalid_webgpu_texture\"}",
        "{\"event\":\"firefox.workspace.channel_terminal\",\"terminal\":\"protocol_failure\"}",
    };
    for (const bool viewer : {false, true}) {
        const nlohmann::json completion{
            {"event", viewer ? "integration.viewer_complete" : "integration.complete"},
            {"detail", viewer ? "square" : "typed-mvc-wayland"},
            {"a", 1U},
            {"b", 1U},
            {"c", 1U},
            {"d", 1U},
        };
        for (const auto* failure : failures) {
            for (const std::string_view order : {"failure-first", "completion-first", "final-drain", "older-than-tail"}) {
                INFO("viewer: " << viewer << ", failure: " << failure << ", order: " << order);
                {
                    std::ofstream output{firefox, std::ios::trunc};
                    if (order == "failure-first" || order == "older-than-tail") output << failure << '\n';
                    if (order == "older-than-tail")
                        for (std::size_t index = 0U; index != count; ++index)
                            output << "Firefox ordinary text\n";
                    output << completion.dump() << '\n';
                    if (order == "completion-first") output << failure << '\n';
                }
                BrowserAudit evidence;
                JsonLineCursor evidence_cursor{firefox, 0U, JsonLineCursor::Format::FirefoxText};
                evidence_cursor.consume(evidence, [](const auto&) {});
                CHECK((viewer ? evidence.viewer_complete : evidence.complete));
                if (order == "final-drain") {
                    CHECK_FALSE(evidence.failed_before_termination());
                    {
                        std::ofstream output{firefox, std::ios::app};
                        output << failure;
                    }
                    // Final Firefox text need not have a newline. Ordinary reads
                    // retain it until terminal settlement supplies the last extent.
                    evidence_cursor.consume(evidence, [](const auto&) {});
                    CHECK_FALSE(evidence.failed_before_termination());
                    evidence_cursor.consume(evidence, [](const auto&) {}, true);
                }
                if (order == "older-than-tail") CHECK_FALSE(read_tail(firefox).contains(failure));
                // Both readiness exits and terminal settlement use this owner,
                // independently of their ordinary/viewer completion evidence.
                CHECK(evidence.failed_before_termination());
                CHECK_FALSE(evidence.failure_blocker().empty());
                evidence_cursor.consume(evidence, [](const auto&) {}, true);
                CHECK(evidence.failed_before_termination());
            }
        }
    }
}

void report_consumed_record(const nlohmann::json& record, const std::string_view source) {
    const std::string event = record.value("event", "");
    const bool protocol_failure =
        event.find("protocol") != std::string::npos ||
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
    if (std::ranges::contains(milestones, event)) {
        std::cout << "workspace-wayland[" << source << "]: " << record.dump() << '\n' << std::flush;
    }
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
        do {
            ready = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (ready < 0 && errno == EINTR);
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

class PreparedWaylandInputs final {
   public:
    PreparedWaylandInputs()
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
            dropped_instance
                << R"({"class":"person","bbox_xyxy":[1,0,2,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"1:1","image_size_wh":[)"
                << fixture->width << ',' << fixture->height << "]}\n";
        }
        replace_synthetic_image(square_, 1, 384, 384);
        // This small prerequisite is compiled once. The primary browser owns
        // the one real 512-pixel compile/control/error workflow.
        compile_wayland_fixture(square_, 384U);
    }
    [[nodiscard]] const auto& square() const noexcept { return square_; }
    [[nodiscard]] const auto& mixed() const noexcept { return mixed_; }
    [[nodiscard]] const auto& probe() {
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

   private:
    ScopedTempDir root_;
    mmltk::backend::data::testsupport::FixtureSpec square_;
    mmltk::backend::data::testsupport::FixtureSpec mixed_;
    mmltk::backend::data::testsupport::FixtureSpec probe_;
};

// One owner retains the process, resources, physical evidence and byte cursors.
// RunScenario contains domain assertions; Advance is acknowledged only after
// typed UI settlement and those independent evidence streams have joined.
class WaylandSession final {
   public:
    WaylandSession(std::shared_ptr<PreparedWaylandInputs> inputs, TerminationMode terminal, std::string profile,
                   bool diagnostics_enabled = true, bool dpi = false, bool host_to_device = true, std::string fault = {},
                   bool pixels_enabled = true, const mmltk::backend::data::testsupport::FixtureSpec* ordinary_fixture = nullptr);
    void RunScenario(const std::string& viewer_scenario, bool last, bool dark = false, bool pending_reconstruction = false);

   private:
    void AdvanceScenario();
    std::shared_ptr<PreparedWaylandInputs> inputs_;
    const mmltk::backend::data::testsupport::FixtureSpec& ordinary_fixture_;
    TerminationMode termination;
    std::string profile_;
    bool logging;
    bool high_dpi;
    bool h2d;
    bool pixel_probes;
    bool pixel_fixture;
    std::string probe_failure;
    ScopedTempDir quiet_artifacts{"mmltk-wayland-quiet-artifacts"};
    ScopedTempDir working{"mmltk-workspace-wayland-session"};
    std::filesystem::path ordinary_compiled_;
    std::filesystem::path diagnostics;
    std::filesystem::path runtime_log;
    std::filesystem::path firefox_log;
    std::filesystem::path acceptance_log;
    std::unique_ptr<ArtifactNotifications> notifications_;
    std::unique_ptr<BrowserHostProcess> process_;
    ScopedFd deadline;
    NativeAudit native;
    BrowserAudit browser;
    SurfaceAudit surface_audit;
    PixelBoundaryAudit pixel_audit;
    std::unique_ptr<JsonLineCursor> native_cursor_;
    std::unique_ptr<JsonLineCursor> browser_cursor_;
    std::uint64_t scenario_sequence_ = 1U;
    bool frontend_settled_ = false;
    bool pressure_entered_ = false;
    bool browser_failed_ = false;
};

WaylandSession::WaylandSession(std::shared_ptr<PreparedWaylandInputs> inputs, const TerminationMode terminal, std::string profile,
                               const bool diagnostics_enabled, const bool dpi, const bool host_to_device, std::string fault,
                               const bool pixels_enabled, const mmltk::backend::data::testsupport::FixtureSpec* ordinary_fixture)
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
      ordinary_compiled_(profile_ == "retained"
                             ? working.path() / "compiled"
                             : std::filesystem::path{mmltk::backend::data::testsupport::compiled_dir(ordinary_fixture_)}),
      pixel_audit(pixel_probes) {
    diagnostics = !logging ? quiet_artifacts.path() / "native.jsonl"
                           : configured_path("MMLTK_GUI_TRACE_FILE", latest_wayland_artifact("latest-wayland-test.jsonl"));
    runtime_log = !logging ? quiet_artifacts.path() / "runtime.log"
                           : configured_path("MMLTK_LOG_FILE", latest_wayland_artifact("latest-wayland-test-native.log"));
    firefox_log = !logging ? quiet_artifacts.path() / "firefox.log"
                           : configured_path("MMLTK_FIREFOX_LOG_FILE", latest_wayland_artifact("latest-wayland-test-firefox.log"));
    acceptance_log = artifact_sibling(diagnostics, "-acceptance.jsonl");
    const auto rotation_id = std::to_string(::getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (logging) {
        const std::array owned_artifacts{diagnostics, acceptance_log, runtime_log, firefox_log,
                                         artifact_sibling(diagnostics, "-application.log")};
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
    std::filesystem::create_directories(working.path() / ".mmltk-data");
    std::ofstream settings_file{working.path() / ".mmltk-data" / "gui.json"};
    REQUIRE(settings_file);
    settings_file << mmltk::controller::contracts::snapshot_gui_settings(initial_settings).dump();
    settings_file.close();
    notifications_ = std::make_unique<ArtifactNotifications>(diagnostics, firefox_log);
    process_ =
        std::make_unique<BrowserHostProcess>(MMLTK_TEST_MMLTK_GUI_LAUNCHER, diagnostics, runtime_log, firefox_log, working.path(),
                                             termination, ordinary_fixture_, ordinary_compiled_, profile_ == "blocked" ? "quiet" : profile_,
                                             logging, high_dpi, h2d, pixel_probes, probe_failure, &inputs_->square());
    deadline.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
    if (deadline.get() < 0)
        throw std::runtime_error(std::string{"failed to create workspace Wayland acceptance deadline: "} + std::strerror(errno));
    native_cursor_ = std::make_unique<JsonLineCursor>(diagnostics, 0U);
    browser_cursor_ = std::make_unique<JsonLineCursor>(firefox_log, 0U, JsonLineCursor::Format::FirefoxText);
}

void WaylandSession::RunScenario(const std::string& viewer_scenario, const bool last, const bool dark, const bool pending_reconstruction) {
    const auto permitted_cpus = permitted_cpu_count();
    const auto& fixture = viewer_scenario == "square" ? inputs_->square() : ordinary_fixture_;
    const auto compiled_directory =
        viewer_scenario == "square" ? std::filesystem::path{mmltk::backend::data::testsupport::compiled_dir(fixture)} : ordinary_compiled_;
    auto& process = *process_;
    auto& notifications = *notifications_;
    auto& native_cursor = *native_cursor_;
    auto& browser_cursor = *browser_cursor_;
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

    if (!logging) {
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
            do {
                ready = ::poll(waits.data(), waits.size(), -1);
            } while (ready < 0 && errno == EINTR);
            REQUIRE(ready > 0);
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
                    FAIL("quiet integration failed after progress " << progress << ", failure receipt " << entered->compiled_index
                                                                    << ", frontend source line " << entered->staging_bytes
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
                    arm_acceptance_deadline((progress & 3U) == 2U ? kWaylandWorkDeadline : kWaylandInteractionDeadline,
                                            "quiet typed workflow progress");
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
        CHECK_FALSE(std::filesystem::exists(diagnostics));
        CHECK_FALSE(std::filesystem::exists(firefox_log));
        CHECK_FALSE(std::filesystem::exists(artifact_sibling(diagnostics, "-application.log")));
        INFO("quiet runtime output: " << read_tail(runtime_log));
        CHECK(std::filesystem::file_size(runtime_log) == 0U);
        return;
    }

    const auto consume_records = [&](const bool final = false) {
        native_cursor.consume(native, [&](const auto& record) {
            surface_audit.native(record);
            pixel_audit.consume(record);
            report_consumed_record(record, "native");
        });
        if (native.firefox_pid > 0) process.retain_peer(native.firefox_pid);
        browser_cursor.consume(
            browser,
            [&](const auto& record) {
                surface_audit.browser(record);
                pixel_audit.consume(record);
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
    };
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
            std::cout << "workspace-wayland: progress " << browser.phase_progress_name << " (phase " << observed_phase_progress << ")\n"
                      << std::flush;
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
            return native.padding_orientations.contains(publication.first) &&
                   native.padding_orientations.at(publication.first) == orientation && native.aligned_rendered_probe(publication.first) &&
                   slots != native.placeholder_slots.end() && std::ranges::any_of(publication.second, [&](const auto frame_revision) {
                       return browser.rendered_frame_for_slots(slots->second, frame_revision, 0U);
                   });
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
    const auto acceptance_blockers = [&] {
        std::vector<std::string> blockers;
        const auto add = [&blockers](const std::string_view owner, const std::string_view blocker) {
            if (!blocker.empty()) blockers.emplace_back(std::string{owner} + ": " + std::string{blocker});
        };
        add("surface", surface_audit.failure);
        add("pixel", pixel_audit.failure);
        add("native", native.readiness_blocker(final_generations, seeded_augmentation_ready(), pixel_fixture));
        add("browser", browser.readiness_blocker());
        if (pixel_fixture) {
            if (!rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical))
                blockers.emplace_back("rendered probe: vertical padding export");
            if (!rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal))
                blockers.emplace_back("rendered probe: horizontal padding export");
        }
        if (pixel_probes && !pixel_audit.raw_complete()) blockers.emplace_back("pixel: physical 25-sample chain");
        if (pixel_probes && !pixel_audit.viewer_nonblack_complete()) blockers.emplace_back("pixel: nonblack viewer");
        if (pixel_probes && viewer_scenario == "square" && !pixel_audit.retained_logical_content)
            blockers.emplace_back("pixel: retained logical content");
        if (pixel_probes && viewer_scenario == "square" && !pixel_audit.upscale_growth)
            blockers.emplace_back("pixel: four-times allocation growth");
        if (!pixel_audit.probe_failure_complete(probe_failure)) blockers.emplace_back("pixel: injected probe-failure recovery");
        if (!frontend_settled_) blockers.emplace_back("frontend: typed scenario settlement");
        if (!surface_audit.evidence_settled()) blockers.emplace_back("surface: independent physical lifecycle joins");
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
    const auto terminal_annotation_ready = [&] {
        return viewer_scenario != "terminal" || (native.annotation_copied && native.annotation_opened && native.annotation_edited &&
                                                 browser.annotation_ready && browser.annotation_tool && browser.annotation_pointer &&
                                                 browser.complete && browser.surface_draws.contains(browser.presentation_receipt) &&
                                                 browser.surface_redraws.contains(browser.presentation_receipt));
    };
    bool readiness_reached = false;
    for (;;) {
        consume_records();
        if (!terminal_failure_observed) static_cast<void>(refresh_progress_deadline());
        if (!terminal_failure_observed && frontend_settled_ && !browser_failed_ && !viewer_scenario.empty() && browser.viewer_complete &&
            browser.surface_draws.contains(browser.viewer_presentation) && native.presentation_ready && native.explore_rendered &&
            surface_audit.failure.empty() && pixel_audit.failure.empty() &&
            (!pixel_probes || (pixel_audit.raw_complete() && pixel_audit.viewer_nonblack_complete())) &&
            pixel_audit.probe_failure_complete(probe_failure) && surface_audit.evidence_settled() && terminal_annotation_ready() &&
            (!pixel_fixture || pixel_audit.composition_complete()) &&
            ((viewer_scenario != "copy" && viewer_scenario != "semantics") || pixel_audit.continuity_complete(false)) &&
            (!pending_reconstruction || surface_audit.pending_supersession_completed()) &&
            (viewer_scenario != "square" || !pixel_probes || (pixel_audit.retained_logical_content && pixel_audit.upscale_growth))) {
            readiness_reached = true;
            break;
        }
        if (const auto* slots = browser.final_cursor_slots()) {
            if (const auto identified =
                    native.final_generations_for(*slots, browser.final_cursor_generation, browser.final_cursor_frame_revision))
                final_generations = *identified;
        }
        const auto recovery = pixel_audit.probe_failure_evidence(probe_failure);
        if (recover_before_reads && !recovery_redraw_requested && recovery.forwarded) {
            command_explore(16U, "requested exact-content probe recovery");
            recovery_redraw_requested = true;
        }
        const bool reads_releasable = !recover_before_reads || (recovery_redraw_requested && recovery.complete());
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
            native.explore_generation > released_placeholder_generation &&
            native.placeholder_cardinalities.contains(native.explore_generation)) {
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
                                           native.product_completed(final_generations, seeded_augmentation_ready(), pixel_fixture) &&
                                           !native.active_peer();
        const bool terminal_failure = (native.failed_before_termination() && !expected_window_close) ||
                                      browser.failed_before_termination() || !surface_audit.failure.empty() || !pixel_audit.failure.empty();
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
            (native.product_ready(final_generations, seeded_augmentation_ready(), pixel_fixture) || expected_window_close) &&
            browser.product_ready() && rendered_probe_exported &&
            (!pixel_probes || (pixel_audit.raw_complete() && pixel_audit.viewer_nonblack_complete())) &&
            pixel_audit.probe_failure_complete(probe_failure) && surface_audit.evidence_settled() &&
            (!pixel_fixture || pixel_audit.composition_complete()) && pixel_audit.continuity_complete(true) &&
            (!pending_reconstruction || surface_audit.pending_supersession_completed())) {
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
        do {
            ready = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (ready < 0 && errno == EINTR);
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
            consume_records();
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
            if (event->event == ExploreAcceptanceGate::ControlEvent::VisibleReadHeld) {
                REQUIRE(visible_read_armed);
                REQUIRE_FALSE(visible_read_held.has_value());
                visible_read_held = *event;
                if (logging)
                    append_acceptance_record(acceptance_log, {{"event", "acceptance.visible_read.held"},
                                                              {"generation", event->generation},
                                                              {"compiled_index", event->compiled_index}});
                continue;
            }
            if (event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld ||
                event->event == ExploreAcceptanceGate::ControlEvent::NativeCapacityAvailable) {
                REQUIRE(capacity_armed);
                REQUIRE((event->source_high != 0U || event->source_low != 0U));
                REQUIRE(event->transfer != 0U);
                REQUIRE(event->publication != 0U);
                if (logging)
                    append_acceptance_record(acceptance_log,
                                             {{"event", event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld
                                                            ? "acceptance.native_completion.held"
                                                            : "acceptance.native_capacity.available"},
                                              {"workspace_source_high", event->source_high},
                                              {"workspace_source_low", event->source_low},
                                              {"transfer_sequence", event->transfer},
                                              {"presentation_revision", event->publication}});
                if (event->event == ExploreAcceptanceGate::ControlEvent::NativeCompletionHeld) {
                    REQUIRE_FALSE(capacity_held.has_value());
                    capacity_held = *event;
                    command_explore(static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlCommand::ReleaseSample),
                                    "release one exact browser sample after native completion is held");
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
                const auto control_event =
                    event->event == ExploreAcceptanceGate::ControlEvent::HeldWait      ? "acceptance.control.held_wait"
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
            consume_records();
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
                                               native.product_completed(final_generations, seeded_augmentation_ready(), pixel_fixture) &&
                                               !native.active_peer();
            exited_early =
                !((native.product_ready(final_generations, seeded_augmentation_ready(), pixel_fixture) || terminal_window_close) &&
                  browser.product_ready());
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
        consume_records();
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
        consume_records(true);
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
        for (const auto joined : pixel_audit.joined)
            CHECK(joined != 0U);
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
    if (viewer_scenario == "copy" || viewer_scenario == "semantics" || viewer_scenario.empty())
        CHECK(pixel_audit.continuity_complete(viewer_scenario.empty()));
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
            CHECK((browser.atlas_draws.stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "end", "restored"}));
            CHECK(browser.atlas_draws.drawn_rows.contains(1U));
            CHECK(browser.atlas_draws.drawn_rows.contains(2U));
            CHECK(browser.atlas_draws.drawn_rows.contains(10U));
            CHECK(browser.atlas_draws.grid_round_trip);
            CHECK(browser.atlas_geometry_valid);
            CHECK_FALSE(browser.atlas_scaled_frames.empty());
            CHECK(browser.atlas_native_capacity);
            CHECK(browser.atlas_draws.return_round_trip);
            if (profile_ == "retained") {
                REQUIRE(visible_read_held.has_value());
                REQUIRE(visible_read_released);
                REQUIRE(browser.pending_focus_index == visible_read_held->compiled_index);
                REQUIRE(browser.pending_selection_index == visible_read_held->compiled_index);
                REQUIRE(browser.pending_read_generation == visible_read_held->generation);
                REQUIRE(browser.atlas_draws.away_return);
                REQUIRE(native.admission_seen);
                REQUIRE(native.admission_priority_valid);
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
                }
                REQUIRE(capacity_held.has_value());
                REQUIRE(capacity_available);
                const auto source = SurfaceAudit::native_identity(
                    {{"surface_high", capacity_held->source_high}, {"surface_low", capacity_held->source_low}});
                bool exact_retry = false;
                for (const auto& [arena, state] : surface_audit.surfaces) {
                    const auto released = state.releases.find({capacity_held->publication, capacity_held->transfer});
                    const auto copied = state.receipts.find(capacity_held->publication);
                    if (released == state.releases.end() || copied == state.receipts.end()) continue;
                    const auto first = state.transfers.find({capacity_held->publication, capacity_held->transfer});
                    const auto retry = state.transfers.find({capacity_held->publication, copied->second.transfer});
                    exact_retry = released->second.source == source && copied->second.source == source &&
                                  copied->second.transfer == capacity_held->transfer + 1U && copied->second.stage == 3U &&
                                  first != state.transfers.end() && retry != state.transfers.end() &&
                                  first->second.read == retry->second.read &&
                                  browser.capacity_retry_publication == capacity_held->publication &&
                                  browser.capacity_retry_frame == copied->second.frame;
                    if (exact_retry) break;
                }
                CHECK(exact_retry);
                if (logging) {
                    std::uint64_t workspace_bytes = 0U, workspaces = 0U, arenas = 0U, copies = 0U, release_only = 0U;
                    std::uint64_t encodings = 0U, settlements = 0U, abandonments = 0U, reader_releases = 0U;
                    for (const auto& [id, allocation] : surface_audit.sources) {
                        if (!allocation.native.retired()) {
                            ++workspaces;
                            workspace_bytes += allocation.bytes;
                        }
                    }
                    for (const auto& [id, allocation] : surface_audit.surfaces) {
                        if (allocation.created != 0U && allocation.retired == 0U) ++arenas;
                        copies +=
                            std::ranges::count_if(allocation.receipts, [](const auto& receipt) { return receipt.second.stage == 3U; });
                        release_only += allocation.releases.size();
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
                                                              {"release_only_transfers", release_only},
                                                              {"encoded_draws", encodings},
                                                              {"settled_draws", settlements},
                                                              {"abandoned_draws", abandonments},
                                                              {"final_reader_releases", reader_releases},
                                                              {"explore_peak_pinned_bytes", native.explore_max_pinned}});
                }
            }
            CHECK((browser.atlas_notices == std::set<std::string>{"explore.gallery.capacity", "explore.gallery.empty"}));
            CHECK((browser.atlas_window_draws == std::set<std::string>{"fullscreen", "restored"}));
            CHECK(browser.atlas_themes.contains(dark ? "dark" : "light"));
            CHECK(browser.atlas_device_scales.contains(high_dpi ? 1.5 : 1.0));
            CHECK((browser.atlas_visibility_modes == std::set<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7}));
            CHECK((browser.atlas_scroll_stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "end", "restored"}));
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
                CHECK(std::ranges::any_of(browser.viewer_label_products, [&](const auto& product) {
                    return product.first == kind && browser.surface_draws.contains(product.second);
                }));
            }
        }
        if (viewer_scenario == "copy") {
            CHECK(browser.upscale_completed_pixels.size() == 3U);
            CHECK(browser.upscale_same_method.size() == 3U);
            CHECK((browser.annotation_shapes == std::set<std::string>{"Point", "Spline", "Skeleton"}));
            CHECK(browser.annotation_pixels_valid);
            CHECK(browser.annotation_layout_valid);
            CHECK((browser.annotation_layouts == std::set<std::string>{"compact", "wide"}));
            CHECK((browser.annotation_capabilities == std::set<std::string>{"enabled", "disabled"}));
            CHECK(browser.annotation_swatches == 2U);
            CHECK(browser.annotation_previews == 2U);
            CHECK_FALSE(browser.annotation_pixel_frames.empty());
            for (const auto& [source, presentation] : browser.annotation_pixel_frames) {
                CHECK(source != 0U);
                CHECK(browser.surface_draws.contains(presentation));
            }
            for (const auto operation : {"color-sample", "mask-fill", "move-Mask", "resize-mask", "paint-mask", "erase-mask", "move-Point",
                                         "move-Spline", "move-Skeleton", "spline-handle", "singleton-spline", "reclassify", "undo-class",
                                         "redo-class", "cancel-preview", "create-Box", "create-Point", "create-Skeleton"})
                CHECK(browser.annotation_product_operations[operation] >= 2U);
            for (const auto tool : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationTool>())
                CHECK(browser.annotation_product_operations["tool-" + std::string{tool.name}] >= 2U);
            for (const auto operation : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationMaskCleanup>())
                CHECK(browser.annotation_product_operations["cleanup-" + std::string{operation.name}] >= 2U);
            CHECK(
                (browser.viewer_import_edits == std::set<std::string>{"box-move-undo-redo", "box-resize-undo-redo", "mask-paint-undo-redo",
                                                                      "mask-erase-undo-redo", "class-undo-redo"}));
            const auto saved_path = compiled_directory / "viewer-annotations.cbor";
            REQUIRE(std::filesystem::is_regular_file(saved_path));
            const auto saved_bytes = read_from(saved_path, 0U);
            const auto saved = mmltk::frameworks::serialization::decode<mmltk::controller::contracts::AnnotationUiState>(
                {.first = std::as_bytes(std::span{saved_bytes})},
                {.max_bytes = mmltk::controller::contracts::kAnnotationUiStateByteBudget,
                 .max_items = mmltk::controller::contracts::kAnnotationUiStateByteBudget});
            REQUIRE(saved.has_value());
            CHECK(saved->valid());
            CHECK(saved->scene.palette == mmltk::controller::contracts::annotation_class_palette(saved->scene.categories.size()));
            for (const auto shape : mmltk::frameworks::reflection::enum_entries<mmltk::controller::contracts::AnnotationShape>())
                CHECK(std::ranges::any_of(saved->scene.objects,
                                          [&](const auto& object) { return object.enabled && object.shape == shape.value; }));
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
    CHECK(std::ranges::any_of(browser.explore_slots, [this](const auto& rendered) {
        return rendered.second == native.placeholder_slots.at(native.partial_generation);
    }));
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
    CHECK_FALSE(compiled.class_names.empty());

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
        const bool complete = std::ranges::all_of(entry.second.counted, [](const bool value) { return value; });
        return complete && !live_publication(entry.first.first, entry.first.second);
    });
    for (auto& [_, publication] : next_pixels.samples) {
        publication.receivers[3] = {};
        publication.viewer = false;
        publication.counted.fill(false);
    }
    pixel_audit = std::move(next_pixels);

    AtlasDrawAudit retained_atlas;
    retained_atlas.acquisitions = std::move(browser.atlas_draws.acquisitions);
    std::erase_if(retained_atlas.acquisitions,
                  [&](const auto& entry) { return !surface_audit.surfaces.contains(std::get<0>(entry.first)); });
    std::set<AtlasDrawAudit::SourceKey> retained_sources;
    for (const auto& [_, capture] : retained_atlas.acquisitions)
        retained_sources.insert(AtlasDrawAudit::source_key(capture));
    std::map<std::array<std::uint64_t, 3U>, AtlasDrawAudit::SourceKey> latest_sources;
    for (const auto& [key, _] : browser.atlas_draws.sources) {
        const std::array owner{key[0], key[1], key[2]};
        auto [found, inserted] = latest_sources.try_emplace(owner, key);
        if (!inserted && key[3] > found->second[3]) found->second = key;
    }
    for (const auto& [_, key] : latest_sources)
        retained_sources.insert(key);
    retained_atlas.sources = std::move(browser.atlas_draws.sources);
    retained_atlas.sessions = std::move(browser.atlas_draws.sessions);
    std::erase_if(retained_atlas.sources, [&](const auto& entry) { return !retained_sources.contains(entry.first); });
    std::erase_if(retained_atlas.sessions, [&](const auto& entry) { return !retained_sources.contains(entry.second); });
    const bool current_atlas = browser.owned_atlas_current;
    browser = {};
    browser.atlas_draws = std::move(retained_atlas);
    browser.owned_atlas_current = current_atlas;

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

void workspace_wayland_retained() {
    WaylandSession session{wayland_inputs(false), TerminationMode::SignalInterrupt, "retained"};
    session.RunScenario("square", false, false, true);
    session.RunScenario("", false);
    session.RunScenario("wide", false);
    session.RunScenario("tall", false);
    session.RunScenario("semantics", false);
    session.RunScenario("copy", false);
    session.RunScenario("copy", false, true);
    session.RunScenario("rapid", true);
}

void workspace_wayland_dpi() {
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, "dpi", true, true};
    session.RunScenario("copy", false, false, true);
    session.RunScenario("copy", false, true);
    session.RunScenario("rapid", true, true);
}

void workspace_wayland_terminal() {
    const auto terminal = GENERATE(TerminationMode::WindowClose, TerminationMode::AbruptPeerLoss);
    WaylandSession session{wayland_inputs(true), terminal, "terminal", true, false, true, {}, terminal != TerminationMode::WindowClose};
    session.RunScenario("terminal", true);
}

void workspace_wayland_probe_recovery() {
    const std::string boundary = GENERATE("allocation", "reset", "begin", "end");
    const auto inputs = wayland_inputs(false);
    WaylandSession session{inputs, TerminationMode::SignalInterrupt, "semantics", true, false, true, boundary, true, &inputs->probe()};
    session.RunScenario("semantics", true);
}

void workspace_wayland_gdr() {
    if (!execution_requested()) SKIP("workspace Wayland integration requires the packaged hardware runner");
    if (!gdr_transport_available()) SKIP("GDR hardware unavailable; focused packaged GDR coverage remains unverified");
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, "wide", true, false, false};
    session.RunScenario("wide", true);
}

void workspace_wayland_quiet() {
    const std::string profile = GENERATE("blocked", "quiet");
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, profile, false};
    session.RunScenario("quiet", true);
}

void add_rendered_probe_audit_fixture(NativeAudit& audit, const NativeAudit::PaddingOrientation orientation, const std::uint64_t generation,
                                      const std::uint64_t slot, const std::uint64_t compiled_index, const std::uint64_t frame_revision) {
    const auto key = std::pair{generation, slot};
    audit.padded_card_slots.emplace(key, compiled_index);
    audit.padding_orientations.emplace(key, orientation);
    audit.rendered_probe_slots.emplace(key, compiled_index);
    audit.transition_probe_slots.emplace(key, compiled_index);
    audit.rendered_probe_frames.emplace(key, std::vector{frame_revision});
    audit.selected_overlay_slots.emplace(key, NativeAudit::OverlayDescriptorIdentity{1U, compiled_index});
    audit.hidden_overlay_slots.emplace(std::pair{generation - 1U, slot + 1U},
                                       NativeAudit::OverlayDescriptorIdentity{1U, compiled_index + 100U});
    audit.transformed_overlay_slots.emplace(key);
    audit.semantic_overlay_slots.emplace(key);
    audit.placeholder_slots[generation].emplace(slot, compiled_index);
    audit.patched_slots[generation].emplace(slot, compiled_index);
}

[[nodiscard]] NativeAudit rendered_probe_audit_fixture(const bool vertical = true, const bool horizontal = true) {
    NativeAudit audit;
    if (vertical) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Vertical, 7U, 2U, 11U, 50U);
    if (horizontal) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Horizontal, 8U, 3U, 12U, 51U);
    return audit;
}

void rendered_probe_audit_rejects_mismatched_identity() {
    NativeAudit self_contained;
    self_contained.consume({{"kind", "gui_runtime"},
                            {"owner", "explore"},
                            {"event", "explore.card.rendered_probe"},
                            {"sequence", 7U},
                            {"value", 2U},
                            {"detail", 11U},
                            {"capacity_width", 31U},
                            {"capacity_height", 3916U},
                            {"staging_bytes", (89ULL << 32U) | (1ULL << 16U) | 1ULL},
                            {"source_width", 89U},
                            {"source_height", 89U},
                            {"content_x", 0U},
                            {"content_y", 22U},
                            {"content_width", 89U},
                            {"content_height", 45U}});
    CHECK_FALSE(self_contained.causal_inconsistent);
    CHECK(self_contained.rendered_probe_slots.at({7U, 2U}) == 11U);
    CHECK(self_contained.padding_orientations.at({7U, 2U}) == NativeAudit::PaddingOrientation::Vertical);

    const auto publish_frame = [](NativeAudit& audit, const std::uint64_t generation, const std::uint64_t revision) {
        audit.consume({{"kind", "gui_runtime"},
                       {"owner", "explore"},
                       {"event", "explore.frame.published"},
                       {"sequence", generation},
                       {"value", revision}});
    };
    NativeAudit incomplete_pair;
    incomplete_pair.placeholder_slots[7U].emplace(2U, 11U);
    incomplete_pair.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    publish_frame(incomplete_pair, 7U, 50U);
    CHECK_FALSE(incomplete_pair.causal_inconsistent);
    CHECK_FALSE(incomplete_pair.rendered_probe_frames.contains({7U, 2U}));
    incomplete_pair.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    incomplete_pair.reconcile_rendered_probes(7U);
    CHECK_FALSE(incomplete_pair.causal_inconsistent);
    CHECK(incomplete_pair.rendered_probe_frames.at({7U, 2U}) == std::vector{std::uint64_t{50U}});

    NativeAudit missing_patch;
    missing_patch.placeholder_slots[7U].emplace(2U, 11U);
    missing_patch.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    missing_patch.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    publish_frame(missing_patch, 7U, 50U);
    CHECK_FALSE(missing_patch.causal_inconsistent);
    CHECK(missing_patch.rendered_probe_frames.at({7U, 2U}) == std::vector{std::uint64_t{50U}});

    NativeAudit coalesced_publications;
    const auto coalesced_key = std::pair{7U, 2U};
    coalesced_publications.placeholder_slots[7U].emplace(2U, 11U);
    coalesced_publications.rendered_probe_slots.emplace(coalesced_key, 11U);
    coalesced_publications.transition_probe_slots.emplace(coalesced_key, 11U);
    coalesced_publications.rendered_probe_ordinals.emplace(coalesced_key, 2U);
    coalesced_publications.transition_probe_ordinals.emplace(coalesced_key, 3U);
    coalesced_publications.published_frames[7U] = {{1U, 49U}, {4U, 50U}, {5U, 51U}};
    coalesced_publications.reconcile_rendered_probes(7U);
    CHECK(coalesced_publications.rendered_probe_frames.at(coalesced_key) == std::vector{std::uint64_t{50U}, std::uint64_t{51U}});

    NativeAudit conflicting_pair;
    conflicting_pair.placeholder_slots[7U].emplace(2U, 11U);
    conflicting_pair.patched_slots[7U].emplace(2U, 12U);
    conflicting_pair.rendered_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    conflicting_pair.transition_probe_slots.emplace(std::pair{7U, 2U}, 11U);
    publish_frame(conflicting_pair, 7U, 50U);
    CHECK(conflicting_pair.causal_inconsistent);
    CHECK(conflicting_pair.causal_failure == "rendered probe patch identity");

    CHECK(rendered_probe_audit_fixture().aligned_overlay_pixels());
    const auto vertical_only = rendered_probe_audit_fixture(true, false);
    CHECK(vertical_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Vertical));
    CHECK_FALSE(vertical_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Horizontal));
    CHECK_FALSE(vertical_only.aligned_overlay_pixels());
    const auto horizontal_only = rendered_probe_audit_fixture(false, true);
    CHECK(horizontal_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Horizontal));
    CHECK_FALSE(horizontal_only.aligned_padding_orientation(NativeAudit::PaddingOrientation::Vertical));
    CHECK_FALSE(horizontal_only.aligned_overlay_pixels());

    auto stale_generation = rendered_probe_audit_fixture();
    stale_generation.transition_probe_slots.erase({7U, 2U});
    stale_generation.transition_probe_slots.emplace(std::pair{8U, 2U}, 11U);
    CHECK_FALSE(stale_generation.aligned_overlay_pixels());

    auto mismatched_slot = rendered_probe_audit_fixture();
    mismatched_slot.placeholder_slots.at(7U).clear();
    mismatched_slot.placeholder_slots.at(7U).emplace(3U, 11U);
    CHECK_FALSE(mismatched_slot.aligned_overlay_pixels());

    auto mismatched_compiled_index = rendered_probe_audit_fixture();
    mismatched_compiled_index.rendered_probe_slots.at({7U, 2U}) = 12U;
    CHECK_FALSE(mismatched_compiled_index.aligned_overlay_pixels());

    auto mismatched_orientation = rendered_probe_audit_fixture();
    mismatched_orientation.padding_orientations.at({7U, 2U}) = NativeAudit::PaddingOrientation::Horizontal;
    CHECK_FALSE(mismatched_orientation.aligned_overlay_pixels());

    auto mismatched_frame = rendered_probe_audit_fixture();
    mismatched_frame.rendered_probe_frames.at({7U, 2U}) = {0U};
    CHECK_FALSE(mismatched_frame.aligned_overlay_pixels());

    BrowserAudit browser;
    browser.surface_geometries.push_back({.presentation_revision = 70U, .source_revision = 50U, .width = 640.0, .height = 320.0});
    browser.surface_geometries.push_back({.presentation_revision = 71U, .source_revision = 51U, .width = 640.0, .height = 320.0});
    browser.surface_scales.emplace(std::pair{70U, 50U}, 1.0);
    browser.surface_scales.emplace(std::pair{71U, 51U}, 1.0);
    browser.surface_draws.emplace(70U);
    browser.surface_draws.emplace(71U);
    browser.explore_slots[9U].emplace(2U, 11U);
    browser.explore_slot_frames.emplace(9U, 50U);
    browser.explore_slots[10U].emplace(3U, 12U);
    browser.explore_slot_frames.emplace(10U, 51U);
    CHECK(browser.observed_frame_revision_for_slots({{2U, 11U}}, 50U));
    CHECK(browser.observed_frame_revision_for_slots({{3U, 12U}}, 51U));
    CHECK_FALSE(browser.observed_frame_revision_for_slots({{2U, 11U}}, 51U));
    CHECK_FALSE(browser.observed_frame_revision_for_slots({{3U, 11U}}, 50U));
    CHECK_FALSE(browser.observed_frame_revision_for_slots({{2U, 12U}}, 50U));
    browser.surface_draws.erase(70U);
    CHECK(browser.observed_frame_revision_for_slots({{2U, 11U}}, 50U));
    CHECK_FALSE(browser.rendered_frame_for_slots({{2U, 11U}}, 0U, 0U));
    CHECK(browser.rendered_frame_for_slots({{3U, 12U}}, 0U, 0U));
}

TEST_CASE("browser audit exposes distinct integration phases for progress deadlines", "[workspace][audit]") {
    BrowserAudit audit;
    audit.consume({{"event", "integration.phase_progress"}, {"control", "startup"}, {"detail", "AwaitBootstrap"}});
    CHECK(audit.phase_progress_revision == 1U);
    CHECK(audit.phase_progress_class == "startup");
    CHECK(audit.phase_progress_name == "AwaitBootstrap");

    audit.consume({{"event", "integration.phase_progress"}, {"control", "startup"}, {"detail", "AwaitBootstrap"}});
    CHECK(audit.phase_progress_revision == 1U);

    audit.consume({{"event", "integration.phase_progress"}, {"control", "work"}, {"detail", "AwaitCompileCompletion"}});
    CHECK(audit.phase_progress_revision == 2U);
    CHECK(audit.phase_progress_class == "work");
    CHECK(audit.phase_progress_name == "AwaitCompileCompletion");

    audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 41U}});
    CHECK(audit.work_progress_revision == 1U);
    audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 41U}});
    CHECK(audit.work_progress_revision == 1U);
    audit.consume({{"event", "integration.explore_reopen_wait"}, {"a", 42U}});
    CHECK(audit.work_progress_revision == 2U);
    audit.consume({{"event", "integration.explore_reopen_draw"}, {"d", 9U}});
    CHECK(audit.work_progress_revision == 3U);
    audit.consume({{"event", "integration.explore_reopen_draw"}, {"d", 9U}});
    CHECK(audit.work_progress_revision == 3U);

    audit.consume({{"event", "integration.phase_progress"}, {"control", "unbounded"}, {"detail", "Unknown"}});
    CHECK_FALSE(audit.bounds_valid);
}

// Configured inventory: primary + DPI + two destructive terminals + four
// startup-latched faults + two quiet paths = 10 H2D lifetimes. Optional GDR
// adds one focused lifetime. The former matrix used 23 per transport (46
// with GDR), recompiling/relaunching ordinary coverage for each case.
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][retained]", workspace_wayland_retained);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][dpi]", workspace_wayland_dpi);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][terminal]", workspace_wayland_terminal);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][probe_recovery]", workspace_wayland_probe_recovery);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][gdr]", workspace_wayland_gdr);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][quiet]", workspace_wayland_quiet);
MMLTK_REGISTER_TEST_CASE("[workspace_wayland_integration][rendered_probe_audit]", rendered_probe_audit_rejects_mismatched_identity);

}  // namespace
namespace {

[[nodiscard]] nlohmann::json native_surface_record(const char* event, const std::uint64_t low = 12U) {
    const std::string_view name{event};
    const bool copying = name.starts_with("presentation.source_borrow.") || name == "presentation.source.read_submitted";
    const bool source_control = name.starts_with("presentation.source.") && !copying;
    return {{"kind", "gui_runtime"},
            {"event", event},
            {"sequence", 7U},
            {"surface_high", 11U},
            {"surface_low", source_control ? low + 1000U : low},
            {"selection_generation", 19U},
            {"frame_revision", 23U},
            {"capacity_width", 64U},
            {"capacity_height", 32U},
            {"condition", 2U},
            {"outcome", name == "presentation.source.read_submitted" ? 0U : 1U},
            {"value", copying ? 0U : 1U},
            {"source_revision", 23U},
            {"source_session", 1U},
            {"source_width", 64U},
            {"source_height", 32U},
            {"workspace_source_high", 11U},
            {"workspace_source_low", low + 1000U},
            {"workspace_allocation", low + 2000U},
            {"workspace_bytes", 8192U},
            {"workspace_pitch", 256U},
            {"workspace_width", 64U},
            {"workspace_height", 32U},
            {"allocation_generation", 7U},
            {"presentation_revision", copying ? 0U : 1U},
            {"transfer_sequence", copying ? 0U : 1U},
            {"timeline_ready", copying ? 0U : 1U},
            {"trace_id", 31U},
            {"span_id", 31U},
            {"span_outcome", static_cast<std::uint64_t>(std::string_view{event}.ends_with(".completed")
                                                            ? mmltk::controller::contracts::DiagnosticSpanOutcome::Success
                                                            : mmltk::controller::contracts::DiagnosticSpanOutcome::Unspecified)}};
}

[[nodiscard]] nlohmann::json browser_surface_record(const char* event, const std::uint64_t low = 12U) {
    const std::string_view name{event};
    return {{"event", event},
            {"surface",
             SurfaceAudit::native_identity(native_surface_record("", name.starts_with("firefox.workspace.source.") ? low + 1000U : low))},
            {"source", SurfaceAudit::native_identity(native_surface_record("", low + 1000U))},
            {"arena", SurfaceAudit::native_identity(native_surface_record("", low))},
            {"workspace_allocation", low + 2000U},
            {"layer", 0U},
            {"slot", 0U},
            {"content_session", 1U},
            {"content_sequence", 23U},
            {"content_width", 64U},
            {"content_height", 32U},
            {"transfer_sequence", 1U},
            {"timeline_ready", 1U},
            {"timeline_release", 2U},
            {"requested_surface", SurfaceAudit::native_identity(native_surface_record("", low))},
            {"generation", 7U},
            {"width", 64U},
            {"height", 32U},
            {"frame_revision", 23U},
            {"presentation_revision", 1U},
            {"outcome", "claimed"}};
}

constexpr std::array native_surface_events{"presentation.arena.advertised",
                                           "presentation.admission.enqueued",
                                           "presentation.admission.written",
                                           "presentation.import.outcome",
                                           "presentation.source.admission.enqueued",
                                           "presentation.source.admission.written",
                                           "presentation.source.ready",
                                           "presentation.source_borrow.started",
                                           "presentation.source_borrow.completed",
                                           "presentation.source.read_submitted",
                                           "presentation.ready_sync.started",
                                           "presentation.ready_sync.completed",
                                           "presentation.frame.edge",
                                           "presentation.release_wait.started",
                                           "presentation.release_wait.completed",
                                           "presentation.active.withdrawal",
                                           "presentation.retirement"};
constexpr std::array browser_surface_events{"firefox.workspace.admitted",
                                            "iced.surface.texture_create",
                                            "firefox.workspace.claim_outcome",
                                            "firefox.workspace.registry_inserted",
                                            "firefox.workspace.import_ready_emitted",
                                            "firefox.workspace.ready",
                                            "firefox.workspace.source.admitted",
                                            "firefox.workspace.source.claim_outcome",
                                            "firefox.workspace.source.ready",
                                            "firefox.workspace.frame_forwarded",
                                            "firefox.workspace.frame_dispatched",
                                            "firefox.workspace.copy_completed",
                                            "iced.surface.sample_acquired",
                                            "iced.surface.sample_draw_selected",
                                            "iced.surface.draw_encoded",
                                            "iced.frame.draw_settled",
                                            "firefox.workspace.withdrawal",
                                            "iced.frame.sample_released",
                                            "iced.surface.texture_destroyed",
                                            "firefox.workspace.retired"};
constexpr std::size_t browser_live_stages = 16U;

void record_source_transfer(SurfaceAudit& audit) {
    for (std::size_t stage = 0U; stage < 15U; ++stage)
        audit.native(native_surface_record(native_surface_events[stage]));
    for (std::size_t stage = 0U; stage < 9U; ++stage)
        audit.browser(browser_surface_record(browser_surface_events[stage]));
}

void record_receiver_withdrawal(SurfaceAudit& audit) {
    for (std::size_t index = 0U; index < 6U; ++index)
        audit.browser(browser_surface_record(browser_surface_events[index]));
    audit.browser(browser_surface_record("iced.surface.pending_discarded"));
    audit.browser(browser_surface_record("firefox.workspace.withdrawal"));
    audit.browser(browser_surface_record("iced.surface.import_dropped"));
    audit.browser(browser_surface_record("iced.surface.texture_destroyed"));
    audit.browser(browser_surface_record("firefox.workspace.retired"));
}

TEST_CASE("scenario settlement retains physical reuse and waits for independent native delivery", "[workspace][audit]") {
    SurfaceAudit audit;
    for (std::size_t stage = 0U; stage < 12U; ++stage)
        audit.native(native_surface_record(native_surface_events[stage]));
    for (std::size_t stage = 0U; stage < browser_live_stages; ++stage)
        audit.browser(browser_surface_record(browser_surface_events[stage]));
    REQUIRE(audit.failure.empty());
    CHECK_FALSE(audit.evidence_settled());
    for (std::size_t stage = 12U; stage < 15U; ++stage)
        audit.native(native_surface_record(native_surface_events[stage]));
    REQUIRE(audit.evidence_settled());
    const auto id = SurfaceAudit::native_identity(native_surface_record(""));
    const auto publications = audit.surfaces.at(id).publications;
    audit.SettleScenario();
    REQUIRE(audit.surfaces.contains(id));
    CHECK(audit.surfaces.at(id).publications == publications);
    CHECK(audit.draws.empty());
    audit.browser(browser_surface_record("iced.surface.sample_draw_selected"));
    audit.browser(browser_surface_record("iced.surface.draw_encoded"));
    audit.browser(browser_surface_record("iced.frame.draw_settled"));
    CHECK(audit.evidence_settled());
    CHECK(audit.draws.size() == 1U);
    for (std::size_t stage = 15U; stage < native_surface_events.size(); ++stage)
        audit.native(native_surface_record(native_surface_events[stage]));
    CHECK_FALSE(audit.evidence_settled());
    for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage)
        audit.browser(browser_surface_record(browser_surface_events[stage]));
    REQUIRE(audit.evidence_settled());
    audit.SettleScenario();
    CHECK(audit.surfaces.empty());
    CHECK(audit.failure.empty());
}

TEST_CASE("receiver process exit terminates page readers without fabricating draw completion", "[workspace][audit]") {
    const std::string boundary = GENERATE("shutdown", "bridge", "exit", "premature-texture-destruction");
    SurfaceAudit audit;
    for (const char* event : native_surface_events)
        audit.native(native_surface_record(event));
    for (std::size_t stage = 0U; stage < browser_live_stages - 1U; ++stage)
        audit.browser(browser_surface_record(browser_surface_events[stage]));
    REQUIRE(audit.failure.empty());
    REQUIRE_FALSE(audit.evidence_settled());
    audit.native({{"event", "shutdown.requested"}});
    if (boundary == "bridge")
        audit.browser({{"event", "firefox.workspace.channel_terminal"}, {"terminal", "orderly_bridge_close"}});
    if (boundary == "premature-texture-destruction")
        audit.browser(browser_surface_record("iced.surface.texture_destroyed"));
    if (boundary == "exit" || boundary == "premature-texture-destruction")
        audit.native({{"event", "shutdown.firefox_terminal"}});
    CHECK(audit.joined_failure().empty() == (boundary == "exit"));
    const auto& custody = audit.surfaces.begin()->second.custody.begin()->second;
    CHECK(custody.settled == 0U);
    CHECK(custody.abandoned == 0U);
}

TEST_CASE("physical lifecycle custody rejects capacity overflow instead of dropping provenance", "[workspace][audit]") {
    SurfaceAudit audit;
    for (std::size_t identity = 1U; identity <= kAcceptanceGenerationLimit; ++identity)
        audit.native(native_surface_record("presentation.arena.advertised", identity));
    REQUIRE(audit.failure.empty());
    audit.native(native_surface_record("presentation.arena.advertised", kAcceptanceGenerationLimit + 1U));
    CHECK_FALSE(audit.failure.empty());
    CHECK(audit.surfaces.size() == kAcceptanceGenerationLimit);
}

TEST_CASE("surface join rejects missing native provenance", "[workspace][audit]") {
    for (const char* field :
         {"sequence", "selection_generation", "frame_revision", "capacity_width", "capacity_height", "condition", "outcome"}) {
        SurfaceAudit audit;
        auto record = native_surface_record("presentation.arena.advertised");
        record.erase(field);
        audit.native(record);
        CHECK_FALSE(audit.joined_failure().empty());
    }
    SurfaceAudit audit;
    audit.browser(nlohmann::json{{"event", "iced.surface.texture_create"}, {"surface", "not-a-capability"}});
    CHECK_FALSE(audit.joined_failure().empty());
}

TEST_CASE("surface join records failed span endings without accepting a completed physical transition", "[workspace][audit]") {
    using mmltk::controller::contracts::DiagnosticSpanOutcome;
    for (const auto outcome : {DiagnosticSpanOutcome::Unspecified, DiagnosticSpanOutcome::ScopeExit, DiagnosticSpanOutcome::Cancelled,
                               DiagnosticSpanOutcome::Exception}) {
        SurfaceAudit audit;
        for (std::size_t index = 0U; index < 8U; ++index)
            audit.native(native_surface_record(native_surface_events[index]));
        auto ended = native_surface_record("presentation.source_borrow.completed");
        ended["span_outcome"] = static_cast<std::uint64_t>(outcome);
        audit.native(ended);
        const auto& surface = audit.surfaces.at(SurfaceAudit::native_identity(ended));
        CHECK(surface.source_steps.at(31U) == 1U);
        CHECK(surface.ended_spans.at(31U) == static_cast<std::uint64_t>(outcome));
        audit.native(native_surface_record("presentation.source.read_submitted"));
        CHECK_FALSE(audit.joined_failure().empty());
    }
}

TEST_CASE("surface join rejects omitted admission and reordered observed copy stages", "[workspace][audit]") {
    for (std::size_t omitted = 0U; omitted < native_surface_events.size(); ++omitted) {
        if (std::string_view{native_surface_events[omitted]} == "presentation.active.withdrawal")
            continue;  // Receiver-initiated withdrawal is independently authoritative.
        SurfaceAudit audit;
        for (std::size_t index = 0U; index < native_surface_events.size(); ++index)
            if (index != omitted) audit.native(native_surface_record(native_surface_events[index]));
        for (const char* event : browser_surface_events)
            audit.browser(browser_surface_record(event));
        CHECK_FALSE(audit.joined_failure().empty());
    }
    for (std::size_t omitted = 0U; omitted < browser_surface_events.size(); ++omitted) {
        SurfaceAudit audit;
        for (const char* event : native_surface_events)
            audit.native(native_surface_record(event));
        for (std::size_t index = 0U; index < browser_surface_events.size(); ++index)
            if (index != omitted) audit.browser(browser_surface_record(browser_surface_events[index]));
        CHECK_FALSE(audit.joined_failure().empty());
    }
    SurfaceAudit reordered;
    reordered.native(native_surface_record("presentation.admission.written"));
    reordered.native(native_surface_record("presentation.arena.advertised"));
    CHECK_FALSE(reordered.joined_failure().empty());
    SurfaceAudit reordered_copy;
    for (std::size_t index = 0U; index < 4U; ++index)
        reordered_copy.native(native_surface_record(native_surface_events[index]));
    reordered_copy.native(native_surface_record("presentation.source.read_submitted"));
    reordered_copy.native(native_surface_record("presentation.source_borrow.started"));
    CHECK_FALSE(reordered_copy.joined_failure().empty());
    SurfaceAudit dropped_ready_start;
    for (std::size_t index = 0U; index < native_surface_events.size(); ++index)
        if (index != 10U) dropped_ready_start.native(native_surface_record(native_surface_events[index]));
    for (const char* event : browser_surface_events)
        dropped_ready_start.browser(browser_surface_record(event));
    CHECK_FALSE(dropped_ready_start.joined_failure().empty());
    SurfaceAudit reordered_ready;
    for (std::size_t index = 0U; index < 10U; ++index)
        reordered_ready.native(native_surface_record(native_surface_events[index]));
    reordered_ready.native(native_surface_record("presentation.ready_sync.completed"));
    reordered_ready.native(native_surface_record("presentation.ready_sync.started"));
    CHECK_FALSE(reordered_ready.joined_failure().empty());
}

TEST_CASE("surface join accepts receiver-confirmed withdrawal and explicit rejected-import retirement", "[workspace][audit]") {
    SurfaceAudit audit;
    for (std::size_t index = 0U; index < 4U; ++index)
        audit.native(native_surface_record(native_surface_events[index]));
    audit.native(native_surface_record("presentation.retirement"));
    record_receiver_withdrawal(audit);
    CHECK(audit.joined_failure().empty());

    SurfaceAudit dropped_candidate_retirement;
    for (std::size_t index = 0U; index < 4U; ++index)
        dropped_candidate_retirement.native(native_surface_record(native_surface_events[index]));
    dropped_candidate_retirement.native(native_surface_record("presentation.candidate.withdrawal"));
    auto replacement = native_surface_record("presentation.replacement", 13U);
    replacement["sequence"] = 8U;
    replacement["allocation_generation"] = 8U;
    dropped_candidate_retirement.native(replacement);
    record_receiver_withdrawal(dropped_candidate_retirement);
    CHECK_FALSE(dropped_candidate_retirement.joined_failure().empty());

    for (const bool released : {false, true}) {
        CAPTURE(released);
        SurfaceAudit used_pending;
        for (const char* event : native_surface_events)
            used_pending.native(native_surface_record(event));
        for (const char* event : browser_surface_events) {
            used_pending.browser(browser_surface_record(event));
            if (std::string_view{event} == "iced.surface.sample_acquired") break;
        }
        used_pending.browser(browser_surface_record("firefox.workspace.withdrawal"));
        used_pending.browser(browser_surface_record("iced.surface.pending_discarded"));
        if (released) used_pending.browser(browser_surface_record("iced.frame.sample_released"));
        used_pending.browser(browser_surface_record("iced.surface.texture_destroyed"));
        used_pending.browser(browser_surface_record("firefox.workspace.retired"));
        CHECK(used_pending.joined_failure().empty() == released);
    }

    for (const bool observed_native_outcome : {false, true}) {
        SurfaceAudit rejected;
        for (std::size_t index = 0U; index < 3U; ++index)
            rejected.native(native_surface_record(native_surface_events[index]));
        if (observed_native_outcome) {
            auto outcome = native_surface_record("presentation.import.outcome");
            outcome["value"] = 0U;
            outcome["outcome"] = 4U;
            rejected.native(outcome);
        }
        rejected.native(native_surface_record("presentation.retirement"));
        rejected.browser(browser_surface_record("firefox.workspace.admitted"));
        rejected.browser(browser_surface_record("iced.surface.texture_create"));
        rejected.browser(browser_surface_record("firefox.workspace.claim_outcome"));
        rejected.browser(browser_surface_record("firefox.workspace.import_failed"));
        rejected.browser(browser_surface_record("iced.surface.pending_discarded"));
        rejected.browser(browser_surface_record("iced.surface.texture_destroyed"));
        CHECK(rejected.joined_failure().empty() == observed_native_outcome);
    }
}

TEST_CASE("incremental Explore audit requires independent lifecycle and exact inventory evidence", "[workspace][audit]") {
    NativeAudit annotation;
    annotation.consume({{"kind", "gui_runtime"}, {"owner", "annotation"}, {"event", "document.opened"}});
    CHECK_FALSE(annotation.annotation_copied);
    CHECK(annotation.annotation_opened);
    annotation.consume({{"kind", "gui_runtime"}, {"owner", "annotation"}, {"event", "copy.completed"}});
    CHECK(annotation.annotation_copied);
    NativeAudit closure;
    closure.consume({{"kind", "gui_runtime"}, {"event", "browser.server.peer_opened"}});
    closure.consume({{"kind", "gui_runtime"}, {"event", "shutdown.complete"}});
    CHECK(closure.peer_open_count == 1U);
    CHECK(closure.peer_close_count == 0U);
    closure.consume({{"kind", "gui_runtime"}, {"event", "browser.server.peer_closed"}});
    CHECK(closure.peer_close_count == closure.peer_open_count);

    NativeAudit audit;
    audit.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U, 4096U);
    audit.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U, 4096U);
    audit.consume_explore_evidence("acceptance.placeholder.complete", 2U, 19U, 4096U);
    audit.consume_explore_evidence("acceptance.slot.patched", 0U, 10U, 4096U);
    audit.consume_explore_evidence("tile.batch.published", 1U, 0U, 4096U);
    CHECK(audit.explore_placeholder);
    CHECK(audit.partial_generation == 2U);
    CHECK(audit.acceptance_first_patch_exact);

    NativeAudit joined_after_publication;
    joined_after_publication.consume_explore_evidence("acceptance.placeholder.complete", 2U, 19U, 4096U);
    joined_after_publication.consume_explore_evidence("acceptance.slot.patched", 0U, 10U, 4096U);
    joined_after_publication.consume_explore_evidence("tile.batch.published", 1U, 0U, 4096U);
    CHECK_FALSE(joined_after_publication.acceptance_first_patch_exact);
    joined_after_publication.join_gallery_publication(2U, {{0U, 10U}, {1U, 11U}});
    CHECK_FALSE(joined_after_publication.acceptance_first_patch_exact);
    joined_after_publication.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U, 4096U);
    joined_after_publication.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U, 4096U);
    joined_after_publication.join_gallery_publication(2U, {{0U, 10U}, {1U, 11U}});
    CHECK(joined_after_publication.partial_generation == 2U);
    CHECK(joined_after_publication.acceptance_first_patch_exact);

    const std::map<std::uint64_t, std::uint64_t> rendered_slots{{0U, 10U}, {1U, 11U}};
    for (const auto missing : {"none", "summary", "slot", "patch", "count", "frame", "identity", "conflict"}) {
        INFO("missing native inventory evidence: " << missing);
        NativeAudit material;
        const std::string_view omitted{missing};
        material.consume_explore_evidence("placeholder.published", 2U);
        material.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U);
        if (omitted != "slot") material.consume_explore_evidence("acceptance.placeholder.slot", 1U, omitted == "conflict" ? 12U : 11U);
        if (omitted != "summary") material.consume_explore_evidence("acceptance.placeholder.complete", 2U);
        material.consume_explore_evidence("acceptance.slot.patched", 0U, 10U);
        if (omitted != "patch") material.consume_explore_evidence("acceptance.slot.patched", 1U, omitted == "identity" ? 12U : 11U);
        material.consume_explore_evidence("tile.batch.published", omitted == "count" ? 1U : 2U);
        if (omitted != "frame") material.consume_explore_evidence("explore.frame.published", 7U);
        material.join_gallery_publication(3U, rendered_slots);
        material.join_gallery_publication(2U, rendered_slots);
        CHECK_FALSE(material.final_generations_for(rendered_slots, 3U, 7U).has_value());
        CHECK_FALSE(material.final_generations_for(rendered_slots, 2U, 8U).has_value());
        material.join_gallery_publication(2U, rendered_slots);
        const auto final = material.final_generations_for(rendered_slots, 2U, 7U);
        CHECK(final.has_value() == (omitted != "summary" && omitted != "slot" && omitted != "frame" && omitted != "conflict"));
        const bool complete_material = final && material.patched_slots.at(2U) == rendered_slots &&
                                       material.published_tiles.at(2U) == rendered_slots.size() && !material.explore_stale_patch &&
                                       !material.causal_inconsistent;
        CHECK(complete_material == (omitted == "none"));
        if (final) {
            CHECK(final->material == 2U);
            CHECK(final->cursor == 2U);
            CHECK(material.placeholder_slots.at(2U) == rendered_slots);
            CHECK(material.placeholder_cardinalities.at(2U) == rendered_slots.size());
        }
        if (omitted == "conflict") CHECK(material.causal_inconsistent);
    }

    NativeAudit initial;
    initial.consume_explore_evidence("placeholder.published", 0U);
    initial.consume_explore_evidence("acceptance.placeholder.slot", 0U, 10U);
    initial.consume_explore_evidence("acceptance.slot.patched", 0U, 10U);
    initial.consume_explore_evidence("explore.frame.published", 7U);
    initial.consume_explore_evidence("tile.batch.published", 1U);
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK_FALSE(initial.acceptance_first_patch_exact);
    CHECK_FALSE(initial.placeholder_cardinalities.contains(2U));
    initial.consume_explore_evidence("acceptance.placeholder.slot", 1U, 11U);
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK_FALSE(initial.acceptance_first_patch_exact);
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK_FALSE(initial.acceptance_first_patch_exact);
    initial.consume_explore_evidence("acceptance.placeholder.complete", 2U);
    CHECK(initial.acceptance_first_patch_exact);
    CHECK(initial.partial_generation == 2U);
    CHECK(initial.partial_first_tile_count == 1U);
    CHECK_FALSE(initial.causal_inconsistent);
    CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
    initial.consume({{"kind", "gui_runtime"},
                     {"owner", "presentation"},
                     {"event", "presentation.frame.edge"},
                     {"source_session", 1U},
                     {"source_instance", 1U},
                     {"source_observation_revision", 10U},
                     {"source_revision", 8U},
                     {"frame_revision", 8U}});
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK_FALSE(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
    initial.consume_explore_evidence("explore.frame.published", 8U);
    initial.join_gallery_publication(2U, rendered_slots);
    CHECK(initial.final_generations_for(rendered_slots, 2U, 8U).has_value());
    initial.augmentation_seeds.emplace(2U, 0U);
    CHECK(initial.augmentation_generation_for(0U, 8U) == 2U);
    CHECK_FALSE(initial.augmentation_generation_for(1U, 8U).has_value());

    for (const std::string_view missing :
         {"none", "bitmap", "wrong-ready", "extra-ready", "patch", "identity", "frame", "observation", "physical", "late-patch"}) {
        INFO("missing partial publication evidence: " << missing);
        NativeAudit partial;
        partial.consume_explore_evidence("placeholder.published", 0U);
        for (std::uint64_t slot = 0U; slot < 3U; ++slot)
            partial.consume_explore_evidence("acceptance.placeholder.slot", slot, slot + 10U);
        partial.consume_explore_evidence("acceptance.placeholder.complete", 3U);
        if (missing != "patch" && missing != "late-patch")
            partial.consume_explore_evidence("acceptance.slot.patched", 1U, missing == "identity" ? 12U : 11U);
        partial.consume_explore_evidence("explore.frame.published", 8U);
        if (missing != "physical")
            partial.consume({{"kind", "gui_runtime"},
                             {"owner", "presentation"},
                             {"event", "presentation.frame.edge"},
                             {"source_session", 1U},
                             {"source_instance", 1U},
                             {"source_observation_revision", 11U},
                             {"source_revision", 8U},
                             {"frame_revision", 8U}});
        if (missing == "late-patch") partial.consume_explore_evidence("acceptance.slot.patched", 1U, 11U);

        BrowserAudit browser;
        nlohmann::json snapshot{{"gallery_generation", 2U},
                                {"visible_indices", {10U, 11U, 12U}},
                                {"source_revision", missing == "frame" ? 9U : 8U},
                                {"source_observation_revision", missing == "observation" ? 12U : 11U}};
        if (missing != "bitmap")
            snapshot["ready_slots"] = std::vector<bool>{missing == "wrong-ready", missing != "wrong-ready", missing == "extra-ready"};
        browser.consume_gallery_generation(snapshot);
        REQUIRE(browser.bounds_valid);
        const auto& evidence = browser.gallery_generations.at(2U);
        partial.join_gallery_publication(2U, evidence.slots);
        // Even a complete receiver bitmap and Presentation receipt cannot
        // substitute for the native batch publication.
        CHECK_FALSE(partial.acceptance_first_patch_exact);
        CHECK_FALSE(partial.first_publication_ordinals.contains(2U));
        if (missing == "none") {
            partial.consume_explore_evidence("tile.batch.published", 1U);
            CHECK(partial.acceptance_first_patch_exact);
            CHECK(partial.partial_generation == 2U);
            CHECK(partial.partial_first_tile_count == 1U);
            CHECK(partial.first_patched_slots.at(2U) == std::map<std::uint64_t, std::uint64_t>{{1U, 11U}});
            partial.consume_explore_evidence("acceptance.slot.patched", 2U, 12U);
            partial.consume_explore_evidence("tile.batch.published", 2U);
            CHECK(partial.explore_ready_batch);
            CHECK_FALSE(partial.explore_tile_regressed);
        } else {
            CHECK_FALSE(partial.first_publication_ordinals.contains(2U));
        }
        snapshot["ready_slots"] = std::vector<bool>{true};
        BrowserAudit malformed;
        malformed.consume_gallery_generation(snapshot);
        CHECK_FALSE(malformed.bounds_valid);
        CHECK(malformed.gallery_generations.empty());
    }
}

TEST_CASE("surface join rejects duplicate claims and texture construction", "[workspace][audit]") {
    for (const char* repeated : {"firefox.workspace.claim_outcome", "iced.surface.texture_create"}) {
        SurfaceAudit audit;
        for (const char* event : browser_surface_events) {
            audit.browser(browser_surface_record(event));
            if (std::string_view{event} == repeated) audit.browser(browser_surface_record(event));
        }
        CHECK_FALSE(audit.failure.empty());
    }
}

TEST_CASE("surface join rejects otherwise complete traces for different physical identities", "[workspace][audit]") {
    SurfaceAudit audit;
    for (const char* event : native_surface_events)
        audit.native(native_surface_record(event, 12U));
    for (const char* event : browser_surface_events)
        audit.browser(browser_surface_record(event, 13U));
    CHECK_FALSE(audit.joined_failure().empty());
    CHECK_FALSE(audit.pending_supersession_completed());
}

}  // namespace

namespace {

enum class MissingHandoffEvidence { PendingDiscard, Reconstruction, CaptureBeforeDraw, DuplicateReconstruction, DifferentCompleted };

[[nodiscard]] SurfaceAudit pending_handoff(const std::optional<MissingHandoffEvidence> missing = std::nullopt) {
    SurfaceAudit audit;
    const auto native = [&](const char* event, const std::uint64_t surface) {
        auto record = native_surface_record(event, surface);
        record["sequence"] = surface;
        record["allocation_generation"] = surface;
        audit.native(record);
    };
    const auto browser = [&](const char* event, const std::uint64_t surface, const std::uint64_t requested = 0U) {
        auto record = browser_surface_record(event, surface);
        record["generation"] = surface;
        if (requested != 0U) record["requested_surface"] = SurfaceAudit::native_identity(native_surface_record("", requested));
        audit.browser(record);
        if (std::string_view{event} == "iced.surface.sample_draw_selected") {
            record["event"] = "iced.surface.draw_encoded";
            audit.browser(record);
            record["event"] = "iced.frame.draw_settled";
            audit.browser(record);
        }
    };
    constexpr std::uint64_t d = 11U, a = 12U, b = 13U, c = 14U;
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (const char* event : native_surface_events)
            native(event, d);
    for (std::size_t stage = 0U; stage < 15U; ++stage)
        native(native_surface_events[stage], a);
    for (const auto surface : {b, c})
        for (std::size_t stage = 0U; stage < 4U; ++stage)
            native(native_surface_events[stage], surface);
    native("presentation.candidate.withdrawal", b);
    native("presentation.retirement", b);
    for (std::size_t stage = 4U; stage < 15U; ++stage)
        native(native_surface_events[stage], c);
    for (const auto surface : {a, c}) {
        native("presentation.active.withdrawal", surface);
        native("presentation.retirement", surface);
    }
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (std::size_t stage = 0U; stage < 14U; ++stage)
            browser(browser_surface_events[stage], d);
    for (std::size_t stage = 0U; stage < 14U; ++stage)
        browser(browser_surface_events[stage], a);
    for (std::size_t stage = 0U; stage < 6U; ++stage)
        browser(browser_surface_events[stage], b);
    if (missing != MissingHandoffEvidence::Reconstruction)
        browser("iced.surface.renderer_reconstructed", missing == MissingHandoffEvidence::DifferentCompleted ? d : a, b);
    if (missing == MissingHandoffEvidence::DuplicateReconstruction) browser("iced.surface.renderer_reconstructed", a, b);
    browser("iced.surface.sample_draw_selected", a, b);
    browser("firefox.workspace.withdrawal", b);
    for (std::size_t stage = 0U; stage < 2U; ++stage)
        browser(browser_surface_events[stage], c);
    if (missing != MissingHandoffEvidence::PendingDiscard) browser("iced.surface.pending_discarded", b);
    browser("iced.surface.import_dropped", b);
    browser("iced.surface.texture_destroyed", b);
    browser("firefox.workspace.retired", b);
    browser("iced.surface.sample_draw_selected", a, c);
    for (std::size_t stage = 2U; stage < 6U; ++stage)
        browser(browser_surface_events[stage], c);
    for (std::size_t stage = 6U; stage < 12U; ++stage)
        browser(browser_surface_events[stage], c);
    if (missing == MissingHandoffEvidence::CaptureBeforeDraw) {
        browser("iced.surface.sample_draw_selected", c);
        browser("iced.surface.sample_acquired", c);
    } else {
        browser("iced.surface.sample_acquired", c);
        browser("iced.surface.sample_draw_selected", a, c);
        browser("iced.surface.sample_draw_selected", c);
    }
    for (const auto surface : {a, c})
        for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage)
            browser(browser_surface_events[stage], surface);
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (std::size_t stage = browser_live_stages; stage < browser_surface_events.size(); ++stage)
            browser(browser_surface_events[stage], d);
    return audit;
}

TEST_CASE("rapid surface join requires a complete pending-candidate handoff", "[workspace][audit]") {
    const auto complete = pending_handoff();
    REQUIRE(complete.joined_failure().empty());
    CHECK(complete.pending_supersession_completed());
    for (const auto missing :
         {MissingHandoffEvidence::PendingDiscard, MissingHandoffEvidence::Reconstruction, MissingHandoffEvidence::CaptureBeforeDraw,
          MissingHandoffEvidence::DuplicateReconstruction, MissingHandoffEvidence::DifferentCompleted}) {
        const auto audit = pending_handoff(missing);
        INFO("surface join: " << audit.joined_failure());
        if (missing != MissingHandoffEvidence::CaptureBeforeDraw && missing != MissingHandoffEvidence::DuplicateReconstruction)
            CHECK(audit.joined_failure().empty());
        else
            CHECK_FALSE(audit.joined_failure().empty());
        CHECK_FALSE(audit.pending_supersession_completed());
    }
}

[[nodiscard]] nlohmann::json atlas_draw_record(const std::uint64_t revision, const std::uint64_t first_row, const std::uint64_t rows,
                                               const double top) {
    nlohmann::json record{
        {"event", "iced.surface.draw_encoded"},
        {"control", kExploreGalleryControl},
        {"source_revision", revision},
        {"frame_revision", revision},
        {"presentation_revision", revision + 100U},
        {"content_session", 1U},
        {"source_kind", 1U},
        {"source_instance", 1U},
        {"dataset_identity", 11U},
        {"gallery_generation", revision},
        {"surface", "000000000000000b000000000000000c"},
        {"generation", 7U},
        {"width", 512U},
        {"height", 1024U},
        {"layer", 0U},
        {"slot", 0U},
        {"content_width", 400U},
        {"content_height", 800U},
        {"row_capacity", 8U},
        {"row_origin", first_row % 8U},
        {"card_extent", 100U},
        {"augmentation_enabled", false},
        {"augmentation_seed", 0U},
        {"ready_slots", std::vector<bool>{true, true}},
        {"columns", 4U},
        {"rows", rows},
        {"first_row", first_row},
        {"matching_count", 300U},
        {"visible_indices", std::vector<std::uint64_t>{first_row * 4U, first_row * 4U + 1U}},
        {"bounds", std::array{30.0, top, 600.0, static_cast<double>(rows) * 150.0}},
        {"clip", std::array{30.0, 0.0, 600.0, 500.0}},
    };
    record["image"] = record["bounds"];
    return record;
}

TEST_CASE("initial atlas completion requires nonblack canvas pixels for the exact publication", "[workspace][audit]") {
    const nlohmann::json completed{{"event", "integration.initial_atlas_complete"},
                                   {"detail", "no-input-canvas-pixels"},
                                   {"a", 17U},
                                   {"b", 3U},
                                   {"c", 23U},
                                   {"d", 29U}};
    for (const auto* defect : {"missing", "black", "empty", "source", "publication", "none"}) {
        INFO(defect);
        BrowserAudit audit;
        nlohmann::json pixels{{"event", "integration.atlas_canvas_pixels"}, {"a", 23U}, {"b", 29U}, {"c", 4U}, {"d", 4U}};
        const std::string_view kind{defect};
        if (kind == "black") pixels["d"] = 0U;
        if (kind == "empty") {
            pixels["c"] = 0U;
            pixels["d"] = 0U;
        }
        if (kind == "source") pixels["a"] = 22U;
        if (kind == "publication") pixels["b"] = 28U;
        if (kind != "missing") audit.consume(pixels);
        audit.consume(completed);
        CHECK(audit.initial_atlas_complete == (kind == "none"));
    }
}

TEST_CASE("annotation canvas palette evidence preserves class hue contrast and solid control colors", "[workspace][audit]") {
    struct Sample final {
        std::array<double, 3U> expected;
        std::array<double, 3U> observed;
        double scale;
        bool valid;
        const char* event = "integration.annotation_pixel";
    };
    constexpr std::array samples{
        Sample{{255, 255, 0}, {255, 255, 0}, 1.0, true},
        Sample{{255, 255, 0}, {228, 229, 8}, 1148.0 / 2048.0, true},
        Sample{{255, 255, 0}, {247, 248, 26}, 1148.0 / 2048.0, true},
        Sample{{0, 255, 0}, {44, 218, 10}, 1148.0 / 2048.0, true},
        Sample{{0, 255, 0}, {55, 218, 10}, 1148.0 / 2048.0, false},
        Sample{{255, 255, 0}, {228, 229, 8}, 1.0, false},
        Sample{{255, 255, 0}, {255, 0, 0}, .5, false},
        Sample{{255, 255, 0}, {200, 200, 200}, .5, false},
        Sample{{255, 255, 0}, {200, 200, 80}, .5, false},
        Sample{{255, 255, 0}, {90, 90, 0}, .5, false},
        Sample{{255, 255, 255}, {228, 229, 228}, .5, false},
        Sample{{255, 255, 0}, {500, 500, 0}, .5, false},
        Sample{{255, 255, 0}, {255, 255, 0}, 0.0, false},
        Sample{{255, 255, 0}, {228, 229, 8}, .5, false, "integration.annotation_swatch"},
        Sample{{32, 32, 32}, {32, 32, 32}, .5, true, "integration.annotation_capability"},
    };
    for (const auto& sample : samples) {
        BrowserAudit audit;
        audit.consume({{"event", sample.event},
                       {"a", 17U},
                       {"b", 23U},
                       {"expected", sample.expected},
                       {"observed", sample.observed},
                       {"source_to_screen", sample.scale},
                       {"matched", true}});
        CHECK(audit.annotation_pixels_valid == sample.valid);
    }
}

TEST_CASE("atlas visibility requires an encoded intersecting draw with exact source rows", "[workspace][audit]") {
    const auto draw = atlas_draw_record(23U, 2U, 5U, -37.25);
    auto source = draw;
    source["event"] = "iced.gallery.source";
    auto capture = draw;
    capture["event"] = "iced.surface.sample_acquired";
    const nlohmann::json missing{{"event", "iced.surface.sample_draw_missing"}, {"control", kExploreGalleryControl}};
    BrowserAudit observed;
    observed.consume(missing);
    CHECK_FALSE(observed.owned_atlas_seen);
    CHECK_FALSE(observed.owned_atlas_interrupted);
    observed.consume(source);
    observed.consume(capture);
    observed.consume(draw);
    const auto& complete = observed.atlas_draws;
    CHECK(complete.valid);
    CHECK(complete.seen);
    CHECK(complete.drawn_rows.contains(2U));
    for (const auto* control : {"", kExploreGalleryControl, "explore.detail.workspace"}) {
        auto browser = observed;
        REQUIRE(browser.owned_atlas_seen);
        browser.consume({{"event", "iced.surface.sample_draw_missing"}, {"control", control}});
        CHECK(browser.owned_atlas_interrupted == (std::string_view{control} != "explore.detail.workspace"));
        browser.consume(missing);
        CHECK(browser.owned_atlas_interrupted);
        browser.consume(draw);
        CHECK(browser.owned_atlas_interrupted);
    }
    for (const auto* control : {"explore.detail.workspace", "workflow.visual.workspace", "unknown.workspace"}) {
        auto browser = observed;
        auto replacement = draw;
        replacement["control"] = control;
        browser.consume(replacement);
        const bool unknown = std::string_view{control} == "unknown.workspace";
        CHECK(browser.owned_atlas_current == unknown);
        browser.consume(missing);
        CHECK(browser.owned_atlas_interrupted == unknown);
        browser.consume(draw);
        CHECK(browser.owned_atlas_current);
        CHECK(browser.owned_atlas_seen);
        browser.consume(missing);
        CHECK(browser.owned_atlas_interrupted);
        browser.consume(replacement);
        CHECK(browser.owned_atlas_interrupted);
    }
    for (const auto* event : {"iced.gallery.source", "iced.surface.renderer_reconstructed", "iced.surface.sample_acquired",
                              "iced.surface.sample_draw_selected"}) {
        auto browser = observed;
        auto pending = atlas_draw_record(24U, 3U, 5U, -37.25);
        pending["event"] = event;
        pending["control"] = "explore.detail.workspace";
        pending["surface"] = "000000000000000b000000000000000d";
        pending["generation"] = 8U;
        browser.consume(pending);
        CHECK(browser.owned_atlas_current);
        browser.consume(missing);
        CHECK(browser.owned_atlas_interrupted);
    }
    for (const auto* defect :
         {"offscreen", "grid", "row", "indices", "source", "source-instance", "session", "pending", "capture", "selection"}) {
        INFO(defect);
        AtlasDrawAudit audit;
        audit.consume(source);
        if (std::string_view{defect} != "capture") audit.consume(capture);
        auto invalid = draw;
        const std::string_view kind{defect};
        if (kind == "offscreen") invalid["image"][1] = 700.0;
        if (kind == "grid") invalid["rows"] = 4U;
        if (kind == "row") invalid["first_row"] = 10U;
        if (kind == "indices") invalid["visible_indices"][0] = 99U;
        if (kind == "source") invalid["source_revision"] = 24U;
        if (kind == "source-instance") invalid["source_instance"] = 2U;
        if (kind == "session") invalid["content_session"] = 3U;
        if (kind == "pending") invalid["presentation_revision"] = 2U;
        if (kind == "selection") invalid["event"] = "iced.surface.sample_draw_selected";
        audit.consume(invalid);
        CHECK_FALSE(audit.seen);
        if (kind != "selection") CHECK_FALSE(audit.valid);
    }
}

TEST_CASE("atlas stages require fresh complete draw identity and reject ambiguous sources", "[workspace][audit]") {
    AtlasDrawAudit complete;
    constexpr std::array names{"fractional", "row1", "row2", "row10", "end", "restored"};
    constexpr std::array<std::uint64_t, 6U> first_rows{0U, 1U, 2U, 10U, 70U, 0U};
    for (std::size_t index = 0U; index != names.size(); ++index) {
        const auto rows = index % 2U == 0U ? 5U : 4U;
        const double top = index == 4U ? -250.0 : (index == 0U || index == 2U ? -37.25 : 0.0);
        const auto draw = atlas_draw_record(23U + index, first_rows[index], rows, top);
        auto source = draw;
        source["event"] = "iced.gallery.source";
        auto capture = draw;
        capture["event"] = "iced.surface.sample_acquired";
        complete.consume(source);
        complete.consume(capture);
        complete.consume(draw);
        auto stage = draw;
        stage["event"] = "iced.surface.scroll_stage";
        stage["control"] = names[index];
        for (const auto* defect : {"source", "surface", "width", "height", "presentation", "row", "clip", "missing", "clipped"}) {
            CAPTURE(index, defect);
            auto invalid_audit = complete;
            auto invalid_stage = stage;
            const std::string_view kind{defect};
            if (kind == "source") invalid_stage["source_instance"] = 2U;
            if (kind == "surface") invalid_stage["surface"] = "000000000000000b000000000000000d";
            if (kind == "width") invalid_stage["width"] = 1024U;
            if (kind == "height") invalid_stage["height"] = scalar(stage, "height") + 1U;
            if (kind == "presentation") invalid_stage["presentation_revision"] = 1U;
            if (kind == "row") invalid_stage["first_row"] = 99U;
            if (kind == "clip") invalid_stage["clip"][1] = 20.0;
            if (kind == "missing") invalid_audit.last_draw.reset();
            if (kind == "clipped") {
                auto clipped = draw;
                clipped["event"] = "iced.surface.sample_draw_clipped";
                invalid_audit.consume(clipped);
            }
            invalid_audit.consume(invalid_stage);
            CHECK_FALSE(invalid_audit.valid);
        }
        complete.consume(stage);
        CHECK(complete.valid);
        CHECK(complete.stages.contains(names[index]));
        auto duplicate = complete;
        source["ready_slots"] = std::vector<bool>{true, true};
        source["gallery_generation"] = scalar(source, "gallery_generation") + 1U;
        source["source_observation_revision"] = scalar(source, "source_observation_revision") + 1U;
        duplicate.consume(source);
        CHECK(duplicate.valid);
        auto observed_draw = draw;
        observed_draw["gallery_generation"] = source["gallery_generation"];
        observed_draw["source_observation_revision"] = source["source_observation_revision"];
        duplicate.consume(observed_draw);
        CHECK(duplicate.valid);
        for (const auto* field : {"dataset_identity", "first_row", "content_width"}) {
            auto conflicting = complete;
            auto conflicting_source = source;
            conflicting_source[field] = scalar(source, field) + 1U;
            conflicting.consume(conflicting_source);
            CHECK_FALSE(conflicting.valid);
        }
        auto ambiguous = complete;
        source["source_instance"] = 2U;
        ambiguous.consume(source);
        CHECK_FALSE(ambiguous.valid);
    }
    CHECK(complete.grid_round_trip);
    CHECK(complete.stages.size() == names.size());
}

TEST_CASE("atlas grid transitions belong only to consecutive stages on one allocation", "[workspace][audit]") {
    constexpr std::array names{"fractional", "row1", "row2", "row10", "end", "restored"};
    const auto publish = [](AtlasDrawAudit& audit, nlohmann::json draw) {
        for (const auto* event : {"iced.gallery.source", "iced.surface.sample_acquired", "iced.surface.draw_encoded"}) {
            draw["event"] = event;
            audit.consume(draw);
        }
    };
    AtlasDrawAudit between_stages;
    for (std::size_t index = 0U; index != names.size(); ++index) {
        const std::array<std::uint64_t, 6U> first_rows{0U, 1U, 2U, 10U, 71U, 0U};
        const double top = index == 4U ? -100.0 : (index == 0U ? -37.25 : 0.0);
        auto draw = atlas_draw_record(70U + index, first_rows[index], 4U, top);
        publish(between_stages, draw);
        draw["event"] = "iced.surface.scroll_stage";
        draw["control"] = names[index];
        between_stages.consume(draw);
        if (index == 0U) publish(between_stages, atlas_draw_record(80U, 0U, 5U, -37.25));
    }
    CHECK(between_stages.valid);
    CHECK(between_stages.grid_round_trip);

    for (const auto* scenario : {"constant", "only-grow", "only-shrink", "generation", "surface", "width", "height"}) {
        INFO(scenario);
        const std::string_view kind{scenario};
        const bool replacement = kind == "generation" || kind == "surface" || kind == "width" || kind == "height";
        AtlasDrawAudit audit;
        // These valid draws contain a complete round trip but belong to no stage.
        for (std::uint64_t index = 0U; index != 3U; ++index)
            publish(audit, atlas_draw_record(10U + index, 0U, index == 1U ? 5U : 4U, 0.0));
        CHECK(audit.valid);
        CHECK_FALSE(audit.grid_round_trip);
        for (std::size_t index = 0U; index != names.size(); ++index) {
            std::uint64_t rows = 4U;
            if (kind == "only-grow") rows = index < 2U ? 4U : 5U;
            if (kind == "only-shrink") rows = index < 2U ? 5U : 4U;
            if (replacement) rows = index % 2U == 0U ? 5U : 4U;
            const std::array<std::uint64_t, 6U> first_rows{0U, 1U, 2U, 10U, 75U - rows, 0U};
            const double top = index == 4U ? 500.0 - static_cast<double>(rows) * 150.0 : (index == 0U || index == 2U ? -37.25 : 0.0);
            auto draw = atlas_draw_record(30U + index, first_rows[index], rows, top);
            if (index != 0U) {
                if (kind == "generation") draw["generation"] = 8U;
                if (kind == "surface") draw["surface"] = "000000000000000b000000000000000d";
                if (kind == "width") draw["width"] = 1024U;
                if (kind == "height") draw["height"] = scalar(draw, "height") + 1U;
            }
            publish(audit, draw);
            CHECK(audit.valid);
            draw["event"] = "iced.surface.scroll_stage";
            draw["control"] = names[index];
            audit.consume(draw);
            if (replacement && index == 1U) {
                CHECK_FALSE(audit.valid);
                CHECK(audit.stages.size() == 1U);
                break;
            }
            CHECK(audit.valid);
        }
        CHECK_FALSE(audit.grid_round_trip);
        if (!replacement) {
            CHECK(audit.stages.size() == names.size());
            CHECK(audit.staged_transitions == (kind == "only-grow" ? 1U : (kind == "only-shrink" ? 2U : 0U)));
        }
    }
}

}  // namespace

TEST_CASE("Source retirement remains independent of a retained sample arena", "[workspace][audit]") {
    SurfaceAudit audit;
    for (std::size_t stage = 0U; stage < 15U; ++stage)
        audit.native(native_surface_record(native_surface_events[stage]));
    for (std::size_t stage = 0U; stage < browser_live_stages; ++stage)
        audit.browser(browser_surface_record(browser_surface_events[stage]));
    for (const auto* event : {"presentation.source.withdrawal", "presentation.source.retirement"})
        audit.native(native_surface_record(event));
    for (const auto* event : {"firefox.workspace.source.withdrawal", "firefox.workspace.source.retired"})
        audit.browser(browser_surface_record(event));
    REQUIRE(audit.failure.empty());
    CHECK(audit.surfaces.size() == 1U);
    CHECK(audit.sources.size() == 1U);
    CHECK(audit.evidence_settled());
    audit.browser(browser_surface_record("iced.surface.sample_draw_selected"));
    audit.browser(browser_surface_record("iced.surface.draw_encoded"));
    audit.browser(browser_surface_record("iced.frame.draw_settled"));
    CHECK(audit.failure.empty());
    CHECK(audit.evidence_settled());
    audit.browser(browser_surface_record("firefox.workspace.source.retired"));
    CHECK_FALSE(audit.failure.empty());
}

TEST_CASE("Source withdrawal permits claimed initialization to finish before retirement", "[workspace][audit]") {
    const auto native_early = GENERATE(false, true);
    const auto browser_early = GENERATE(false, true);
    const auto install_native_timeline = GENERATE(false, true);
    SurfaceAudit audit;
    for (const bool browser : {false, true}) {
        const std::array events = browser
            ? std::array{"firefox.workspace.source.admitted", "firefox.workspace.source.claim_outcome",
                         "firefox.workspace.source.ready", "firefox.workspace.source.withdrawal", "firefox.workspace.source.retired"}
            : std::array{"presentation.source.admission.enqueued", "presentation.source.admission.written",
                         "presentation.source.ready", "presentation.source.withdrawal", "presentation.source.retirement"};
        const auto observe = [&](SurfaceAudit& target, const std::size_t stage) {
            if (browser)
                target.browser(browser_surface_record(events[stage]));
            else
                target.native(native_surface_record(events[stage]));
        };
        observe(audit, 0U);
        auto missing_claim = audit;
        observe(missing_claim, 2U);
        CHECK_FALSE(missing_claim.failure.empty());
        observe(audit, 1U);
        auto premature_retirement = audit;
        observe(premature_retirement, 4U);
        CHECK_FALSE(premature_retirement.failure.empty());
        const bool early = browser ? browser_early : native_early;
        if (!browser && !install_native_timeline) {
            observe(audit, 3U);
        } else {
            observe(audit, early ? 3U : 2U);
            observe(audit, early ? 2U : 3U);
        }
        auto duplicate = audit;
        observe(duplicate, 3U);
        CHECK_FALSE(duplicate.failure.empty());
        observe(audit, 4U);
        auto after_retirement = audit;
        observe(after_retirement, 2U);
        CHECK_FALSE(after_retirement.failure.empty());
        REQUIRE(audit.failure.empty());
    }
    CHECK(audit.evidence_settled());
}

TEST_CASE("Source cancellation settles the exact unclaimed admission without a ready product", "[workspace][audit]") {
    const auto omitted = GENERATE("", "failure", "withdrawal", "retirement");
    SurfaceAudit audit;
    for (const auto* event : {"presentation.source.admission.enqueued", "presentation.source.admission.written",
                              "presentation.source.withdrawal", "presentation.source.retirement"})
        audit.native(native_surface_record(event));
    audit.browser(browser_surface_record("firefox.workspace.source.admitted"));
    if (std::string_view{omitted} != "withdrawal")
        audit.browser(browser_surface_record("firefox.workspace.source.withdrawal"));
    auto cancelled = browser_surface_record("firefox.workspace.source.import_failed");
    cancelled["code"] = 1U;
    if (std::string_view{omitted} != "failure") audit.browser(cancelled);
    if (std::string_view{omitted} != "retirement")
        audit.browser(browser_surface_record("firefox.workspace.source.retired"));
    CHECK(audit.evidence_settled() == std::string_view{omitted}.empty());
    if (std::string_view{omitted}.empty()) {
        audit.browser(cancelled);
        CHECK_FALSE(audit.failure.empty());
    }
}

TEST_CASE("Source publication audit requires exact physical receiver receipts", "[workspace][audit]") {
    for (const std::string_view fault : {"none", "missing", "duplicate", "reordered", "arena", "content", "presentation", "transfer",
                                         "source", "allocation", "source-generation", "source-width"}) {
        INFO("physical receipt fault: " << fault);
        SurfaceAudit audit;
        for (const auto* event : native_surface_events) {
            auto record = native_surface_record(event);
            if (std::string_view{event} == "presentation.source.read_submitted") {
                if (fault == "allocation") record["workspace_allocation"] = 999U;
                if (fault == "source") record["workspace_source_low"] = 999U;
            }
            if (std::string_view{event} == "presentation.source.ready") {
                if (fault == "source-generation") record["sequence"] = 8U;
                if (fault == "source-width") record["capacity_width"] = 65U;
            }
            audit.native(record);
        }
        for (const auto* event : browser_surface_events) {
            const std::string_view name{event};
            if (name == "firefox.workspace.copy_completed" && fault == "missing") continue;
            auto record = browser_surface_record(event);
            if (name == "firefox.workspace.frame_forwarded" && fault == "transfer") {
                record["transfer_sequence"] = 2U;
                record["timeline_ready"] = 3U;
                record["timeline_release"] = 4U;
            }
            if (name == "firefox.workspace.copy_completed") {
                if (fault == "arena") record["surface"] = SurfaceAudit::native_identity(native_surface_record("", 13U));
                if (fault == "content") record["content_sequence"] = 24U;
                if (fault == "presentation") record["presentation_revision"] = 2U;
            }
            if (name == "firefox.workspace.frame_dispatched" && fault == "reordered")
                audit.browser(browser_surface_record("firefox.workspace.copy_completed"));
            audit.browser(record);
            if (name == "firefox.workspace.copy_completed" && fault == "duplicate") audit.browser(record);
        }
        CHECK(audit.evidence_settled() == (fault == "none"));
        CHECK(audit.joined_failure().empty() == (fault == "none"));
    }
}

TEST_CASE("Source publication joins survive independent drains and retirement before copy evidence", "[workspace][audit]") {
    SurfaceAudit audit;
    for (const auto* event : browser_surface_events)
        if (std::string_view{event} != "firefox.workspace.copy_completed") audit.browser(browser_surface_record(event));
    for (const auto* event : native_surface_events)
        audit.native(native_surface_record(event));
    for (const auto* event : {"presentation.source.withdrawal", "presentation.source.retirement"})
        audit.native(native_surface_record(event));
    for (const auto* event : {"firefox.workspace.source.withdrawal", "firefox.workspace.source.retired"})
        audit.browser(browser_surface_record(event));
    REQUIRE(audit.failure.empty());
    CHECK_FALSE(audit.evidence_settled());
    audit.SettleScenario();
    CHECK(audit.surfaces.size() == 1U);
    CHECK(audit.sources.size() == 1U);
    audit.browser(browser_surface_record("firefox.workspace.copy_completed"));
    REQUIRE(audit.evidence_settled());
    audit.SettleScenario();
    CHECK(audit.surfaces.empty());
    CHECK(audit.sources.empty());
}

TEST_CASE("Capacity retry preserves logical publication with a new physical source transfer", "[workspace][audit]") {
    SurfaceAudit audit;
    record_source_transfer(audit);
    // This first physical attempt returned the source read without a sample.
    auto released = browser_surface_record("firefox.workspace.frame_released");
    audit.browser(released);
    REQUIRE(audit.evidence_settled());
    for (std::size_t stage = 7U; stage < 15U; ++stage) {
        auto record = native_surface_record(native_surface_events[stage]);
        record["trace_id"] = 32U;
        record["span_id"] = 32U;
        if (stage >= 10U) {
            record["transfer_sequence"] = 2U;
            record["timeline_ready"] = 3U;
        }
        // The newer observation still selects the actual retained revision 23.
        record["source_observation_revision"] = 30U;
        audit.native(record);
    }
    for (std::size_t stage = 9U; stage < browser_live_stages; ++stage) {
        auto record = browser_surface_record(browser_surface_events[stage]);
        record["transfer_sequence"] = 2U;
        record["timeline_ready"] = 3U;
        record["timeline_release"] = 4U;
        audit.browser(record);
    }
    REQUIRE(audit.failure.empty());
    CHECK(audit.evidence_settled());
    audit.browser(released);
    CHECK_FALSE(audit.failure.empty());
}

TEST_CASE("Release-only settlement requires positive exact transfer evidence", "[workspace][audit]") {
    for (const std::string_view fault : {"none", "missing", "wrong-transfer", "wrong-content", "wrong-source"}) {
        SurfaceAudit audit;
        record_source_transfer(audit);
        auto record = browser_surface_record("firefox.workspace.frame_released");
        if (fault == "wrong-transfer") record["transfer_sequence"] = 2U;
        if (fault == "wrong-content") record["content_sequence"] = 24U;
        if (fault == "wrong-source") record["source"] = SurfaceAudit::native_identity(native_surface_record("", 99U));
        if (fault != "missing") audit.browser(record);
        CHECK(audit.evidence_settled() == (fault == "none"));
    }
}

TEST_CASE("Detached pre-metadata release joins physical custody without inventing sample dimensions", "[workspace][audit]") {
    SurfaceAudit audit;
    record_source_transfer(audit);
    audit.browser(browser_surface_record("firefox.workspace.source.withdrawal"));
    auto completion = browser_surface_record("firefox.workspace.frame_released");
    for (const auto* field : {"width", "height", "content_width", "content_height", "layer", "slot", "timeline_ready", "timeline_release"})
        completion.erase(field);
    audit.browser(completion);
    REQUIRE(audit.failure.empty());
    CHECK(audit.evidence_settled());
    const auto& state = audit.surfaces.at(SurfaceAudit::native_identity(native_surface_record("")));
    CHECK(state.receipts.empty());
    CHECK(state.releases.size() == 1U);
    CHECK(state.samples.empty());
}

TEST_CASE("Source admission audit requires canonical allocation provenance", "[workspace][audit]") {
    for (const auto* field : {"sequence", "capacity_width", "capacity_height", "workspace_source_high", "workspace_source_low",
                              "workspace_allocation", "workspace_bytes", "workspace_pitch", "workspace_width", "workspace_height"}) {
        SurfaceAudit audit;
        auto record = native_surface_record("presentation.source.admission.enqueued");
        record.erase(field);
        audit.native(record);
        CHECK_FALSE(audit.failure.empty());
    }
    for (const auto* field : {"arena", "width", "height", "workspace_allocation"}) {
        SurfaceAudit audit;
        auto record = browser_surface_record("firefox.workspace.source.admitted");
        record.erase(field);
        audit.browser(record);
        CHECK_FALSE(audit.failure.empty());
    }
}

TEST_CASE("fixed gallery return rejects any intermediate readiness or ready-pixel loss", "[workspace][audit]") {
    for (const std::string_view defect :
         {"none", "placeholder", "missing-cell", "black-cell", "wrong-cell", "origin", "capacity", "clip"}) {
        INFO(defect);
        AtlasDrawAudit audit;
        constexpr std::array names{"return-cached", "return-aligned", "return-extra", "return-restored"};
        for (std::size_t step = 0U; step < names.size(); ++step) {
            const auto rows = step == 2U ? 6U : 5U;
            auto draw = atlas_draw_record(300U + step, 0U, rows, step == 2U ? -75.0 : 0.0);
            draw["clip"][3] = 712.5;
            std::vector<std::uint64_t> indices(rows * 4U);
            std::iota(indices.begin(), indices.end(), 0U);
            draw["visible_indices"] = indices;
            draw["ready_slots"] = std::vector<bool>(indices.size(), true);
            if (step == 2U && defect == "placeholder") draw["ready_slots"][0] = false;
            auto source = draw;
            source["event"] = "iced.gallery.source";
            audit.consume(source);
            auto acquired = draw;
            acquired["event"] = "iced.surface.sample_acquired";
            audit.consume(acquired);
            if (step == 2U && defect == "origin") draw["row_origin"] = 8U;
            if (step == 2U && defect == "capacity") draw["content_height"] = 600U;
            if (step == 2U && defect == "clip") draw["clip"][3] = 750.0;
            audit.consume(draw);
            for (const auto index : indices) {
                if (step == 2U && defect == "missing-cell" && index == 0U) continue;
                auto pixel = draw;
                pixel["event"] = "integration.atlas_ready_cell";
                pixel["compiled_index"] = index;
                pixel["sampled_pixels"] = 16U;
                pixel["cell_sample_x"] = 50000U;
                pixel["cell_sample_y"] = 50000U;
                pixel["cell_sample_rgba"] = step == 2U && defect == "wrong-cell" && index == 0U ? 0xff806040U : 0xff705030U;
                pixel["overlay_boxes"] = true;
                pixel["overlay_masks"] = true;
                pixel["overlay_labels"] = true;
                pixel["matched"] = !(step == 2U && defect == "black-cell" && index == 0U);
                audit.consume(pixel);
            }
            draw["event"] = "iced.surface.scroll_stage";
            draw["control"] = names[step];
            audit.consume(draw);
        }
        CHECK((audit.valid && audit.return_round_trip) == (defect == "none"));
    }
}

TEST_CASE("physical ledger distinguishes abandonment and rejects obsolete image copies", "[workspace][audit]") {
    for (const std::string_view defect : {"none", "settlement", "early-release", "native-copy", "capture"}) {
        SurfaceAudit audit;
        for (const auto* event : native_surface_events)
            audit.native(native_surface_record(event));
        for (const auto* event : browser_surface_events) {
            if (std::string_view{event} == "iced.frame.draw_settled") {
                if (defect == "early-release") audit.browser(browser_surface_record("iced.frame.sample_released"));
                if (defect == "settlement") continue;
                event = "iced.frame.draw_abandoned";
            }
            audit.browser(browser_surface_record(event));
        }
        if (defect == "native-copy") audit.native(native_surface_record("presentation.copy.started"));
        if (defect == "capture") audit.browser(browser_surface_record("iced.surface.capture_encoded"));
        CHECK(audit.joined_failure().empty() == (defect == "none"));
    }
}

TEST_CASE("read admission checks actual eligible demand and independently clipped neighbor windows", "[workspace][audit]") {
    for (const std::string_view defect : {"none", "immediate", "preferred", "outside"}) {
        NativeAudit audit;
        nlohmann::json admission{{"kind", "gui_runtime"},
                                 {"event", "gallery.read.scheduled"},
                                 {"sequence", 7U},
                                 {"detail", 20U},
                                 {"admission_position", 20U},
                                 {"admission_columns", 4U},
                                 {"admission_first_row", 6U},
                                 {"admission_row_count", 5U},
                                 {"admission_forward", true},
                                 {"admission_tier", 2U},
                                 {"admission_immediate_eligible", 0U},
                                 {"admission_forward_eligible", 0U},
                                 {"admission_backward_eligible", 7U}};
        if (defect == "immediate") admission["admission_immediate_eligible"] = 1U;
        if (defect == "preferred") admission["admission_forward_eligible"] = 1U;
        if (defect == "outside") admission["admission_position"] = 0U;
        audit.consume(admission);
        CHECK(audit.admission_seen);
        CHECK(audit.admission_priority_valid == (defect == "none"));
    }
}
