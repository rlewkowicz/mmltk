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
#include <nlohmann/json.hpp>
#include <optional>
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
#include "src/frameworks/serialization/serialization.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/services/firefox_process_owner.h"
#include "test_fixture.h"

namespace {

// CLEANUP-IGNORE: The Wayland product driver imports its own typed test helpers and hardware-only termination
// vocabulary.
using mmltk::common::io::ScopedFd;
using mmltk::testsupport::arm_timerfd;
using mmltk::testsupport::consume_timerfd;
using mmltk::testsupport::reap_pidfd;
using mmltk::testsupport::ScopedTempDir;

constexpr int kCompiledResolution = 512;
constexpr auto kWaylandAcceptanceDeadline = std::chrono::seconds{90};
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

void prepare_latest_log(const std::filesystem::path& path, const std::string_view description) {
    std::filesystem::create_directories(path.parent_path());
    if (std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) != 0U) {
        const auto history = path.parent_path() / (path.filename().string() + ".history");
        std::filesystem::create_directories(history);
        const auto identity =
            std::to_string(::getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::rename(path, history / (identity + path.extension().string()));
    }
    if (!std::ofstream{path, std::ios::binary | std::ios::trunc})
        throw std::runtime_error("failed to prepare " + std::string{description} + " at " + path.string());
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

[[nodiscard]] std::string bounded_tail(const std::string& value, const std::size_t maximum = 96U * 1024U) {
    return value.size() <= maximum ? value : value.substr(value.size() - maximum);
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
                       const mmltk::backend::data::testsupport::FixtureSpec& fixture, const std::string& viewer_scenario,
                       const bool logging = true, const bool high_dpi = false, const bool h2d = true) {
        const std::string executable_text = executable.string();
        const std::string diagnostics_text = diagnostics.string();
        const std::string runtime_log_text = runtime_log.string();
        const std::string firefox_text = firefox_log.string();
        const std::string dataset_source = mmltk::backend::data::testsupport::dataset_dir(fixture);
        const std::string compiled_directory = mmltk::backend::data::testsupport::compiled_dir(fixture);
        const std::string resolution = std::to_string(kCompiledResolution);
        std::array<int, 2U> control_pipe{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control_pipe.data()) != 0)
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
                (logging ? (::setenv("MMLTK_LOG_LEVEL", "trace", 1) == 0 && ::setenv("MMLTK_LOG_FILE", runtime_log_text.c_str(), 1) == 0 &&
                            ::setenv("MMLTK_GUI_TRACE_FILE", diagnostics_text.c_str(), 1) == 0 &&
                            ::setenv("MMLTK_FIREFOX_LOG_FILE", firefox_text.c_str(), 1) == 0 &&
                            ::setenv("MOZ_LOG",
                                     "WebGPU:5,Widget:5,WidgetVSync:5,WidgetWayland:5,Dmabuf:5,WidgetCompositor:5,"
                                     "nsRefreshDriver:5,PresShell:5",
                                     1) == 0 &&
                            ::setenv("RUST_BACKTRACE", "full", 1) == 0)
                         : (::unsetenv("MMLTK_LOG_LEVEL") == 0 && ::unsetenv("MMLTK_LOG_FILE") == 0 &&
                            ::unsetenv("MMLTK_GUI_TRACE_FILE") == 0 && ::unsetenv("MMLTK_FIREFOX_LOG_FILE") == 0 &&
                            ::unsetenv("MOZ_LOG") == 0 && ::unsetenv("RUST_BACKTRACE") == 0)) &&
                ::unsetenv("MOZ_LOG_FILE") == 0 && ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION", "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DPI", high_dpi ? "1.5" : "1", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_VIEWER_SCENARIO", viewer_scenario.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_PENDING_SUPERSESSION", viewer_scenario == "rapid" && logging ? "1" : "0", 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_DATASET_SOURCE", dataset_source.c_str(), 1) == 0 &&
                ::setenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPILED_DIRECTORY", compiled_directory.c_str(), 1) == 0 &&
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
    struct SurfaceState final {
        std::uint64_t generation = 0U;
        std::uint64_t width = 0U;
        std::uint64_t height = 0U;
        std::uint64_t iced_generation = 0U;
        std::uint64_t browser_width = 0U;
        std::uint64_t browser_height = 0U;
        unsigned native_stage = 0U;
        unsigned firefox_stage = 0U;
        bool import_failed = false;
        bool withdrawn = false;
        bool candidate_withdrawn = false;
        bool native_retired = false;
        bool firefox_withdrawn = false;
        bool firefox_retired = false;
        std::size_t created = 0U;
        std::size_t captured = 0U;
        std::size_t discarded = 0U;
        std::size_t retired = 0U;
        std::optional<Reconstruction> reconstruction;
        std::map<std::uint64_t, unsigned> source_steps;
        std::map<std::uint64_t, std::uint64_t> ended_spans;
        std::map<std::uint64_t, std::uint64_t> publications;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> samples;
    };
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

    void reject(const std::string_view why) {
        if (failure.empty()) failure = why;
    }

    static std::string native_identity(const nlohmann::json& record) {
        std::string result(32U, '0');
        for (const auto& [offset, field] : {std::pair{0U, "surface_high"}, std::pair{16U, "surface_low"}}) {
            std::array<char, 16U> digits{};
            const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), scalar(record, field), 16);
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
        native_shutdown = native_shutdown || event == "shutdown.requested";
        browser_shutdown = browser_shutdown || event == "shutdown.firefox_terminal";
        constexpr std::array transitions{
            "presentation.allocation.created", "presentation.admission.enqueued",    "presentation.admission.written",
            "presentation.import.outcome",     "presentation.source_borrow.started", "presentation.source_borrow.completed",
            "presentation.source.copy",        "presentation.ready_sync.started",    "presentation.ready_sync.completed",
            "presentation.frame.edge",         "presentation.active.withdrawal",     "presentation.candidate.withdrawal",
            "presentation.retirement",         "presentation.release_wait.started",  "presentation.release_wait.completed",
            "presentation.replacement"};
        if (record.value("kind", "") != "gui_runtime" || std::ranges::find(transitions, event) == transitions.end()) return;
        const auto id = native_identity(record);
        if (!record.contains("surface_high") || !record.contains("surface_low") || !valid_identity(id)) {
            reject("native lifecycle record has no physical identity");
            return;
        }
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
                             event == "presentation.source.copy";
        const bool transferred = event == "presentation.ready_sync.started" || event == "presentation.ready_sync.completed" ||
                                 event == "presentation.frame.edge" || event == "presentation.release_wait.started" ||
                                 event == "presentation.release_wait.completed";
        if (copying || transferred) {
            if (scalar(record, "source_revision") != scalar(record, "frame_revision") ||
                scalar(record, "allocation_generation") != generation ||
                scalar(record, "presentation_revision") != scalar(record, "value"))
                reject("native operation mixes source allocation or publication identities");
            if (copying && (scalar(record, "presentation_revision") != 0U || scalar(record, "transfer_sequence") != 0U ||
                            scalar(record, "timeline_ready") != 0U))
                reject("new native copy inherited an incumbent physical publication");
            if (transferred && (scalar(record, "transfer_sequence") == 0U || scalar(record, "timeline_ready") == 0U))
                reject("native physical operation omitted its transfer identity");
        }
        if (state.native_retired) reject("native transition after physical retirement");
        const auto advance = [&](const unsigned from) {
            if (state.native_stage != from) reject("native admission stages are missing, duplicate, or reordered");
            state.native_stage = from + 1U;
        };
        if (event == "presentation.allocation.created")
            advance(0U);
        else if (event == "presentation.admission.enqueued")
            advance(1U);
        else if (event == "presentation.admission.written")
            advance(2U);
        else if (event == "presentation.import.outcome") {
            advance(3U);
            state.import_failed = scalar(record, "value") != 1U;
        } else if (event == "presentation.active.withdrawal" || event == "presentation.candidate.withdrawal") {
            if (state.native_stage != 4U || state.import_failed || state.withdrawn) reject("withdrawal lacks a unique completed import");
            state.withdrawn = true;
            state.candidate_withdrawn = event == "presentation.candidate.withdrawal";
        } else if (event == "presentation.retirement") {
            if (!state.withdrawn && !state.import_failed && !native_shutdown)
                reject("physical retirement lacks withdrawal or terminal shutdown");
            if (scalar(record, "outcome") != 1U) reject("native physical retirement failed");
            state.native_retired = true;
        } else {
            const bool span_end = event == "presentation.source_borrow.completed" ||
                                  event == "presentation.ready_sync.completed" ||
                                  event == "presentation.release_wait.completed";
            if (span_end) {
                const auto span_outcome = scalar(record, "span_outcome");
                state.ended_spans.insert_or_assign(scalar(record, "span_id"), span_outcome);
                if (span_outcome != static_cast<std::uint64_t>(mmltk::controller::contracts::DiagnosticSpanOutcome::Success) ||
                    scalar(record, "outcome") != 1U) return;
            }
            if (event == "presentation.source.copy" && scalar(record, "outcome") != 0U) return;
            auto& steps = state.source_steps[scalar(record, "frame_revision")];
            const auto step = [&](const unsigned required, const unsigned next) {
                if (state.native_stage != 4U || state.import_failed || steps != required)
                    reject("native source/copy/ready stages are incomplete or reordered");
                steps = next;
            };
            if (event == "presentation.source_borrow.started")
                step(steps, 1U);
            else if (event == "presentation.source_borrow.completed")
                step(1U, 3U);
            else if (event == "presentation.source.copy")
                step(3U, 7U);
            else if (event == "presentation.ready_sync.started")
                step(steps == 63U ? 63U : 7U, 15U);
            else if (event == "presentation.ready_sync.completed")
                step(15U, 31U);
            else if (event == "presentation.frame.edge") {
                step(31U, 63U);
                state.publications[scalar(record, "value")] = scalar(record, "frame_revision");
            }
        }
    }

    void browser(const nlohmann::json& record) {
        ++browser_ordinal;
        const std::string event = record.value("event", "");
        if (event == "browser.invalid_webgpu_texture") reject("Firefox reported an invalid WebGPU texture");
        browser_shutdown =
            browser_shutdown || (event == "firefox.workspace.channel_terminal" && record.value("terminal", "") == "orderly_bridge_close");
        if (!event.starts_with("firefox.workspace.") && !event.starts_with("iced.surface.")) return;
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
        else if (event == "firefox.workspace.registry_inserted")
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
        if (!event.starts_with("iced.surface.")) return;
        const auto generation = scalar(record, "generation");
        if (generation == 0U || !record.contains("width") || !record.contains("height") ||
            (state.iced_generation != 0U && state.iced_generation != generation))
            reject("Iced surface provenance is missing or inconsistent");
        state.iced_generation = generation;
        if (state.retired != 0U) reject("Iced surface used after texture retirement");
        if (event == "iced.surface.texture_create") {
            if (state.created != 0U) reject("Iced physical texture created twice");
            state.created = browser_ordinal;
        } else if (event == "iced.surface.owned_capture_submitted" || event == "iced.surface.owned_draw_selected") {
            if (state.created == 0U || state.firefox_stage != 5U) reject("Iced sampled surface lacks texture or complete Firefox import");
            state.samples.emplace_back(scalar(record, "presentation_revision"), scalar(record, "frame_revision"));
            if (event == "iced.surface.owned_capture_submitted")
                state.captured = browser_ordinal;
            else {
                if (state.captured == 0U) reject("Iced selected image without an owned capture");
                if (!valid_identity(record.value("requested_surface", "")))
                    reject("Iced selected draw lacks requested capability identity");
                draws.push_back({id, record.value("requested_surface", ""), browser_ordinal});
            }
        } else if (event == "iced.surface.pending_discarded") {
            if (state.created == 0U || state.captured != 0U || state.discarded != 0U)
                reject("Iced pending discard does not name a unique uncaptured import");
            state.discarded = browser_ordinal;
        } else if (event == "iced.surface.renderer_reconstructed") {
            const std::string requested = record.value("requested_surface", "");
            if (!valid_identity(requested) || requested == id || state.captured == 0U)
                reject("pipeline reconstruction lacks its retained owned image");
            else {
                auto& pending = surfaces[requested];
                if (pending.reconstruction || pending.created == 0U || state.captured >= pending.created || pending.captured != 0U ||
                    pending.discarded != 0U || pending.retired != 0U)
                    reject("renderer reconstruction is duplicate or outside its exact pending ownership window");
                else
                    pending.reconstruction = Reconstruction{id, requested, browser_ordinal};
            }
        } else if (event == "iced.surface.retired") {
            if (state.created == 0U) reject("Iced retirement lacks texture creation");
            state.retired = browser_ordinal;
        }
    }

    [[nodiscard]] std::string joined_failure() const {
        if (!failure.empty()) return failure;
        for (const auto& [id, state] : surfaces) {
            if (state.created == 0U && state.samples.empty() && !state.candidate_withdrawn) continue;
            if (state.native_stage != 4U || state.generation != state.iced_generation || state.width != state.browser_width ||
                state.height != state.browser_height || !state.native_retired)
                return "surface " + id + " lacks matching native/browser import and retirement";
            if (state.import_failed && state.samples.empty()) continue;
            if (state.import_failed || state.firefox_stage != 5U) return "surface " + id + " sampled or withdrew an incomplete import";
            if ((!state.firefox_retired || state.retired == 0U) && !(native_shutdown && browser_shutdown))
                return "surface " + id + " lacks receiver retirement";
            for (const auto& [publication, frame] : state.samples) {
                const auto found = state.publications.find(publication);
                if (publication == 0U || frame == 0U || found == state.publications.end() || found->second != frame)
                    return "sampled surface " + id + " lacks matching native frame publication";
            }
            if (state.discarded != 0U && (state.retired == 0U || !state.withdrawn))
                return "discarded pending surface " + id + " lacks withdrawal and texture retirement";
        }
        return {};
    }

    [[nodiscard]] bool pending_supersession_completed() const {
        if (!joined_failure().empty()) return false;
        for (const auto& [b, pending] : surfaces) {
            if (!pending.candidate_withdrawn || pending.created == 0U || pending.captured != 0U || !pending.publications.empty() ||
                pending.discarded <= pending.created || pending.retired <= pending.discarded || !pending.reconstruction ||
                pending.reconstruction->requested != b || pending.reconstruction->ordinal <= pending.created ||
                pending.reconstruction->ordinal >= pending.discarded || !pending.firefox_retired || !pending.native_retired)
                continue;
            const auto& active_identity = pending.reconstruction->completed;
            bool selected_while_pending = false;
            std::size_t bridge_draw = 0U;
            for (const auto& draw : draws) {
                const auto& active = surfaces.at(draw.selected);
                if (draw.selected == active_identity && active.generation < pending.generation &&
                    draw.ordinal > pending.reconstruction->ordinal && draw.ordinal < pending.discarded && draw.requested == b &&
                    active.captured != 0U && active.captured < pending.created)
                    selected_while_pending = true;
                // Preserve the first bridge: A may keep drawing while C's
                // submitted capture awaits completion and promotion.
                if (bridge_draw == 0U && selected_while_pending && draw.selected == active_identity && draw.ordinal > pending.retired)
                    bridge_draw = draw.ordinal;
                if (bridge_draw != 0U && active.generation > pending.generation && active.captured > bridge_draw &&
                    draw.ordinal > active.captured && active.created > pending.created)
                    return true;
            }
        }
        return false;
    }
};

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
    bool acceptance_first_patch_exact = false;
    bool acceptance_held_read = false;
    bool acceptance_held_completed = false;
    bool acceptance_held_released = false;
    bool causal_inconsistent = false;
    std::string_view causal_failure;
    // CLEANUP-IGNORE: Native process custody begins a distinct evidence group from CUDA render-layout records.
    pid_t firefox_pid = -1;
    // CLEANUP-IGNORE: Presentation and Explore runtime counters are acceptance evidence, not kernel ABI geometry.
    std::uint64_t presentation_timeline = 0U;
    std::uint64_t explore_generation = 0U;
    std::uint64_t explore_nproc = 0U;
    std::uint64_t explore_tiles = 0U;
    std::uint64_t explore_placeholder_first_row = 0U;
    std::uint64_t partial_generation = 0U;
    std::uint64_t explore_stale_count = 0U;
    std::size_t explore_placeholder_ordinal = 0U;
    std::size_t explore_first_patch_ordinal = 0U;
    // CLEANUP-IGNORE: Partial-patch ordering counters are distinct from rendered-card geometry fields.
    std::size_t partial_placeholder_ordinal = 0U;
    std::size_t partial_first_patch_ordinal = 0U;
    std::uint64_t partial_first_tile_count = 0U;
    std::size_t explore_max_pinned = 0U;
    std::size_t ordinal = 0U;
    std::size_t peer_open_count = 0U;
    std::size_t peer_close_count = 0U;
    std::size_t viewport_accept_count = 0U;
    std::size_t viewport_placeholder_count = 0U;
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
    std::map<std::uint64_t, std::size_t> placeholder_ordinals;
    std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> patched_slots;
    std::map<std::uint64_t, std::size_t> last_patch_ordinals;
    std::map<std::uint64_t, std::size_t> first_publication_ordinals;
    std::map<std::uint64_t, std::uint64_t> published_tiles;
    std::map<std::uint64_t, std::size_t> compiled_reads;
    std::map<std::uint64_t, std::size_t> completed_reads;
    std::map<std::uint64_t, std::uint64_t> augmentation_seeds;
    bool augmentation_seed_zero_pixels = false;
    bool augmentation_seed_one_pixels = false;
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
    using ProbeSlots = std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>;
    ProbeSlots rendered_probe_slots;
    ProbeSlots transition_probe_slots;
    ProbeSlots rendered_probe_frames;
    bool donor_descriptors = false;

    void reject_causal_evidence(const std::string_view reason) noexcept {
        causal_inconsistent = true;
        if (causal_failure.empty()) causal_failure = reason;
    }

    void record_complete_probe(ProbeSlots& probes, const ProbeSlots::key_type& key, const ProbeSlots::mapped_type compiled_index,
                               const bool complete, const std::string_view incomplete_reason) {
        if (!complete) {
            reject_causal_evidence(incomplete_reason);
            return;
        }
        if (!probes.contains(key) && probes.size() == kAcceptanceRecordLimit) {
            reject_causal_evidence("rendered probe evidence capacity");
            return;
        }
        if (probes.contains(key) && probes.at(key) != compiled_index) reject_causal_evidence("rendered probe identity changed");
        probes.insert_or_assign(key, compiled_index);
    }

    void consume(const nlohmann::json& record) {
        if (record.value("kind", "") != "gui_runtime") return;
        ++ordinal;
        const std::string owner = record.value("owner", "");
        const std::string event = record.value("event", "");
        const std::uint64_t sequence = scalar(record, "sequence");
        const std::uint64_t value = scalar(record, "value");
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
            ++viewport_accept_count;
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
            explore_placeholder_first_row = value;
            explore_placeholder_ordinal = ordinal;
            placeholder_ordinals.try_emplace(sequence, ordinal);
            viewport_placeholder_count += value != 0U ? 1U : 0U;
            explore_first_patch_ordinal = 0U;
            explore_tiles = 0U;
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
            if (!placeholder_slots.contains(sequence) && placeholder_slots.size() == kAcceptanceGenerationLimit) {
                reject_causal_evidence("placeholder generation capacity");
                return;
            }
            const auto& slots = placeholder_slots[sequence];
            if (cardinality > kAcceptanceSlotLimit || slots.size() != cardinality ||
                (placeholder_cardinalities.contains(sequence) && placeholder_cardinalities.at(sequence) != cardinality))
                reject_causal_evidence("placeholder cardinality");
            if (cardinality <= kAcceptanceSlotLimit) placeholder_cardinalities.insert_or_assign(sequence, cardinality);
        }
        if (owner == "explore" && event == "acceptance.compiled.read.started") {
            if (compiled_reads.contains(sequence) || compiled_reads.size() < kAcceptanceGenerationLimit)
                ++compiled_reads[sequence];
            else
                reject_causal_evidence("compiled-read generation capacity");
        }
        if (owner == "explore" && event == "acceptance.compiled.read.completed") {
            if (completed_reads.contains(sequence) || completed_reads.size() < kAcceptanceGenerationLimit)
                ++completed_reads[sequence];
            else
                reject_causal_evidence("completed-read generation capacity");
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
                explore_stale_patch || placeholder == placeholder_slots[sequence].end() || placeholder->second != compiled_index;
        }
        if (owner == "explore" && event == "acceptance.completion.held") {
            acceptance_held_read = true;
            acceptance_held_completed = completed_reads[sequence] != 0U;
            held_generation = sequence;
            held_slot = value;
            held_compiled_index = scalar(record, "detail");
            held_capacity = scalar(record, "staging_bytes");
            held_ordinal = ordinal;
        }
        if (owner == "explore" && event == "acceptance.completion.released" && sequence == held_generation && value == held_slot &&
            scalar(record, "detail") == held_compiled_index) {
            acceptance_held_released = true;
            held_release_ordinal = ordinal;
        }
        if (owner == "explore" && event == "tile.batch.published" && sequence == explore_generation) {
            if (value != 0U && placeholder_ordinals.contains(sequence)) {
                first_publication_ordinals.try_emplace(sequence, ordinal);
                published_tiles.insert_or_assign(sequence, value);
            }
            if (explore_first_patch_ordinal == 0U) {
                explore_first_patch_ordinal = ordinal;
                const auto cardinality = placeholder_cardinalities.find(sequence);
                if (partial_generation == 0U && value != 0U && cardinality != placeholder_cardinalities.end() &&
                    value < cardinality->second && patched_slots[sequence].size() == value) {
                    partial_generation = sequence;
                    partial_placeholder_ordinal = explore_placeholder_ordinal;
                    partial_first_patch_ordinal = ordinal;
                    partial_first_tile_count = value;
                    explore_partial_patch = true;
                    acceptance_first_patch_exact = true;
                }
            }
            explore_ready_batch = explore_ready_batch || (sequence == partial_generation && ordinal > partial_first_patch_ordinal &&
                                                          value > partial_first_tile_count);
            explore_tile_regressed = explore_tile_regressed || value <= explore_tiles;
            explore_tiles = std::max(explore_tiles, value);
            explore_max_pinned = std::max(explore_max_pinned, static_cast<std::size_t>(scalar(record, "staging_bytes")));
        }
        explore_stale_patch = explore_stale_patch || (owner == "explore" && event == "tile.batch.published" && explore_generation != 0U &&
                                                      sequence != explore_generation);
        explore_stale_discard =
            explore_stale_discard || (owner == "explore" && event == "thumbnail.stale.discarded" && sequence != 0U && value != 0U);
        if (owner == "explore" && event == "acceptance.stale.read.discarded" && sequence == held_generation && value == held_slot &&
            scalar(record, "detail") == held_compiled_index && scalar(record, "staging_bytes") == held_capacity &&
            held_discard_ordinal == 0U)
            held_discard_ordinal = ordinal;
        if (owner == "explore" && event == "thumbnail.stale.discarded") explore_stale_count += value;
        if (owner == "explore" && event == "explore.augmentation.batch.prepared") {
            if (augmentation_seeds.contains(sequence) || augmentation_seeds.size() < kAcceptanceGenerationLimit)
                augmentation_seeds.insert_or_assign(sequence, scalar(record, "detail"));
            else
                reject_causal_evidence("augmentation seed capacity");
        }
        if (owner == "explore" && event == "explore.image.pixel_checksum" && (scalar(record, "staging_bytes") & 7U) == 7U) {
            const auto seed = scalar(record, "capacity_width");
            const bool has_pixels = scalar(record, "detail") != 0U;
            augmentation_seed_zero_pixels = augmentation_seed_zero_pixels || (seed == 0U && has_pixels);
            augmentation_seed_one_pixels = augmentation_seed_one_pixels || (seed == 1U && has_pixels);
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
            const auto content_x = (packed >> 48U) & 0xffffU;
            const auto content_y = (packed >> 32U) & 0xffffU;
            const auto content_width = (packed >> 16U) & 0xffffU;
            const auto content_height = packed & 0xffffU;
            const auto card_width = scalar(record, "capacity_width");
            const auto card_height = scalar(record, "capacity_height");
            const auto right_padding = card_width >= content_x + content_width ? card_width - content_x - content_width
                                                                               : std::numeric_limits<std::uint64_t>::max();
            const auto bottom_padding = card_height >= content_y + content_height ? card_height - content_y - content_height
                                                                                  : std::numeric_limits<std::uint64_t>::max();
            const bool vertical_padding = content_x == 0U && content_width == card_width && content_y != 0U && bottom_padding != 0U &&
                                          std::max(content_y, bottom_padding) - std::min(content_y, bottom_padding) <= 1U;
            const bool horizontal_padding = content_y == 0U && content_height == card_height && content_x != 0U && right_padding != 0U &&
                                            std::max(content_x, right_padding) - std::min(content_x, right_padding) <= 1U;
            if (card_width != 0U && card_width == card_height && content_width != 0U && content_height != 0U &&
                (vertical_padding != horizontal_padding)) {
                const auto key = std::pair{sequence, value};
                const auto compiled_index = scalar(record, "detail");
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
            const bool complete_probe = scalar(record, "capacity_width") == 31U && scalar(record, "capacity_height") != 0U &&
                                        padded_card_slots.contains(key) && padded_card_slots.at(key) == compiled_index &&
                                        padding_pixels != 0U && ((packed >> 16U) & 0xffffU) != 0U && (packed & 0xffffU) != 0U;
            record_complete_probe(rendered_probe_slots, key, compiled_index, complete_probe, "rendered card probe incomplete");
        }
        if (owner == "explore" && event == "explore.card.transition_probe") {
            const auto key = std::pair{sequence, value};
            const auto compiled_index = scalar(record, "detail");
            const auto expected = scalar(record, "staging_bytes");
            const bool complete_transition = expected != 0U && scalar(record, "capacity_width") >= expected / 2U &&
                                             scalar(record, "capacity_width") <= expected &&
                                             scalar(record, "capacity_height") == expected && padded_card_slots.contains(key) &&
                                             padded_card_slots.at(key) == compiled_index;
            record_complete_probe(transition_probe_slots, key, compiled_index, complete_transition, "rendered transition probe incomplete");
        }
        if (owner == "explore" && event == "explore.frame.published") {
            const bool has_pending_probe = std::ranges::any_of(rendered_probe_slots, [&](const auto& probe) {
                return probe.first.first == sequence && !rendered_probe_frames.contains(probe.first);
            });
            if (!has_pending_probe) { return; }
            if (value == 0U || !patched_slots.contains(sequence)) {
                reject_causal_evidence("rendered probe frame lacks patched generation");
            } else {
                for (const auto& [key, compiled_index] : rendered_probe_slots) {
                    if (key.first != sequence || rendered_probe_frames.contains(key)) continue;
                    const auto patched = patched_slots.at(sequence).find(key.second);
                    const bool exact = patched != patched_slots.at(sequence).end() && patched->second == compiled_index &&
                                       transition_probe_slots.contains(key) && transition_probe_slots.at(key) == compiled_index;
                    if (!exact || rendered_probe_frames.size() == kAcceptanceRecordLimit)
                        reject_causal_evidence("rendered probe frame identity");
                    else
                        rendered_probe_frames.emplace(key, value);
                }
            }
        }
        donor_descriptors = donor_descriptors || (owner == "explore" && event == "explore.donor.descriptors.prepared" && value != 0U &&
                                                  scalar(record, "detail") != 0U && scalar(record, "capacity_width") > value &&
                                                  scalar(record, "capacity_height") > scalar(record, "detail"));
        annotation_copied = annotation_copied || (owner == "annotation" && event == "copy.completed");
        annotation_opened = annotation_opened || (owner == "annotation" && event == "document.opened");
        annotation_edited = annotation_edited || (owner == "annotation" && event == "document.edited");
        if (owner == "presentation" && event == "timeline.ready" && value != 0U) {
            presentation_ready = true;
            presentation_timeline = std::max(presentation_timeline, value);
        }
        if (owner == "firefox_process" && event == "child.spawned" &&
            sequence <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
            firefox_pid = static_cast<pid_t>(sequence);
    }

    [[nodiscard]] std::optional<FinalCursorGenerations> final_generations_for(
        const std::map<std::uint64_t, std::uint64_t>& rendered_slots) const noexcept {
        if (accepted_viewports.empty() || rendered_slots.empty()) return std::nullopt;
        const auto cursor =
            std::ranges::find_if(accepted_viewports.rbegin(), accepted_viewports.rend(), [this, &rendered_slots](const auto& accepted) {
                const auto placeholder = placeholder_slots.find(accepted.generation);
                return placeholder != placeholder_slots.end() && placeholder_cardinalities.contains(accepted.generation) &&
                       placeholder_cardinalities.at(accepted.generation) == placeholder->second.size() &&
                       placeholder->second == rendered_slots;
            });
        if (cursor == accepted_viewports.rend()) return std::nullopt;
        for (auto placeholder = placeholder_slots.lower_bound(cursor->generation);
             placeholder != placeholder_slots.end() && placeholder->second == rendered_slots; ++placeholder) {
            const auto generation = placeholder->first;
            const auto patched = patched_slots.find(generation);
            if (placeholder_cardinalities.contains(generation) && placeholder_cardinalities.at(generation) == placeholder->second.size() &&
                patched != patched_slots.end() && patched->second == placeholder->second &&
                first_publication_ordinals.contains(generation) && published_tiles.contains(generation) &&
                published_tiles.at(generation) == placeholder->second.size())
                return FinalCursorGenerations{.material = generation, .cursor = cursor->generation};
        }
        for (auto accepted = cursor; accepted != accepted_viewports.rend(); ++accepted) {
            const auto generation = accepted->generation;
            const auto placeholder = placeholder_slots.find(generation);
            if (placeholder == placeholder_slots.end()) continue;
            if (!placeholder_cardinalities.contains(generation) || placeholder_cardinalities.at(generation) != placeholder->second.size() ||
                placeholder->second != rendered_slots)
                break;
            const auto patched = patched_slots.find(generation);
            if (patched != patched_slots.end() && patched->second == placeholder->second &&
                first_publication_ordinals.contains(generation) && published_tiles.contains(generation) &&
                published_tiles.at(generation) == placeholder->second.size())
                return FinalCursorGenerations{.material = generation, .cursor = cursor->generation};
        }
        return std::nullopt;
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

    [[nodiscard]] bool held_stale_read_discarded() const noexcept {
        return acceptance_held_released && held_generation != 0U && held_capacity != 0U && held_release_ordinal > held_ordinal &&
               held_discard_ordinal > held_release_ordinal;
    }

    [[nodiscard]] bool superseding_placeholder_observed(const std::size_t before = std::numeric_limits<std::size_t>::max()) const noexcept {
        return acceptance_held_read && std::ranges::any_of(placeholder_ordinals, [this, before](const auto& placeholder) {
                   const auto accepted = accepted_generations.find(placeholder.first);
                   const auto old_patch = last_patch_ordinals.find(held_generation);
                   return placeholder.first > held_generation && accepted != accepted_generations.end() &&
                          accepted->second > held_ordinal && accepted->second < placeholder.second &&
                          (old_patch == last_patch_ordinals.end() || old_patch->second < placeholder.second) && placeholder.second < before;
               });
    }

    [[nodiscard]] std::string_view causal_stale_blocker(const FinalCursorGenerations final) const noexcept {
        if (!acceptance_held_read) return "held read";
        if (!acceptance_held_completed) return "held read completion";
        if (!held_stale_read_discarded()) return "released held read discard";
        if (!compiled_reads.contains(held_generation) || compiled_reads.at(held_generation) == 0U) return "held compiled read";
        if (!completed_reads.contains(held_generation) || completed_reads.at(held_generation) == 0U) return "held completed read";
        if (completed_reads.at(held_generation) > compiled_reads.at(held_generation)) return "held read count";
        if (final.material <= held_generation || final.cursor <= held_generation) return "final generation order";
        if (!accepted_generations.contains(final.cursor)) return "final cursor acceptance";
        if (!placeholder_slots.contains(final.material)) return "final material placeholder";
        if (!patched_slots.contains(final.material) || patched_slots.at(final.material) != placeholder_slots.at(final.material))
            return "final material patches";
        if (!first_publication_ordinals.contains(final.material) || !published_tiles.contains(final.material) ||
            published_tiles.at(final.material) != placeholder_slots.at(final.material).size())
            return "final material publication";
        const auto& material_slots = placeholder_slots.at(final.material);
        const auto first_generation = std::min(final.material, final.cursor);
        const auto last_generation = std::max(final.material, final.cursor);
        for (const auto& accepted : accepted_viewports) {
            const auto generation = accepted.generation;
            if (generation < first_generation || generation > last_generation) continue;
            const auto placeholder = placeholder_slots.find(generation);
            if (placeholder == placeholder_slots.end()) continue;
            if (placeholder->second != material_slots || !placeholder_cardinalities.contains(generation) ||
                placeholder_cardinalities.at(generation) != placeholder->second.size() ||
                (patched_slots.contains(generation) && patched_slots.at(generation) != material_slots))
                return "final cursor continuity";
        }
        if (!superseding_placeholder_observed(held_release_ordinal)) return "pre-release superseding placeholder";
        return {};
    }

    [[nodiscard]] bool causal_stale_chain(const FinalCursorGenerations final) const noexcept { return causal_stale_blocker(final).empty(); }

    [[nodiscard]] bool exact_partial_slot_identity() const noexcept {
        const auto placeholder = placeholder_slots.find(partial_generation);
        const auto patched = patched_slots.find(partial_generation);
        const auto published = published_tiles.find(partial_generation);
        // A newer viewport may supersede unfinished slots or an unpublished batch.
        return placeholder != placeholder_slots.end() && patched != patched_slots.end() && published != published_tiles.end() &&
               published->second > 1U && published->second <= patched->second.size() &&
               std::ranges::includes(placeholder->second, patched->second);
    }

    [[nodiscard]] bool seeded_augmentation_pixels_published() const noexcept {
        const auto prepared = [this](const std::uint64_t seed) {
            return std::ranges::any_of(augmentation_seeds, [seed](const auto& generation) { return generation.second == seed; });
        };
        return prepared(0U) && prepared(1U) && augmentation_seed_zero_pixels && augmentation_seed_one_pixels;
    }

    [[nodiscard]] bool aligned_rendered_probe(const std::pair<std::uint64_t, std::uint64_t> key) const noexcept {
        const auto rendered = rendered_probe_slots.find(key);
        const auto selected = selected_overlay_slots.find(key);
        const auto patched_generation = patched_slots.find(key.first);
        return rendered != rendered_probe_slots.end() && padded_card_slots.contains(key) && padding_orientations.contains(key) &&
               padded_card_slots.at(key) == rendered->second && transition_probe_slots.contains(key) &&
               transition_probe_slots.at(key) == rendered->second && rendered_probe_frames.contains(key) &&
               rendered_probe_frames.at(key) != 0U && patched_generation != patched_slots.end() &&
               patched_generation->second.contains(key.second) && patched_generation->second.at(key.second) == rendered->second &&
               selected != selected_overlay_slots.end() && selected->second.second == rendered->second &&
               transformed_overlay_slots.contains(key) && semantic_overlay_slots.contains(key);
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

    [[nodiscard]] std::string_view overlay_readiness_blocker() const noexcept {
        if (!overlay_descriptors) return "overlay descriptors";
        if (!donor_descriptors) return "donor descriptors";
        if (!filtered_overlay_identity()) return "filtered overlay identity";
        if (!aligned_padding_orientation(PaddingOrientation::Vertical)) return "vertical rendered overlay";
        if (!aligned_padding_orientation(PaddingOrientation::Horizontal)) return "horizontal rendered overlay";
        return {};
    }

    [[nodiscard]] std::string_view readiness_blocker(const FinalCursorGenerations final) const noexcept {
        const auto overlay_blocker = overlay_readiness_blocker();
        const std::array checks{
            std::pair{server_started && peer_opened && firefox_pid > 0, std::string_view{"browser peer"}},
            std::pair{explore_rendered && explore_placeholder, std::string_view{"Explore publication"}},
            std::pair{annotation_copied && annotation_opened && annotation_edited, std::string_view{"Annotation lifecycle"}},
            std::pair{presentation_ready, std::string_view{"Presentation timeline"}},
            std::pair{explore_partial_patch && acceptance_first_patch_exact && explore_ready_batch,
                      std::string_view{"incremental Explore publication"}},
            std::pair{explore_stale_discard, std::string_view{"stale thumbnail discard"}},
            std::pair{causal_stale_chain(final),
                      causal_stale_blocker(final).empty() ? std::string_view{"causal stale-read chain"} : causal_stale_blocker(final)},
            std::pair{exact_partial_slot_identity(), std::string_view{"partial-slot identity"}},
            std::pair{overlay_blocker.empty(), overlay_blocker},
            std::pair{seeded_augmentation_pixels_published(), std::string_view{"seeded augmentation pixel publication"}},
            std::pair{viewport_accept_count > viewport_placeholder_count, std::string_view{"viewport coalescing"}},
            std::pair{!worker_failed && !invalid_message && !peer_replaced && !interaction_rejected, std::string_view{"runtime validity"}},
            std::pair{!explore_tile_regressed && !explore_nproc_changed && !explore_stale_patch && !causal_inconsistent,
                      std::string_view{"Explore causal validity"}},
            std::pair{peer_close_count == 0U, std::string_view{"open browser peer"}},
        };
        const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return !check.first; });
        return blocker == checks.end() ? std::string_view{} : blocker->second;
    }

    [[nodiscard]] bool product_completed(const FinalCursorGenerations final) const noexcept {
        return server_started && peer_opened && firefox_pid > 0 && explore_rendered && annotation_copied && annotation_opened &&
               annotation_edited && presentation_ready && explore_placeholder && explore_partial_patch && acceptance_first_patch_exact &&
               explore_ready_batch && explore_stale_discard && causal_stale_chain(final) && exact_partial_slot_identity() &&
               overlay_descriptors && donor_descriptors && aligned_overlay_pixels() && seeded_augmentation_pixels_published() &&
               viewport_accept_count > viewport_placeholder_count && !worker_failed && !explore_tile_regressed && !explore_nproc_changed &&
               !explore_stale_patch && !invalid_message && !peer_replaced && !interaction_rejected && !causal_inconsistent;
    }

    [[nodiscard]] bool product_ready(const FinalCursorGenerations final) const noexcept {
        return product_completed(final) && peer_close_count == 0U;
    }

    [[nodiscard]] bool failed_before_termination() const noexcept {
        return worker_failed || invalid_message || peer_replaced || peer_close_count != 0U || interaction_rejected || causal_inconsistent;
    }

    [[nodiscard]] std::string_view failure_blocker() const noexcept {
        const std::array checks{
            std::pair{worker_failed, std::string_view{"worker failure"}},
            std::pair{invalid_message, std::string_view{"invalid browser message"}},
            std::pair{peer_replaced, std::string_view{"browser peer replacement"}},
            std::pair{peer_close_count != 0U, std::string_view{"browser peer closed"}},
            std::pair{interaction_rejected, std::string_view{"browser interaction rejected"}},
            std::pair{causal_inconsistent, causal_failure.empty() ? std::string_view{"causal evidence inconsistency"} : causal_failure},
        };
        const auto blocker = std::ranges::find_if(checks, [](const auto& check) { return check.first; });
        return blocker == checks.end() ? std::string_view{} : blocker->second;
    }
};

struct AtlasDrawAudit final {
    using SourceKey = std::array<std::uint64_t, 4U>;
    using CaptureKey = std::tuple<std::string, std::uint64_t, std::uint64_t>;
    using AllocationKey = std::tuple<std::string, std::uint64_t, std::uint64_t, std::uint64_t>;
    static constexpr std::initializer_list<const char*> source_fields{
        "content_session", "source_kind", "source_instance", "source_revision", "content_width", "content_height",
        "dataset_identity", "gallery_generation", "columns", "rows", "first_row", "matching_count", "visible_indices"};
    std::map<SourceKey, nlohmann::json> sources;
    std::map<std::pair<std::uint64_t, std::uint64_t>, SourceKey> sessions;
    std::map<CaptureKey, nlohmann::json> captures;
    std::set<std::uint64_t> drawn_rows;
    std::optional<AllocationKey> staged_allocation;
    std::uint64_t previous_staged_rows = 0U;
    unsigned staged_transitions = 0U;
    std::optional<nlohmann::json> last_draw;
    std::set<std::string> stages;
    bool grid_round_trip = false;
    bool seen = false;
    bool valid = true;

    [[nodiscard]] static SourceKey source_key(const nlohmann::json& record) {
        return {scalar(record, "content_session"), scalar(record, "source_kind"), scalar(record, "source_instance"),
                scalar(record, "source_revision")};
    }

    [[nodiscard]] static CaptureKey capture_key(const nlohmann::json& record) {
        return {record.value("surface", ""), scalar(record, "generation"), scalar(record, "presentation_revision")};
    }

    [[nodiscard]] static bool same_fields(const nlohmann::json& left, const nlohmann::json& right,
                                          const std::initializer_list<const char*> fields) {
        return std::ranges::all_of(
            fields, [&](const auto* field) { return left.contains(field) && right.contains(field) && left[field] == right[field]; });
    }

    void stage(const nlohmann::json& record) {
        constexpr std::array names{"fractional", "row1", "row2", "row10", "end", "restored"};
        const auto name = record.value("control", "");
        bool matching =
            stages.size() < names.size() && name == names[stages.size()] && last_draw &&
            same_fields(record, *last_draw,
                        {"surface",         "generation",  "width",           "height",          "presentation_revision", "frame_revision",
                         "content_session", "source_kind", "source_instance", "source_revision", "content_width",         "content_height",
                         "columns",         "rows",        "first_row",       "matching_count",  "visible_indices",       "bounds",
                         "image",           "clip"});
        const AllocationKey allocation{record.value("surface", ""), scalar(record, "generation"), scalar(record, "width"),
                                       scalar(record, "height")};
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
            staged_allocation = allocation;
            const auto rows = scalar(record, "rows");
            if (previous_staged_rows == 4U && rows == 5U) staged_transitions |= 1U;
            if (previous_staged_rows == 5U && rows == 4U) staged_transitions |= 2U;
            grid_round_trip = staged_transitions == 3U;
            previous_staged_rows = rows;
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
        if (event == "iced.gallery.source") {
            const auto key = source_key(record);
            const auto session = std::pair{key[0], key[3]};
            const auto existing = sources.find(key);
            if (std::ranges::any_of(key, [](const auto value) { return value == 0U; })) {
                valid = false;
            } else if (existing != sources.end()) {
                // Selection and completion can observe the same pixels with
                // newer readiness metadata. Their source geometry must agree.
                valid = valid && same_fields(record, existing->second, source_fields);
            } else if (sources.size() >= kAcceptanceRecordLimit || sessions.contains(session)) {
                valid = false;
            } else {
                sources.emplace(key, record);
                sessions.emplace(session, key);
            }
            return;
        }
        if (event == "iced.surface.owned_capture_submitted") {
            const auto key = capture_key(record);
            if (captures.size() >= kAcceptanceRecordLimit || captures.contains(key)) {
                valid = false;
                return;
            }
            captures.emplace(key, record);
            return;
        }
        if (event == "iced.surface.scroll_stage") {
            stage(record);
            return;
        }
        if (record.value("control", "") != kExploreGalleryControl) return;
        if (event == "iced.surface.owned_draw_clipped" || event == "iced.surface.owned_draw_rejected") {
            last_draw.reset();
            return;
        }
        if (event != "iced.surface.draw_encoded") return;
        const auto source = sources.find(source_key(record));
        const auto capture = captures.find(capture_key(record));
        const auto columns = scalar(record, "columns"), rows = scalar(record, "rows");
        const auto width = scalar(record, "content_width"), height = scalar(record, "content_height");
        bool matching = source != sources.end() && capture != captures.end() && visible(record) &&
                        SurfaceAudit::valid_identity(record.value("surface", "")) && scalar(record, "generation") != 0U &&
                        scalar(record, "presentation_revision") != 0U && width <= scalar(record, "width") &&
                        height <= scalar(record, "height") && scalar(record, "source_revision") == scalar(record, "frame_revision") &&
                        columns != 0U && rows != 0U && width != 0U && height != 0U && width % columns == 0U && height % rows == 0U &&
                        width / columns == height / rows;
        if (matching) {
            matching = same_fields(record, source->second, source_fields) &&
                       same_fields(record, capture->second,
                                   {"content_session", "source_kind", "source_instance", "source_revision", "surface", "generation",
                                    "width", "height", "presentation_revision", "frame_revision", "layer", "slot", "content_width",
                                    "content_height", "columns", "rows", "first_row", "matching_count", "visible_indices"});
            const auto& image = record["image"];
            const auto& bounds = record["bounds"];
            matching = matching &&
                       std::abs(image[2].get<double>() / static_cast<double>(columns) -
                                image[3].get<double>() / static_cast<double>(rows)) < 0.01 &&
                       image[0] == bounds[0] && image[1] == bounds[1] && image[2] == bounds[2];
        }
        valid = valid && matching;
        if (matching) {
            seen = true;
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
    // CLEANUP-IGNORE: Rendered slot-frame identity precedes cursor facts; later booleans track workflow outcomes.
    std::map<std::uint64_t, std::uint64_t> explore_slot_frames;
    std::uint64_t final_cursor_revision = 0U;
    std::uint64_t final_cursor_frame_revision = 0U;
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
    bool padded_detail_drawn = false;
    bool original_detail_drawn = false;
    bool ui_scale_drag = false;
    bool ui_scale_released = false;
    bool ui_scale_restored = false;
    std::set<std::string, std::less<>> ui_scale_pointer_stages;
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
    std::size_t progress_ordinal = 0U;
    std::size_t dataset_complete_ordinal = 0U;
    std::uint64_t compiled_images = 0U;
    std::uint64_t compiled_width = 0U;
    std::uint64_t compiled_height = 0U;
    std::uint64_t presentation_receipt = 0U;
    std::uint64_t final_scroll_row = 0U;

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
        } else if (surface_event == "iced.surface.owned_draw_missing") {
            const auto control = record.value("control", "");
            owned_atlas_interrupted =
                owned_atlas_interrupted || (owned_atlas_current && (control.empty() || control == kExploreGalleryControl));
        }
        ++ordinal;
        const std::string event = record.value("event", "");
        if (event == "integration.control_bounds") {
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
            final_scroll_row = scalar(record, "b");
        } else if (event == "integration.explore_detail") {
            detail = scalar(record, "b") != 0U;
        } else if (event == "integration.explore_augmentation") {
            augmentation_enabled =
                augmentation_enabled || (record.value("detail", "") == "enabled-rendered-seed-zero" &&
                                         scalar(record, "b") > scalar(record, "a") && scalar(record, "d") > scalar(record, "c"));
            augmentation_rerolled =
                augmentation_rerolled || (record.value("detail", "") == "rerolled-distinct-seed" &&
                                          scalar(record, "b") > scalar(record, "a") && scalar(record, "d") > scalar(record, "c"));
        } else if (event == "integration.explore_reshuffle") {
            reshuffle_order_only = record.value("detail", "") == "order-only" && scalar(record, "b") > scalar(record, "a") &&
                                   scalar(record, "c") == scalar(record, "d");
        } else if (event == "integration.annotation_pixel" || event == "integration.annotation_swatch" ||
                   event == "integration.annotation_capability") {
            const auto expected = record.value("expected", std::vector<double>{});
            const auto observed = record.value("observed", std::vector<double>{});
            const double tolerance = event == "integration.annotation_pixel" ? 24.0 : 3.0;
            bool matched = record.value("matched", false) && expected.size() == 3U && observed.size() >= 3U;
            if (matched) {
                double gain = 1.0;
                double minimum = 0.0;
                const double peak = std::max({observed[0], observed[1], observed[2]});
                const double low = std::min({observed[0], observed[1], observed[2]});
                const double scale = event == "integration.annotation_pixel" ? record.value("source_to_screen", 0.0) : 1.0;
                matched = matched && std::isfinite(scale) && scale > 0.0;
                if (event == "integration.annotation_pixel" && scale > 0.0 && scale < 1.0 &&
                    std::ranges::max(expected) == 255.0 && std::ranges::min(expected) == 0.0 && peak - low >= 127.5) {
                    minimum = low;
                    gain = 255.0 / (peak - low);
                }
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    matched = matched && std::isfinite(expected[channel]) && std::isfinite(observed[channel]) &&
                              expected[channel] >= 0.0 && expected[channel] <= 255.0 && observed[channel] >= 0.0 &&
                              observed[channel] <= 255.0 &&
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
            if (scalar(record, "b") >= scalar(record, "a") + 2U && scalar(record, "c") != 0U) ++annotation_previews;
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
            padded_detail_drawn = padded_detail_drawn || (scalar(record, "a") == 512U && scalar(record, "b") == 512U &&
                                                          std::abs(numeric(record, "c") / numeric(record, "d") - 1.0) < 0.01);
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
            final_cursor_slot_count = scalar(record, "d");
        } else if (event == "integration.explore_pointer_inverse") {
            pointer_inverse = record.value("detail", "") == "rendered-grid-slot" && scalar(record, "a") == scalar(record, "b") &&
                              scalar(record, "d") != 0U;
            pointer_slot = scalar(record, "a");
            pointer_compiled_index = scalar(record, "c");
            pointer_revision = scalar(record, "d");
        } else if (event == "integration.explore_pointer_scheduled") {
            pointer_frame_revision = scalar(record, "a");
        } else if (event == "integration.surface_click_dispatched" && record.value("control", "") == kExploreGalleryControl) {
            const auto requested_frame_revision = pointer_frame_revision;
            pointer_frame_revision = scalar(record, "a");
            pointer_dispatched = record.value("detail", "") == "real-canvas-pointer" && requested_frame_revision != 0U &&
                                 pointer_frame_revision >= requested_frame_revision && surface_draws.contains(scalar(record, "b")) &&
                                 surface_scales.contains(std::pair{scalar(record, "b"), pointer_frame_revision}) &&
                                 numeric(record, "c") > 0.0 && numeric(record, "d") > 0.0;
        } else if (event == "integration.explore_pointer_selected") {
            pointer_selected = record.value("detail", "") == "selected-from-dispatched-pointer" &&
                               scalar(record, "a") == pointer_revision && scalar(record, "b") == pointer_slot &&
                               scalar(record, "c") == pointer_compiled_index && scalar(record, "c") == scalar(record, "d") &&
                               explore_slots.contains(pointer_revision) && explore_slots.at(pointer_revision).contains(pointer_slot) &&
                               explore_slots.at(pointer_revision).at(pointer_slot) == pointer_compiled_index &&
                               observed_frame_revision_for_slots(explore_slots.at(pointer_revision), pointer_frame_revision);
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
        } else if (event == "integration.failed") {
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
        const bool padded_and_original_detail = padded_detail_drawn && original_detail_drawn;
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
            "presentation completion", bounds_valid && !failed && !panic && !workspace_protocol_failure, "browser validity",
            firefox_import && firefox_claim && firefox_ready, "Firefox integration",
            std::ranges::all_of(expected, [this](const std::string_view id) { return controls.contains(id); }), "expected controls");
    }

    [[nodiscard]] bool product_ready() const { return readiness_blocker().empty(); }

    [[nodiscard]] bool terminal_evidence_settled() const noexcept {
        return complete && presentation_receipt != 0U && surface_redraw_counts.contains(presentation_receipt) &&
               surface_redraw_counts.at(presentation_receipt) >= 3U;
    }

    [[nodiscard]] bool rendered_frame_for_slots(const std::map<std::uint64_t, std::uint64_t>& native_slots,
                                                const std::size_t minimum_redraws) const noexcept {
        for (const auto& [snapshot_revision, slots] : explore_slots) {
            if (slots != native_slots || !explore_slot_frames.contains(snapshot_revision)) continue;
            const auto source = explore_slot_frames.at(snapshot_revision);
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
        if (final_cursor_revision == 0U || final_cursor_frame_revision == 0U || final_cursor_slot_count == 0U ||
            final_cursor_slot_count > kAcceptanceSlotLimit || slots == explore_slots.end() ||
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
    JsonLineCursor(std::filesystem::path path, const std::uintmax_t offset) : path_(std::move(path)), offset_(offset) {}

    template <class Audit, class Observer>
    void consume(Audit& audit, Observer&& observer) {
        std::ifstream input{path_, std::ios::binary};
        if (!input) return;
        input.seekg(static_cast<std::streamoff>(offset_));
        std::string appended{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        offset_ += appended.size();
        pending_.append(appended);
        std::size_t begin = 0U;
        for (;;) {
            const auto end = pending_.find('\n', begin);
            if (end == std::string::npos) break;
            const std::string_view line{pending_.data() + begin, end - begin};
            if (line.find("Uncaptured WebGPU error: Texture") != std::string_view::npos &&
                line.find("is invalid") != std::string_view::npos) {
                const nlohmann::json record{{"event", "browser.invalid_webgpu_texture"}};
                audit.consume(record);
                observer(record);
            }
            const auto object = line.find('{');
            if (object != std::string_view::npos) {
                const auto record = nlohmann::json::parse(line.substr(object), nullptr, false);
                if (record.is_object()) {
                    audit.consume(record);
                    observer(record);
                }
            }
            begin = end + 1U;
        }
        pending_.erase(0U, begin);
    }

   private:
    std::filesystem::path path_;
    std::uintmax_t offset_;
    std::string pending_;
};

void report_consumed_record(const nlohmann::json& record, const std::string_view source) {
    const std::string event = record.value("event", "");
    const bool protocol_failure = event.find("protocol") != std::string::npos || record.value("terminal", "") == "protocol_failure";
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

void run_workspace_wayland_product(const TerminationMode termination, const std::string& viewer_scenario = {}, const bool logging = true) {
    if (!execution_requested()) SKIP("workspace Wayland integration requires the packaged hardware runner");
    const auto permitted_cpus = permitted_cpu_count();
    if (permitted_cpus < 2U) SKIP("workspace Wayland streaming acceptance requires at least two permitted CPUs");
    const bool h2d = GENERATE(true, false);
    INFO("selected H2D dataset transport: " << h2d);
    if (!h2d && !gdr_transport_available()) SKIP("GDR hardware unavailable; packaged Wayland GDR behavior remains unverified");

    const auto diagnostics = configured_path("MMLTK_GUI_TRACE_FILE", latest_wayland_artifact("latest-wayland-test.jsonl"));
    const auto runtime_log = configured_path("MMLTK_LOG_FILE", latest_wayland_artifact("latest-wayland-test-native.log"));
    const auto firefox_log = configured_path("MMLTK_FIREFOX_LOG_FILE", latest_wayland_artifact("latest-wayland-test-firefox.log"));
    prepare_latest_log(diagnostics, "workspace Wayland native trace");
    prepare_latest_log(runtime_log, "workspace Wayland native runtime log");
    prepare_latest_log(firefox_log, "workspace Wayland Firefox log");

    const std::string working_label = "mmltk-workspace-wayland-" + std::string{termination_label(termination)};
    ScopedTempDir working{working_label.c_str()};
    auto initial_settings = mmltk::controller::contracts::default_gui_settings_state();
    if (viewer_scenario == "rapid" || viewer_scenario == "copy") {
        const bool dark = GENERATE(false, true);
        initial_settings.ui.dark_mode = dark;
        initial_settings.ui.ui_scale = 1.0F;
    }
    initial_settings.workflows.explore.h2d_dataloader = h2d;
    initial_settings.workflows.train.request.h2d_dataloader = h2d;
    std::filesystem::create_directories(working.path() / ".mmltk-data");
    std::ofstream settings_file{working.path() / ".mmltk-data" / "gui.json"};
    REQUIRE(settings_file);
    settings_file << mmltk::controller::contracts::snapshot_gui_settings(initial_settings).dump();
    settings_file.close();
    const mmltk::backend::data::testsupport::FixtureSpec fixture{
        .root_dir = (working.path() / "fixture").string(),
        .split = "train",
        .width = 768,
        .height = 384,
        .num_images = viewer_scenario == "rapid" ? 300 : 128,
        .background_images = viewer_scenario.empty() ? 7 : 10,
    };
    mmltk::backend::data::testsupport::create_synthetic_dataset(fixture);
    if (viewer_scenario == "square")
        mmltk::backend::data::testsupport::replace_synthetic_image(fixture, 1, 384, 384);
    else if (viewer_scenario == "tall")
        mmltk::backend::data::testsupport::replace_synthetic_image(fixture, 1, 192, 384);
    if (viewer_scenario.empty()) {
        // Keep both padding orientations inside the initial square 3x3 atlas.
        mmltk::backend::data::testsupport::replace_synthetic_image(fixture, 8, 192, 384);
        mmltk::backend::data::testsupport::replace_synthetic_image(fixture, 9, 384, 192);
    } else {
        mmltk::backend::data::testsupport::replace_synthetic_image(fixture, 11, 192, 384);
    }
    if (viewer_scenario == "copy" || viewer_scenario == "semantics") {
        const auto split = std::filesystem::path{mmltk::backend::data::testsupport::dataset_dir(fixture)} / fixture.split;
        std::filesystem::copy_file(split / "000012.jsonl", split / "000001.jsonl", std::filesystem::copy_options::overwrite_existing);
    }
    {
        std::ofstream dropped_instance{
            std::filesystem::path{mmltk::backend::data::testsupport::dataset_dir(fixture)} / fixture.split / "000013.jsonl", std::ios::app};
        REQUIRE(dropped_instance);
        dropped_instance
            << R"({"class":"person","bbox_xyxy":[1,0,2,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"1:1","image_size_wh":[)"
            << fixture.width << ',' << fixture.height << "]}\n";
    }

    constexpr std::uintmax_t diagnostics_offset = 0U;
    constexpr std::uintmax_t runtime_log_offset = 0U;
    constexpr std::uintmax_t firefox_offset = 0U;
    ArtifactNotifications notifications{diagnostics, firefox_log};
    const bool high_dpi = viewer_scenario == "copy" ? GENERATE(false, true) : viewer_scenario == "rapid" && initial_settings.ui.dark_mode;
    BrowserHostProcess process{MMLTK_TEST_MMLTK_GUI_LAUNCHER,
                               diagnostics,
                               runtime_log,
                               firefox_log,
                               working.path(),
                               termination,
                               fixture,
                               viewer_scenario,
                               logging,
                               high_dpi,
                               h2d};
    ScopedFd deadline{::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)};
    if (deadline.get() < 0)
        throw std::runtime_error(std::string{"failed to create workspace Wayland acceptance deadline: "} + std::strerror(errno));
    arm_timerfd(deadline.get(), kWaylandAcceptanceDeadline, "workspace Wayland acceptance deadline");
    std::cout << "workspace-wayland: 90-second C++ acceptance deadline armed"
              << "\nnative JSONL: " << diagnostics << "\nnative runtime log: " << runtime_log << "\nFirefox log: " << firefox_log << '\n'
              << std::flush;

    // CLEANUP-IGNORE: This acceptance wait observes a test gate, while production monitors child-process custody.
    if (!logging) {
        // The acceptance socket reports entry into the real I/O gate. This
        // shutdown case needs neither diagnostic records nor a startup delay.
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
        REQUIRE((waits[0].revents & POLLIN) != 0);
        std::uint8_t entered = 0U;
        REQUIRE(::read(process.control_fd(), &entered, sizeof(entered)) == sizeof(entered));
        REQUIRE(entered == 0x80U);
        process.interrupt();
        const auto terminal = await_shutdown(process, deadline.get());
        REQUIRE(terminal.has_value());
        CHECK(*terminal == 0);
        CHECK(read_from(diagnostics, 0U).empty());
        return;
    }

    NativeAudit native;
    BrowserAudit browser;
    SurfaceAudit surface_audit;
    JsonLineCursor native_cursor{diagnostics, diagnostics_offset};
    JsonLineCursor browser_cursor{firefox_log, firefox_offset};
    const auto consume_records = [&] {
        native_cursor.consume(native, [&](const auto& record) {
            surface_audit.native(record);
            report_consumed_record(record, "native");
        });
        if (native.firefox_pid > 0) process.retain_peer(native.firefox_pid);
        browser_cursor.consume(browser, [&](const auto& record) {
            surface_audit.browser(record);
            report_consumed_record(record, "firefox");
        });
    };
    const auto report_artifacts = [&] {
        std::cerr << "\nnative diagnostics:\n"
                  << bounded_tail(read_from(diagnostics, diagnostics_offset)) << "\nnative runtime log:\n"
                  << bounded_tail(read_from(runtime_log, runtime_log_offset)) << "\nFirefox diagnostics:\n"
                  << bounded_tail(read_from(firefox_log, firefox_offset)) << '\n';
    };
    bool exited_early = false;
    bool released_one_lane = false;
    bool released_remaining_lanes = false;
    bool released_held_read = false;
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
                   slots != native.placeholder_slots.end() &&
                   browser.observed_frame_revision_for_slots(slots->second, publication.second) &&
                   browser.rendered_frame_for_slots(slots->second, 0U);
        });
    };
    for (;;) {
        consume_records();
        if (!viewer_scenario.empty() && browser.explore_ready && !released_remaining_lanes) {
            command_explore(2U, "released viewer scenario lanes");
            released_one_lane = true;
            released_remaining_lanes = true;
        }
        if (!viewer_scenario.empty() && browser.viewer_complete && browser.surface_draws.contains(browser.viewer_presentation) &&
            native.presentation_ready && native.explore_rendered)
            break;
        if (const auto* slots = browser.final_cursor_slots()) {
            if (const auto identified = native.final_generations_for(*slots)) final_generations = *identified;
        }
        if (!released_one_lane && browser.explore_ready) {
            const auto rendered_placeholder = std::ranges::find_if(native.placeholder_slots, [&native, &browser](const auto& placeholder) {
                const auto cardinality = native.placeholder_cardinalities.find(placeholder.first);
                return cardinality != native.placeholder_cardinalities.end() && cardinality->second == placeholder.second.size() &&
                       !placeholder.second.empty() && browser.rendered_frame_for_slots(placeholder.second, 0U);
            });
            if (rendered_placeholder != native.placeholder_slots.end()) {
                command_explore(1U, "released one Explore lane");
                released_placeholder_generation = rendered_placeholder->first;
                released_one_lane = true;
            }
        }
        if (!released_remaining_lanes && native.acceptance_first_patch_exact) {
            command_explore(2U, "released remaining Explore lanes");
            if (released_placeholder_generation == 0U) released_placeholder_generation = native.partial_generation;
            // A ready prefetch can satisfy the partial-patch evidence before
            // the first placeholder is drawn. Releasing all also releases one.
            released_one_lane = true;
            released_remaining_lanes = true;
        }
        if (released_one_lane && !released_remaining_lanes && !native.acceptance_first_patch_exact &&
            native.explore_generation > released_placeholder_generation &&
            native.placeholder_cardinalities.contains(native.explore_generation)) {
            command_explore(1U, "reissued one Explore lane for superseding generation");
            released_placeholder_generation = native.explore_generation;
        }
        if (!released_held_read && ((!viewer_scenario.empty() && native.acceptance_held_read) ||
                                    (viewer_scenario.empty() && native.superseding_placeholder_observed()))) {
            command_explore(4U, viewer_scenario.empty() ? "released stale Explore read" : "released held Explore read");
            released_held_read = true;
        }
        const bool rendered_probe_exported = rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical) &&
                                             rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal);
        const bool expected_window_close =
            termination == TerminationMode::WindowClose && native.product_completed(final_generations) && native.peer_close_count == 1U;
        const bool terminal_failure = (native.failed_before_termination() && !expected_window_close) || browser.failed_before_termination();
        if (terminal_failure && !terminal_failure_observed) {
            std::cerr << "workspace-wayland failed before readiness"
                      << "\nnative failure: " << native.failure_blocker() << "\nbrowser failure: " << browser.failure_blocker()
                      << "\nnative readiness blocker: " << native.readiness_blocker(final_generations)
                      << "\nbrowser readiness blocker: " << browser.readiness_blocker();
            report_artifacts();
            if (const auto terminal = process.reap_if_exited()) {
                std::cerr << "native host terminal status: " << process_status_text(*terminal) << '\n';
                std::cerr << std::flush;
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            std::cerr << "native host remained active at browser failure; draining evidence to the acceptance deadline\n";
            std::cerr << std::flush;
            terminal_failure_observed = true;
        }
        if (!terminal_failure_observed && (native.product_ready(final_generations) || expected_window_close) && browser.product_ready() &&
            rendered_probe_exported)
            break;
        if (viewer_scenario.empty() && !terminal_failure_observed && browser.terminal_evidence_settled() && !browser.product_ready()) {
            const auto blocker = browser.readiness_blocker();
            std::cerr << "workspace-wayland browser terminal evidence is incomplete: " << blocker;
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland browser terminal evidence is incomplete");
        }
        // CLEANUP-IGNORE: This acceptance loop polls process, JSONL, and deadline descriptors; production polls
        // independent child-custody, stop, and timer descriptors.
        std::array<pollfd, 3U> descriptors{{
            {.fd = process.pidfd(), .events = POLLIN, .revents = 0},
            {.fd = notifications.descriptor(), .events = POLLIN, .revents = 0},
            {.fd = deadline.get(), .events = POLLIN, .revents = 0},
        }};
        int ready = -1;
        do {
            ready = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            std::cerr << "workspace-wayland failed to poll product evidence: " << std::strerror(errno);
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland evidence poll failed");
        }
        if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            static_cast<void>(consume_timerfd(deadline.get()));
            consume_records();
            if (terminal_failure_observed) {
                std::cerr << "workspace-wayland terminal failure did not settle before its 90-second C++ acceptance deadline";
                report_artifacts();
                std::cerr << std::flush;
                process.terminate();
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            std::cerr << "workspace-wayland exceeded its 90-second C++ acceptance deadline"
                      << "\ntermination: " << termination_label(termination)
                      << "\nnative readiness blocker: " << native.readiness_blocker(final_generations)
                      << "\nbrowser readiness blocker: " << browser.readiness_blocker();
            report_artifacts();
            std::cerr << std::flush;
            process.terminate();
            FAIL("workspace Wayland acceptance exceeded 90 seconds");
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) notifications.consume();
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            consume_records();
            if (terminal_failure_observed) {
                std::cerr << "workspace-wayland native host settled after a terminal integration failure";
                report_artifacts();
                std::cerr << std::flush;
                const int terminal = process.reap();
                std::cerr << "native host terminal status: " << process_status_text(terminal) << '\n' << std::flush;
                FAIL("workspace Wayland product reported a terminal integration failure");
            }
            const bool terminal_window_close =
                termination == TerminationMode::WindowClose && native.product_completed(final_generations) && native.peer_close_count == 1U;
            exited_early = !((native.product_ready(final_generations) || terminal_window_close) && browser.product_ready());
            if (exited_early) {
                std::cerr << "workspace-wayland child exited before readiness";
                report_artifacts();
                std::cerr << std::flush;
                const int terminal = process.reap();
                std::cerr << "native host terminal status: " << process_status_text(terminal) << '\n' << std::flush;
            }
            break;
        }
    }

    bool peer_killed = false;
    process.close_explore_control();
    if (termination == TerminationMode::SignalInterrupt)
        process.interrupt();
    else if (termination == TerminationMode::AbruptPeerLoss)
        peer_killed = process.kill_peer(native.firefox_pid);
    const auto terminal_result = process.active() ? await_shutdown(process, deadline.get()) : std::optional<int>{process.status()};
    if (!terminal_result) {
        consume_records();
        std::cerr << "workspace-wayland exceeded its 90-second C++ acceptance deadline during shutdown"
                  << "\ntermination: " << termination_label(termination)
                  << "\nnative readiness blocker: " << native.readiness_blocker(final_generations)
                  << "\nbrowser readiness blocker: " << browser.readiness_blocker();
        report_artifacts();
        std::cerr << std::flush;
        process.terminate();
        FAIL("workspace Wayland acceptance exceeded 90 seconds during shutdown");
    }
    const int terminal = *terminal_result;
    consume_records();
    const std::string native_text = read_from(diagnostics, diagnostics_offset);
    const std::string browser_text = read_from(firefox_log, firefox_offset);

    INFO("termination: " << termination_label(termination));
    INFO("native diagnostics: " << bounded_tail(native_text));
    INFO("native runtime log: " << bounded_tail(read_from(runtime_log, runtime_log_offset)));
    INFO("Firefox diagnostics: " << bounded_tail(browser_text));
    INFO("native readiness blocker: " << native.readiness_blocker(final_generations));
    INFO("browser readiness blocker: " << browser.readiness_blocker());
    REQUIRE(terminal >= 0);
    INFO("surface identity join: " << surface_audit.joined_failure());
    CHECK(surface_audit.joined_failure().empty());
    CHECK_FALSE(browser.owned_atlas_interrupted);
    CHECK(browser.initial_atlas_complete);
    if (!viewer_scenario.empty()) {
        CHECK(terminal == 0);
        CHECK(browser.viewer_complete);
        if (viewer_scenario == "rapid") {
            CHECK(surface_audit.pending_supersession_completed());
            CHECK(browser.gallery_no_input_complete);
            CHECK(browser.owned_atlas_seen);
            CHECK(browser.atlas_draws.valid);
            CHECK((browser.atlas_draws.stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "end", "restored"}));
            CHECK(browser.atlas_draws.drawn_rows.contains(1U));
            CHECK(browser.atlas_draws.drawn_rows.contains(2U));
            CHECK(browser.atlas_draws.drawn_rows.contains(10U));
            CHECK(browser.atlas_draws.grid_round_trip);
            for (const auto& [id, surface] : surface_audit.surfaces)
                if (surface.candidate_withdrawn) CHECK(browser.renderer_reconstructions[id] == 1U);
            CHECK(browser.atlas_geometry_valid);
            CHECK_FALSE(browser.atlas_scaled_frames.empty());
            CHECK(browser.atlas_native_capacity);
            CHECK((browser.atlas_notices == std::set<std::string>{"explore.gallery.capacity", "explore.gallery.empty"}));
            CHECK((browser.atlas_window_draws == std::set<std::string>{"fullscreen", "restored"}));
            CHECK(browser.atlas_themes.contains(initial_settings.ui.dark_mode ? "dark" : "light"));
            CHECK(browser.atlas_device_scales.contains(initial_settings.ui.dark_mode ? 1.5 : 1.0));
            CHECK((browser.atlas_visibility_modes == std::set<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7}));
            CHECK((browser.atlas_scroll_stages == std::set<std::string>{"fractional", "row1", "row2", "row10", "end", "restored"}));
        }
        CHECK(browser.detail_fit);
        CHECK(browser.surface_draws.contains(browser.viewer_presentation));
        CHECK(native.explore_rendered);
        CHECK(native.presentation_ready);
        CHECK(native.shutdown_complete);
        CHECK_FALSE(native.worker_failed);
        CHECK_FALSE(browser.workspace_protocol_failure);
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
            const auto saved_path =
                std::filesystem::path(mmltk::backend::data::testsupport::compiled_dir(fixture)) / "viewer-annotations.cbor";
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
        return;
    }
    CHECK_FALSE(exited_early);
    CHECK(native.product_completed(final_generations));
    CHECK(browser.product_ready());
    REQUIRE(final_generations.material != 0U);
    REQUIRE(final_generations.cursor != 0U);
    REQUIRE(released_placeholder_generation != 0U);
    const auto released_placeholder = native.placeholder_slots.find(released_placeholder_generation);
    REQUIRE(released_placeholder != native.placeholder_slots.end());
    CHECK(browser.rendered_frame_for_slots(released_placeholder->second, 0U));
    const auto* final_cursor_slots = browser.final_cursor_slots();
    REQUIRE(final_cursor_slots != nullptr);
    const auto identified_final_generations = native.final_generations_for(*final_cursor_slots);
    REQUIRE(identified_final_generations.has_value());
    CHECK(identified_final_generations->material == final_generations.material);
    CHECK(identified_final_generations->cursor == final_generations.cursor);
    const auto* pointer_slots = browser.pointer_slots();
    REQUIRE(pointer_slots != nullptr);
    const auto pointer_generation = native.generation_for(*pointer_slots);
    REQUIRE(pointer_generation.has_value());
    CHECK(browser.rendered_frame_for_slots(*pointer_slots, 0U));
    CHECK(rendered_padding_exported(NativeAudit::PaddingOrientation::Vertical));
    CHECK(rendered_padding_exported(NativeAudit::PaddingOrientation::Horizontal));
    CHECK(browser.compiled_images == static_cast<std::uint64_t>(fixture.num_images));
    CHECK(browser.compiled_width == static_cast<std::uint64_t>(kCompiledResolution));
    CHECK(browser.compiled_height == static_cast<std::uint64_t>(kCompiledResolution));
    CHECK(browser.presentation_receipt != 0U);
    CHECK(native.presentation_timeline != 0U);
    CHECK(native.shutdown_requested);
    CHECK(native.firefox_terminal);
    CHECK(native.shutdown_complete);
    CHECK_FALSE(native.shutdown_incomplete);
    CHECK_FALSE(native.worker_failed);
    CHECK_FALSE(native.invalid_message);
    CHECK_FALSE(native.peer_replaced);
    CHECK(native.peer_open_count == 1U);
    if (termination == TerminationMode::SignalInterrupt) {
        CHECK((native.peer_close_count == 0U || (native.peer_close_count == 1U && native.peer_closed_after_shutdown)));
    } else {
        CHECK(native.peer_close_count == 1U);
    }
    CHECK_FALSE(native.interaction_rejected);
    CHECK(native.partial_generation != 0U);
    CHECK(native.partial_placeholder_ordinal != 0U);
    CHECK(native.partial_first_patch_ordinal > native.partial_placeholder_ordinal);
    CHECK(native.explore_ready_batch);
    REQUIRE(native.placeholder_slots.contains(native.partial_generation));
    REQUIRE(native.patched_slots.contains(native.partial_generation));
    REQUIRE(native.compiled_reads.contains(native.partial_generation));
    CHECK(native.patched_slots.at(native.partial_generation).size() > 1U);
    CHECK(std::ranges::any_of(browser.explore_slots, [&native](const auto& rendered) {
        return rendered.second == native.placeholder_slots.at(native.partial_generation);
    }));
    // The uncached first lane proves actual reading; later visible slots may
    // be receiver-owned copies or already completed prefetched lanes.
    CHECK(native.compiled_reads.at(native.partial_generation) > 0U);
    CHECK(native.exact_partial_slot_identity());
    CHECK(native.viewport_accept_count > native.viewport_placeholder_count);
    const auto viewport_read_generations = std::ranges::count_if(
        native.compiled_reads, [&native](const auto& read) { return native.accepted_generations.contains(read.first); });
    CHECK(static_cast<std::size_t>(viewport_read_generations) < native.viewport_accept_count);
    CHECK(native.explore_stale_count >= 1U);
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
    CHECK(native.explore_placeholder_first_row == browser.final_scroll_row);
    CHECK_FALSE(browser.workspace_protocol_failure);
    CHECK(browser_text.find("XPCOMGlueLoad error") == std::string::npos);
    CHECK(browser_text.find("Couldn't load XPCOM") == std::string::npos);
    CHECK(browser_text.find("panicked at") == std::string::npos);

    const auto compiled_path = std::filesystem::path{mmltk::backend::data::testsupport::compiled_bin_path(fixture)};
    REQUIRE(std::filesystem::is_regular_file(compiled_path));
    // CLEANUP-IGNORE: Runtime audit facts and persisted dataset metadata are independent Wayland acceptance evidence.
    const auto compiled = mmltk::backend::data::inspect_compiled_dataset(compiled_path);
    CHECK(compiled.image_count == static_cast<std::uint32_t>(fixture.num_images));
    CHECK(compiled.width == static_cast<std::uint32_t>(kCompiledResolution));
    CHECK(compiled.height == static_cast<std::uint32_t>(kCompiledResolution));
    CHECK(compiled.channels != 0U);
    CHECK_FALSE(compiled.class_names.empty());

    if (termination == TerminationMode::AbruptPeerLoss) {
        CHECK(peer_killed);
        REQUIRE(WIFEXITED(terminal));
        CHECK(WEXITSTATUS(terminal) != 0);
    } else {
        CHECK(terminal == 0);
    }
}

void workspace_wayland_product_sigint() { run_workspace_wayland_product(TerminationMode::SignalInterrupt); }

void workspace_wayland_product_sigint_without_logging() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, {}, false); }

void workspace_wayland_product_window_close() { run_workspace_wayland_product(TerminationMode::WindowClose); }

void workspace_wayland_product_peer_loss() { run_workspace_wayland_product(TerminationMode::AbruptPeerLoss); }

void workspace_wayland_viewer_square() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "square"); }
void workspace_wayland_viewer_wide() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "wide"); }
void workspace_wayland_viewer_tall() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "tall"); }
void workspace_wayland_viewer_semantics() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "semantics"); }
void workspace_wayland_viewer_copy() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "copy"); }
void workspace_wayland_viewer_rapid() { run_workspace_wayland_product(TerminationMode::SignalInterrupt, "rapid"); }

void add_rendered_probe_audit_fixture(NativeAudit& audit, const NativeAudit::PaddingOrientation orientation, const std::uint64_t generation,
                                      const std::uint64_t slot, const std::uint64_t compiled_index, const std::uint64_t frame_revision) {
    const auto key = std::pair{generation, slot};
    audit.padded_card_slots.emplace(key, compiled_index);
    audit.padding_orientations.emplace(key, orientation);
    audit.rendered_probe_slots.emplace(key, compiled_index);
    audit.transition_probe_slots.emplace(key, compiled_index);
    audit.rendered_probe_frames.emplace(key, frame_revision);
    audit.selected_overlay_slots.emplace(key, NativeAudit::OverlayDescriptorIdentity{1U, compiled_index});
    audit.hidden_overlay_slots.emplace(std::pair{generation - 1U, slot + 1U},
                                       NativeAudit::OverlayDescriptorIdentity{1U, compiled_index + 100U});
    audit.transformed_overlay_slots.emplace(key);
    audit.semantic_overlay_slots.emplace(key);
    audit.patched_slots[generation].emplace(slot, compiled_index);
}

[[nodiscard]] NativeAudit rendered_probe_audit_fixture(const bool vertical = true, const bool horizontal = true) {
    NativeAudit audit;
    if (vertical) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Vertical, 7U, 2U, 11U, 50U);
    if (horizontal) add_rendered_probe_audit_fixture(audit, NativeAudit::PaddingOrientation::Horizontal, 8U, 3U, 12U, 51U);
    return audit;
}

void rendered_probe_audit_rejects_mismatched_identity() {
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
    mismatched_slot.patched_slots.at(7U).clear();
    mismatched_slot.patched_slots.at(7U).emplace(3U, 11U);
    CHECK_FALSE(mismatched_slot.aligned_overlay_pixels());

    auto mismatched_compiled_index = rendered_probe_audit_fixture();
    mismatched_compiled_index.rendered_probe_slots.at({7U, 2U}) = 12U;
    CHECK_FALSE(mismatched_compiled_index.aligned_overlay_pixels());

    auto mismatched_orientation = rendered_probe_audit_fixture();
    mismatched_orientation.padding_orientations.at({7U, 2U}) = NativeAudit::PaddingOrientation::Horizontal;
    CHECK_FALSE(mismatched_orientation.aligned_overlay_pixels());

    auto mismatched_frame = rendered_probe_audit_fixture();
    mismatched_frame.rendered_probe_frames.at({7U, 2U}) = 0U;
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
    CHECK_FALSE(browser.rendered_frame_for_slots({{2U, 11U}}, 0U));
    CHECK(browser.rendered_frame_for_slots({{3U, 12U}}, 0U));
}

MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][sigint]", workspace_wayland_product_sigint);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][window_close]", workspace_wayland_product_window_close);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][peer_loss]", workspace_wayland_product_peer_loss);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_square]", workspace_wayland_viewer_square);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_wide]", workspace_wayland_viewer_wide);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_tall]", workspace_wayland_viewer_tall);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_semantics]", workspace_wayland_viewer_semantics);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_copy]", workspace_wayland_viewer_copy);
MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][viewer_rapid]", workspace_wayland_viewer_rapid);
MMLTK_REGISTER_TEST_CASE("[workspace_wayland_integration][rendered_probe_audit]", rendered_probe_audit_rejects_mismatched_identity);

MMLTK_REGISTER_TEST_CASE("[workspace_hardware][workspace_wayland_integration][sigint_without_logging]",
                         workspace_wayland_product_sigint_without_logging);

}  // namespace
namespace {

[[nodiscard]] nlohmann::json native_surface_record(const char* event, const std::uint64_t low = 12U) {
    const std::string_view name{event};
    const bool copying = name.starts_with("presentation.source_borrow.") || name == "presentation.source.copy";
    return {{"kind", "gui_runtime"}, {"event", event},        {"sequence", 7U},
            {"surface_high", 11U},   {"surface_low", low},    {"selection_generation", 19U},
            {"frame_revision", 23U}, {"capacity_width", 64U}, {"capacity_height", 32U},
            {"condition", 2U},       {"outcome", name == "presentation.source.copy" ? 0U : 1U},
            {"value", copying ? 0U : 1U}, {"source_revision", 23U}, {"allocation_generation", 7U},
            {"presentation_revision", copying ? 0U : 1U}, {"transfer_sequence", copying ? 0U : 1U},
            {"timeline_ready", copying ? 0U : 1U},
            {"span_id", 31U},
            {"span_outcome", static_cast<std::uint64_t>(std::string_view{event}.ends_with(".completed")
                ? mmltk::controller::contracts::DiagnosticSpanOutcome::Success
                : mmltk::controller::contracts::DiagnosticSpanOutcome::Unspecified)}};
}

[[nodiscard]] nlohmann::json browser_surface_record(const char* event, const std::uint64_t low = 12U) {
    return {{"event", event},
            {"surface", SurfaceAudit::native_identity(native_surface_record("", low))},
            {"requested_surface", SurfaceAudit::native_identity(native_surface_record("", low))},
            {"generation", 7U},
            {"width", 64U},
            {"height", 32U},
            {"frame_revision", 23U},
            {"presentation_revision", 1U},
            {"outcome", "claimed"}};
}

constexpr std::array native_surface_events{
    "presentation.allocation.created", "presentation.admission.enqueued",    "presentation.admission.written",
    "presentation.import.outcome",     "presentation.source_borrow.started", "presentation.source_borrow.completed",
    "presentation.source.copy",        "presentation.ready_sync.started",    "presentation.ready_sync.completed",
    "presentation.frame.edge",         "presentation.active.withdrawal",     "presentation.retirement"};
constexpr std::array browser_surface_events{"firefox.workspace.admitted",
                                            "iced.surface.texture_create",
                                            "firefox.workspace.claim_outcome",
                                            "firefox.workspace.registry_inserted",
                                            "firefox.workspace.import_ready_emitted",
                                            "firefox.workspace.ready",
                                            "iced.surface.owned_capture_submitted",
                                            "iced.surface.owned_draw_selected",
                                            "firefox.workspace.withdrawal",
                                            "iced.surface.retired",
                                            "firefox.workspace.retired"};

TEST_CASE("surface join rejects missing native provenance", "[workspace][audit]") {
    for (const char* field :
         {"sequence", "selection_generation", "frame_revision", "capacity_width", "capacity_height", "condition", "outcome"}) {
        SurfaceAudit audit;
        auto record = native_surface_record("presentation.allocation.created");
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
    for (const auto outcome : {DiagnosticSpanOutcome::Unspecified, DiagnosticSpanOutcome::ScopeExit,
                               DiagnosticSpanOutcome::Cancelled, DiagnosticSpanOutcome::Exception}) {
        SurfaceAudit audit;
        for (std::size_t index = 0U; index < 5U; ++index) audit.native(native_surface_record(native_surface_events[index]));
        auto ended = native_surface_record("presentation.source_borrow.completed");
        ended["span_outcome"] = static_cast<std::uint64_t>(outcome);
        audit.native(ended);
        const auto& surface = audit.surfaces.at(SurfaceAudit::native_identity(ended));
        CHECK(surface.source_steps.at(23U) == 1U);
        CHECK(surface.ended_spans.at(31U) == static_cast<std::uint64_t>(outcome));
        audit.native(native_surface_record("presentation.source.copy"));
        CHECK_FALSE(audit.joined_failure().empty());
    }
}

TEST_CASE("surface join rejects omitted or reordered admission and copy stages", "[workspace][audit]") {
    for (std::size_t omitted = 0U; omitted < native_surface_events.size(); ++omitted) {
        SurfaceAudit audit;
        for (std::size_t index = 0U; index < native_surface_events.size(); ++index)
            if (index != omitted) audit.native(native_surface_record(native_surface_events[index]));
        for (const char* event : browser_surface_events)
            audit.browser(browser_surface_record(event));
        CHECK_FALSE(audit.joined_failure().empty());
    }
    for (std::size_t omitted = 0U; omitted < browser_surface_events.size(); ++omitted) {
        if (std::string_view{browser_surface_events[omitted]} == "iced.surface.owned_draw_selected")
            continue;  // A capture alone is still a sample requiring the full join.
        SurfaceAudit audit;
        for (const char* event : native_surface_events)
            audit.native(native_surface_record(event));
        for (std::size_t index = 0U; index < browser_surface_events.size(); ++index)
            if (index != omitted) audit.browser(browser_surface_record(browser_surface_events[index]));
        CHECK_FALSE(audit.joined_failure().empty());
    }
    SurfaceAudit reordered;
    reordered.native(native_surface_record("presentation.admission.written"));
    reordered.native(native_surface_record("presentation.allocation.created"));
    CHECK_FALSE(reordered.joined_failure().empty());
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

enum class MissingHandoffEvidence { PendingDiscard, RetainedDraw, CaptureBeforeDraw, DuplicateReconstruction, DifferentCompleted };

[[nodiscard]] SurfaceAudit pending_handoff(const std::optional<MissingHandoffEvidence> missing = std::nullopt) {
    SurfaceAudit audit;
    const auto native = [&](const char* event, const std::uint64_t surface) {
        auto record = native_surface_record(event, surface);
        record["sequence"] = surface;
        audit.native(record);
    };
    const auto browser = [&](const char* event, const std::uint64_t surface, const std::uint64_t requested = 0U) {
        auto record = browser_surface_record(event, surface);
        record["generation"] = surface;
        if (requested != 0U) record["requested_surface"] = SurfaceAudit::native_identity(native_surface_record("", requested));
        audit.browser(record);
    };
    constexpr std::uint64_t d = 11U, a = 12U, b = 13U, c = 14U;
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (const char* event : native_surface_events)
            native(event, d);
    for (std::size_t stage = 0U; stage < 10U; ++stage)
        native(native_surface_events[stage], a);
    for (const auto surface : {b, c})
        for (std::size_t stage = 0U; stage < 4U; ++stage)
            native(native_surface_events[stage], surface);
    native("presentation.candidate.withdrawal", b);
    native("presentation.retirement", b);
    for (std::size_t stage = 4U; stage < 10U; ++stage)
        native(native_surface_events[stage], c);
    for (const auto surface : {a, c}) {
        native("presentation.active.withdrawal", surface);
        native("presentation.retirement", surface);
    }
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (std::size_t stage = 0U; stage < 8U; ++stage)
            browser(browser_surface_events[stage], d);
    for (std::size_t stage = 0U; stage < 8U; ++stage)
        browser(browser_surface_events[stage], a);
    for (std::size_t stage = 0U; stage < 6U; ++stage)
        browser(browser_surface_events[stage], b);
    browser("iced.surface.renderer_reconstructed", missing == MissingHandoffEvidence::DifferentCompleted ? d : a, b);
    if (missing == MissingHandoffEvidence::DuplicateReconstruction) browser("iced.surface.renderer_reconstructed", a, b);
    browser("iced.surface.owned_draw_selected", a, b);
    browser("firefox.workspace.withdrawal", b);
    for (std::size_t stage = 0U; stage < 2U; ++stage)
        browser(browser_surface_events[stage], c);
    if (missing != MissingHandoffEvidence::PendingDiscard) browser("iced.surface.pending_discarded", b);
    browser("iced.surface.retired", b);
    browser("firefox.workspace.retired", b);
    if (missing != MissingHandoffEvidence::RetainedDraw) browser("iced.surface.owned_draw_selected", a, c);
    for (std::size_t stage = 2U; stage < 6U; ++stage)
        browser(browser_surface_events[stage], c);
    if (missing == MissingHandoffEvidence::CaptureBeforeDraw) {
        browser("iced.surface.owned_draw_selected", c);
        browser("iced.surface.owned_capture_submitted", c);
    } else {
        browser("iced.surface.owned_capture_submitted", c);
        browser("iced.surface.owned_draw_selected", a, c);
        browser("iced.surface.owned_draw_selected", c);
    }
    for (const auto surface : {a, c})
        for (std::size_t stage = 8U; stage < browser_surface_events.size(); ++stage)
            browser(browser_surface_events[stage], surface);
    if (missing == MissingHandoffEvidence::DifferentCompleted)
        for (std::size_t stage = 8U; stage < browser_surface_events.size(); ++stage)
            browser(browser_surface_events[stage], d);
    return audit;
}

TEST_CASE("rapid surface join requires a complete pending-candidate handoff", "[workspace][audit]") {
    const auto complete = pending_handoff();
    REQUIRE(complete.joined_failure().empty());
    CHECK(complete.pending_supersession_completed());
    for (const auto missing :
         {MissingHandoffEvidence::PendingDiscard, MissingHandoffEvidence::RetainedDraw, MissingHandoffEvidence::CaptureBeforeDraw,
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
        {"height", 512U},
        {"layer", 0U},
        {"slot", 0U},
        {"content_width", 400U},
        {"content_height", rows * 100U},
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
    capture["event"] = "iced.surface.owned_capture_submitted";
    const nlohmann::json missing{{"event", "iced.surface.owned_draw_missing"}, {"control", kExploreGalleryControl}};
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
        browser.consume({{"event", "iced.surface.owned_draw_missing"}, {"control", control}});
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
    for (const auto* event : {"iced.gallery.source", "iced.surface.renderer_reconstructed", "iced.surface.owned_capture_submitted",
                             "iced.surface.owned_draw_selected"}) {
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
        if (kind == "selection") invalid["event"] = "iced.surface.owned_draw_selected";
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
        capture["event"] = "iced.surface.owned_capture_submitted";
        complete.consume(source);
        complete.consume(capture);
        complete.consume(draw);
        auto stage = draw;
        stage["event"] = "iced.surface.scroll_stage";
        stage["control"] = names[index];
        for (const auto* defect : {"source", "surface", "width", "height", "presentation", "row", "clip", "missing", "clipped"}) {
            auto invalid_audit = complete;
            auto invalid_stage = stage;
            const std::string_view kind{defect};
            if (kind == "source") invalid_stage["source_instance"] = 2U;
            if (kind == "surface") invalid_stage["surface"] = "000000000000000b000000000000000d";
            if (kind == "width") invalid_stage["width"] = 1024U;
            if (kind == "height") invalid_stage["height"] = 1024U;
            if (kind == "presentation") invalid_stage["presentation_revision"] = 1U;
            if (kind == "row") invalid_stage["first_row"] = 99U;
            if (kind == "clip") invalid_stage["clip"][1] = 20.0;
            if (kind == "missing") invalid_audit.last_draw.reset();
            if (kind == "clipped") {
                auto clipped = draw;
                clipped["event"] = "iced.surface.owned_draw_clipped";
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
        duplicate.consume(source);
        CHECK(duplicate.valid);
        for (const auto* field : {"dataset_identity", "gallery_generation", "first_row", "content_width"}) {
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
        for (const auto* event : {"iced.gallery.source", "iced.surface.owned_capture_submitted", "iced.surface.draw_encoded"}) {
            draw["event"] = event;
            audit.consume(draw);
        }
    };
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
                if (kind == "height") draw["height"] = 1024U;
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
