#include "gui/browser_runtime_firefox.h"

#include "browser/host_api.h"
#include "gui/app.h"
#include "gui/browser_app_server.h"
#include "gui/browser_host_helpers.h"
#include "gui/workspace_surface_broker.h"
#include "common_utils.h"
#include "mmltk/live/workspace_surface_pool.h"
#include "mmltk/live/workspace_trace.h"
#include "mmltk_logging.h"
#include "runtime_paths.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mmltk::gui {

namespace {

constexpr std::chrono::milliseconds kSnapshotInterval{50};
constexpr std::chrono::milliseconds kWorkspacePresentPollInterval{4};
constexpr std::chrono::seconds kGpuCapabilityTimeout{10};
constexpr std::size_t kMaxInboundMessagesPerWake = 64U;

volatile std::sig_atomic_t g_runtime_signal = 0;
volatile std::sig_atomic_t g_runtime_event_fd = -1;

void runtime_signal_handler(const int signal_number) noexcept {
    const int saved_errno = errno;
    g_runtime_signal = signal_number;
    const int event_fd = g_runtime_event_fd;
    if (event_fd >= 0) {
        const std::uint64_t value = 1U;
        const ssize_t ignored = ::write(event_fd, &value, sizeof(value));
        static_cast<void>(ignored);
    }
    errno = saved_errno;
}

class ScopedFd {
   public:
    ScopedFd() = default;
    explicit ScopedFd(const int fd) noexcept : fd_(fd) {}
    ~ScopedFd() {
        reset();
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset(const int next_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = next_fd;
    }

   private:
    int fd_ = -1;
};

class ScopedRuntimeSignals {
   public:
    explicit ScopedRuntimeSignals(const int event_fd) {
        struct sigaction action {};
        action.sa_handler = runtime_signal_handler;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        g_runtime_signal = 0;
        g_runtime_event_fd = event_fd;
        if (::sigaction(SIGINT, &action, &previous_interrupt_) != 0) {
            const int saved_errno = errno;
            g_runtime_event_fd = -1;
            throw std::system_error(saved_errno, std::generic_category(), "failed to install browser signal handlers");
        }
        if (::sigaction(SIGTERM, &action, &previous_terminate_) != 0) {
            const int saved_errno = errno;
            g_runtime_event_fd = -1;
            ::sigaction(SIGINT, &previous_interrupt_, nullptr);
            throw std::system_error(saved_errno, std::generic_category(), "failed to install browser signal handlers");
        }
        installed_ = true;
    }

    ~ScopedRuntimeSignals() {
        if (installed_) {
            g_runtime_event_fd = -1;
            ::sigaction(SIGTERM, &previous_terminate_, nullptr);
            ::sigaction(SIGINT, &previous_interrupt_, nullptr);
        }
    }

    ScopedRuntimeSignals(const ScopedRuntimeSignals&) = delete;
    ScopedRuntimeSignals& operator=(const ScopedRuntimeSignals&) = delete;

    [[nodiscard]] int received_signal() const noexcept {
        return g_runtime_signal;
    }

   private:
    struct sigaction previous_interrupt_ {};
    struct sigaction previous_terminate_ {};
    bool installed_ = false;
};

class RuntimeTrace {
   public:
    RuntimeTrace() {
        const char* path = std::getenv("MMLTK_GUI_TRACE_FILE");
        if (path == nullptr || *path == '\0') {
            return;
        }
        file_ = std::fopen(path, "a");
        if (file_ == nullptr) {
            mmltk::logging::logger("gui")->warn("failed to open GUI runtime JSONL trace `{}`: {}", path,
                                                std::strerror(errno));
        }
    }

    ~RuntimeTrace() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    RuntimeTrace(const RuntimeTrace&) = delete;
    RuntimeTrace& operator=(const RuntimeTrace&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return file_ != nullptr;
    }

    void write(const std::string_view event, nlohmann::json fields = nlohmann::json::object()) noexcept {
        if (file_ == nullptr) {
            return;
        }
        try {
            nlohmann::json record{
                {"source", "host"}, {"level", "trace"}, {"event", event}, {"fields", std::move(fields)}};
            std::string line = record.dump();
            line.push_back('\n');
            std::fwrite(line.data(), sizeof(char), line.size(), file_);
            std::fflush(file_);
        } catch (...) {
            constexpr std::string_view fallback =
                "{\"source\":\"host\",\"level\":\"error\",\"event\":\"trace.serialize_failed\"}\n";
            std::fwrite(fallback.data(), sizeof(char), fallback.size(), file_);
            std::fflush(file_);
        }
    }

   private:
    std::FILE* file_ = nullptr;
};

class EphemeralFirefoxProfile {
   public:
    EphemeralFirefoxProfile() {
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (runtime_dir == nullptr || *runtime_dir == '\0') {
            throw std::runtime_error("XDG_RUNTIME_DIR is required for the Wayland Firefox runtime");
        }
        std::string pattern = (std::filesystem::path(runtime_dir) / "mmltk-firefox-XXXXXX").string();
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        char* created = ::mkdtemp(mutable_pattern.data());
        if (created == nullptr) {
            throw std::system_error(errno, std::generic_category(), "failed to create ephemeral Firefox profile");
        }
        path_ = created;
        if (::chmod(path_.c_str(), S_IRWXU) != 0) {
            const int saved_errno = errno;
            std::error_code ignored;
            (void)mmltk::remove_tree_no_follow(path_, ignored);
            throw std::system_error(saved_errno, std::generic_category(), "failed to secure Firefox profile");
        }
    }

    ~EphemeralFirefoxProfile() {
        if (!path_.empty()) {
            std::error_code ignored;
            (void)mmltk::remove_tree_no_follow(path_, ignored);
        }
    }

    EphemeralFirefoxProfile(const EphemeralFirefoxProfile&) = delete;
    EphemeralFirefoxProfile& operator=(const EphemeralFirefoxProfile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

   private:
    std::filesystem::path path_;
};

[[nodiscard]] bool environment_entry_has_key(const std::string_view entry, const std::string_view key) noexcept {
    return entry.size() > key.size() && entry.starts_with(key) && entry[key.size()] == '=';
}

[[nodiscard]] std::optional<std::string> environment_value(const std::string_view key) {
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string_view entry{*current};
        if (environment_entry_has_key(entry, key)) {
            return std::string(entry.substr(key.size() + 1U));
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> firefox_environment(const std::filesystem::path& runtime_root,
                                                           const std::filesystem::path& workspace_socket,
                                                           const std::string_view session_token,
                                                           const std::string_view expected_origin) {
    constexpr std::array<std::string_view, 13> overridden{
        "DISPLAY",
        "GDK_BACKEND",
        "LD_LIBRARY_PATH",
        "MMLTK_WORKSPACE_BROKER_ORIGIN",
        "MMLTK_WORKSPACE_BROKER_SOCKET",
        "MMLTK_WORKSPACE_BROKER_TOKEN",
        "MOZILLA_FIVE_HOME",
        "MOZ_CRASHREPORTER_DISABLE",
        "MOZ_ENABLE_WAYLAND",
        "MOZ_NOREMOTE",
        "MOZ_DBUS_REMOTE",
        "NO_AT_BRIDGE",
        "XDG_SESSION_TYPE",
    };
    std::vector<std::string> result;
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string_view entry{*current};
        const bool replace = std::any_of(overridden.begin(), overridden.end(), [&](const std::string_view key) {
            return environment_entry_has_key(entry, key);
        });
        if (!replace) {
            result.emplace_back(entry);
        }
    }

    result.emplace_back("GDK_BACKEND=wayland");
    result.emplace_back("MOZILLA_FIVE_HOME=" + runtime_root.string());
    result.emplace_back("MOZ_CRASHREPORTER_DISABLE=1");
    result.emplace_back("MOZ_ENABLE_WAYLAND=1");
    result.emplace_back("MOZ_NOREMOTE=1");
    result.emplace_back("NO_AT_BRIDGE=1");
    result.emplace_back("XDG_SESSION_TYPE=wayland");
    result.emplace_back("MMLTK_WORKSPACE_BROKER_ORIGIN=" + std::string(expected_origin));
    result.emplace_back("MMLTK_WORKSPACE_BROKER_SOCKET=" + workspace_socket.string());
    result.emplace_back("MMLTK_WORKSPACE_BROKER_TOKEN=" + std::string(session_token));
    std::string library_path = runtime_root.string();
    if (const std::optional<std::string> existing = environment_value("LD_LIBRARY_PATH");
        existing.has_value() && !existing->empty()) {
        library_path.push_back(':');
        library_path.append(*existing);
    }
    result.emplace_back("LD_LIBRARY_PATH=" + std::move(library_path));
    return result;
}

[[nodiscard]] bool executable_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error && ::access(path.c_str(), X_OK) == 0;
}

[[nodiscard]] std::filesystem::path normalize_firefox_candidate(std::filesystem::path path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error) {
        path /= "firefox";
    }
    return path;
}

[[nodiscard]] std::filesystem::path resolve_firefox_binary() {
    std::vector<std::filesystem::path> candidates;
    if (const char* override_path = std::getenv("MMLTK_FIREFOX_BINARY");
        override_path != nullptr && *override_path != '\0') {
        candidates.push_back(normalize_firefox_candidate(override_path));
    }
#ifdef MMLTK_FIREFOX_RUNTIME_ROOT_SOURCE
    candidates.emplace_back(std::filesystem::path(MMLTK_FIREFOX_RUNTIME_ROOT_SOURCE) / "firefox");
#endif
#ifdef MMLTK_FIREFOX_RUNTIME_INSTALL_RELATIVE
    const std::filesystem::path install_prefix = mmltk::runtime_paths::install_prefix();
    candidates.emplace_back(install_prefix / MMLTK_FIREFOX_RUNTIME_INSTALL_RELATIVE / "firefox");
#endif

    for (const std::filesystem::path& candidate : candidates) {
        if (executable_file(candidate)) {
            return std::filesystem::canonical(candidate);
        }
    }
    throw std::runtime_error(
        "Firefox runtime not found at the configured owned runtime root; set MMLTK_FIREFOX_BINARY to an "
        "equivalent packaged runtime only for diagnostics");
}

[[nodiscard]] std::string make_session_token() {
    std::array<unsigned char, 32> bytes{};
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t result = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "failed to generate browser session token");
    }

    constexpr std::string_view hex = "0123456789abcdef";
    std::string token(bytes.size() * 2U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        token[index * 2U] = hex[bytes[index] >> 4U];
        token[index * 2U + 1U] = hex[bytes[index] & 0x0fU];
    }
    return token;
}

[[nodiscard]] int process_exit_code(const int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

class FirefoxProcess {
   public:
    ~FirefoxProcess() {
        terminate_and_reap();
    }

    FirefoxProcess(const FirefoxProcess&) = delete;
    FirefoxProcess& operator=(const FirefoxProcess&) = delete;
    FirefoxProcess() = default;

    void spawn(const std::filesystem::path& binary, const std::filesystem::path& profile, const std::string& page_url,
               const std::filesystem::path& workspace_socket, const std::string_view session_token,
               const std::string_view expected_origin) {
        const std::filesystem::path runtime_root = binary.parent_path();
        std::vector<std::string> arguments{
            binary.string(), "--no-remote", "--new-instance", "--profile", profile.string(), page_url,
        };
        std::vector<char*> argument_pointers;
        argument_pointers.reserve(arguments.size() + 1U);
        for (std::string& argument : arguments) {
            argument_pointers.push_back(argument.data());
        }
        argument_pointers.push_back(nullptr);

        std::vector<std::string> environment =
            firefox_environment(runtime_root, workspace_socket, session_token, expected_origin);
        std::vector<char*> environment_pointers;
        environment_pointers.reserve(environment.size() + 1U);
        for (std::string& entry : environment) {
            environment_pointers.push_back(entry.data());
        }
        environment_pointers.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        int spawn_error = ::posix_spawn_file_actions_init(&actions);
        if (spawn_error != 0) {
            throw std::system_error(spawn_error, std::generic_category(), "failed to initialize Firefox spawn actions");
        }
        ScopedFd log_fd;
        if (const char* log_path = std::getenv("MMLTK_FIREFOX_LOG_FILE"); log_path != nullptr && *log_path != '\0') {
            log_fd.reset(::open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR));
            if (log_fd.get() < 0) {
                const int saved_errno = errno;
                ::posix_spawn_file_actions_destroy(&actions);
                throw std::system_error(saved_errno, std::generic_category(), "failed to open Firefox log file");
            }
            spawn_error = ::posix_spawn_file_actions_adddup2(&actions, log_fd.get(), STDOUT_FILENO);
            if (spawn_error == 0) {
                spawn_error = ::posix_spawn_file_actions_adddup2(&actions, log_fd.get(), STDERR_FILENO);
            }
            if (spawn_error != 0) {
                ::posix_spawn_file_actions_destroy(&actions);
                throw std::system_error(spawn_error, std::generic_category(), "failed to redirect Firefox output");
            }
        }

        posix_spawnattr_t attributes;
        spawn_error = ::posix_spawnattr_init(&attributes);
        if (spawn_error != 0) {
            ::posix_spawn_file_actions_destroy(&actions);
            throw std::system_error(spawn_error, std::generic_category(),
                                    "failed to initialize Firefox spawn attributes");
        }
        spawn_error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (spawn_error == 0) {
            spawn_error = ::posix_spawnattr_setpgroup(&attributes, 0);
        }
        if (spawn_error == 0) {
            spawn_error = ::posix_spawn(&pid_, binary.c_str(), &actions, &attributes, argument_pointers.data(),
                                        environment_pointers.data());
        }
        ::posix_spawnattr_destroy(&attributes);
        ::posix_spawn_file_actions_destroy(&actions);
        if (spawn_error != 0) {
            pid_ = -1;
            throw std::system_error(spawn_error, std::generic_category(), "failed to launch Firefox");
        }
        process_group_ = pid_;

#ifdef SYS_pidfd_open
        pid_fd_.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0)));
#endif
    }

    [[nodiscard]] int pid_fd() const noexcept {
        return pid_fd_.get();
    }

    [[nodiscard]] pid_t process_group() const noexcept {
        return process_group_;
    }

    [[nodiscard]] std::optional<int> try_reap() noexcept {
        if (pid_ <= 0) {
            return exit_code_;
        }
        int status = 0;
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) {
            return std::nullopt;
        }
        if (result < 0) {
            exit_code_ = 1;
        } else {
            exit_code_ = process_exit_code(status);
        }
        pid_ = -1;
        pid_fd_.reset();
        return exit_code_;
    }

    void terminate_and_reap() noexcept {
        if (pid_ <= 0 && process_group_ <= 0) {
            return;
        }
        if (process_group_ > 0) {
            ::kill(-process_group_, SIGTERM);
        } else if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
        }
        for (int attempt = 0; attempt < 20; ++attempt) {
            static_cast<void>(try_reap());
            if (process_group_ > 0 && ::kill(-process_group_, 0) != 0 && errno == ESRCH) {
                process_group_ = -1;
            }
            if (pid_ <= 0 && process_group_ <= 0) {
                return;
            }
            if (process_group_ <= 0 && pid_ > 0) {
                ::kill(pid_, SIGTERM);
            }
            ::poll(nullptr, 0, 100);
        }
        if (process_group_ > 0) {
            ::kill(-process_group_, SIGKILL);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            pid_t wait_result = -1;
            do {
                wait_result = ::waitpid(pid_, &status, 0);
            } while (wait_result < 0 && errno == EINTR);
            exit_code_ = wait_result > 0 ? process_exit_code(status) : 1;
            pid_ = -1;
        }
        process_group_ = -1;
        pid_fd_.reset();
    }

   private:
    pid_t pid_ = -1;
    pid_t process_group_ = -1;
    ScopedFd pid_fd_;
    std::optional<int> exit_code_;
};

void signal_event_fd(const int event_fd) noexcept {
    const std::uint64_t value = 1U;
    const ssize_t result = ::write(event_fd, &value, sizeof(value));
    if (result < 0 && errno != EAGAIN && errno != EINTR) {
    }
}

void drain_event_fd(const int event_fd) noexcept {
    std::uint64_t value = 0U;
    while (::read(event_fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

[[nodiscard]] nlohmann::json bridge_state_json(const mmltk::browser::BrowserBridgeState& state) {
    const mmltk::browser::BrowserRuntimeCapabilities& capabilities = state.runtime_capabilities;
    nlohmann::json encoded{
        {"phase", mmltk::browser::browser_bridge_phase_name(state.phase)},
        {"connected", state.connected},
        {"lastError", state.last_error},
        {"lastSuccessRevision", state.last_success_revision.has_value() ? nlohmann::json(*state.last_success_revision)
                                                                        : nlohmann::json(nullptr)},
        {"runtimeCapabilities",
         {{"hostBackend", mmltk::browser::browser_host_backend_name(capabilities.host_backend)},
          {"navigatorGpu", mmltk::browser::browser_runtime_capability_status_name(capabilities.navigator_gpu)},
          {"workspaceSurfaceBridge",
           mmltk::browser::browser_runtime_capability_status_name(capabilities.workspace_surface_bridge)},
          {"workspaceSurfaceZeroCopy",
           mmltk::browser::browser_runtime_capability_status_name(capabilities.workspace_surface_zero_copy)}}},
    };
    if (state.capabilities.is_object() && !state.capabilities.empty()) {
        encoded["capabilities"] = state.capabilities;
    }
    return encoded;
}

void apply_runtime_capabilities(mmltk::browser::StateSnapshot& snapshot,
                                const mmltk::browser::BrowserRuntimeCapabilities& capabilities) {
    snapshot.runtime_capabilities = capabilities;
    if (snapshot.workflow_state.is_object()) {
        if (auto found = snapshot.workflow_state.find("live_runtime");
            found != snapshot.workflow_state.end() && found->is_object()) {
            (*found)["runtime_capabilities"] = capabilities;
        }
    }
}

void apply_workspace_surface_config(mmltk::browser::StateSnapshot& snapshot,
                                    const std::optional<mmltk::live::WorkspaceSwapchainDescriptor>& descriptor,
                                    const bool ready) {
    if (!descriptor.has_value() || !descriptor->valid()) {
        snapshot.workspace_surface.reset();
        return;
    }
    mmltk::browser::WorkspaceSurfaceInfo surface;
    surface.generation = std::to_string(descriptor->generation);
    surface.capacity_width = descriptor->width;
    surface.capacity_height = descriptor->height;
    surface.slot_count = static_cast<std::uint32_t>(descriptor->slots.size());
    surface.ready = ready;
    switch (snapshot.active_workflow) {
        case mmltk::browser::Workflow::Live:
            surface.source_kind = "live_prediction";
            break;
        case mmltk::browser::Workflow::Predict:
            surface.source_kind = "static_prediction";
            break;
        case mmltk::browser::Workflow::Annotate:
            surface.source_kind = snapshot.source.kind == mmltk::browser::SourceKind::VideoStream ? "live_annotation"
                                                                                                  : "still_annotation";
            break;
        default:
            surface.source_kind = "unknown";
            break;
    }
    snapshot.workspace_surface = std::move(surface);
}

[[nodiscard]] nlohmann::json workspace_present_json(const WorkspaceSurfacePresent& present) {
    return {
        {"type", "workspace.present"},
        {"generation", std::to_string(present.generation)},
        {"slot", present.slot},
        {"revision", std::to_string(present.revision)},
        {"width", present.width},
        {"height", present.height},
        {"sourceRegion",
         {{"x", present.source_region.x},
          {"y", present.source_region.y},
          {"width", present.source_region.width},
          {"height", present.source_region.height}}},
        {"captureNs", std::to_string(present.capture_ns)},
        {"readyNs", std::to_string(present.ready_ns)},
    };
}

class EventedWorkWakeGuard {
   public:
    EventedWorkWakeGuard(App& app, const int event_fd) : app_(app) {
        app_.set_evented_work_wake_callback([event_fd] { signal_event_fd(event_fd); });
    }
    ~EventedWorkWakeGuard() {
        app_.set_evented_work_wake_callback({});
    }

    EventedWorkWakeGuard(const EventedWorkWakeGuard&) = delete;
    EventedWorkWakeGuard& operator=(const EventedWorkWakeGuard&) = delete;

   private:
    App& app_;
};

}  

int run_firefox_browser_runtime(App& app, const AppLaunchOptions&, int, char**) {
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display == nullptr || *wayland_display == '\0') {
        throw std::runtime_error("WAYLAND_DISPLAY is required; no alternate display backend is supported");
    }

    ScopedFd event_fd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (event_fd.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "failed to create browser runtime eventfd");
    }
    ScopedRuntimeSignals signals(event_fd.get());
    EventedWorkWakeGuard wake_guard(app, event_fd.get());
    RuntimeTrace trace;

    const BrowserHostAssetPaths assets = resolve_browser_host_asset_paths();
    BrowserAppServer server;
    BrowserAppServer::Config server_config;
    server_config.asset_root = assets.browser_root;
    const std::string session_token = make_session_token();
    server_config.session_token = session_token;
    if (const char* trace_path = std::getenv("MMLTK_GUI_TRACE_FILE"); trace_path != nullptr && *trace_path != '\0') {
        server_config.ui_trace_enabled = true;
    }
    BrowserAppServer::Callbacks server_callbacks;
    server_callbacks.on_inbound_ready = [fd = event_fd.get()] { signal_event_fd(fd); };
    server_callbacks.on_connection_changed = [fd = event_fd.get()] { signal_event_fd(fd); };
    if (!server.start(std::move(server_config), std::move(server_callbacks))) {
        throw std::runtime_error(server.last_error());
    }
    const std::optional<std::string> page_url = server.page_url();
    if (!page_url.has_value()) {
        throw std::runtime_error("browser app server did not publish a page URL");
    }

    const std::filesystem::path firefox_binary = resolve_firefox_binary();
    EphemeralFirefoxProfile profile;
    const std::string expected_origin = page_url->substr(0U, page_url->find('?'));
    WorkspaceSurfaceBroker workspace_broker(profile.path() / "workspace.sock", session_token);
    FirefoxProcess firefox;
    firefox.spawn(firefox_binary, profile.path(), *page_url, workspace_broker.socket_path(), session_token,
                  expected_origin);
    workspace_broker.set_expected_process_group(firefox.process_group());
    if (trace.enabled()) {
        trace.write("runtime.started", {{"firefox", firefox_binary.string()}, {"origin", expected_origin}});
    }

    mmltk::browser::BrowserRuntimeCapabilities runtime_capabilities;
    runtime_capabilities.host_backend = mmltk::browser::BrowserHostBackend::Firefox;
    runtime_capabilities.workspace_surface_bridge = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    runtime_capabilities.workspace_surface_zero_copy = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    mmltk::browser::BrowserBridgeState bridge_state;
    bridge_state.runtime_capabilities = runtime_capabilities;
    BrowserSnapshotCache snapshot_cache;
    bool callbacks_ready = false;
    bool gpu_ready = false;
    bool previous_transport_connected = false;
    std::uint64_t current_connection_epoch = 0U;
    std::string last_published_bridge;
    std::vector<BrowserAppInboundMessage> inbound_messages;
    inbound_messages.reserve(kMaxInboundMessagesPerWake);

    const auto publish_bridge = [&] {
        std::string encoded = bridge_state_json(bridge_state).dump();
        if (encoded == last_published_bridge) {
            return;
        }
        last_published_bridge = encoded;
        if (!server.publish_bridge_state(std::move(encoded))) {
            throw std::runtime_error(server.last_error());
        }
    };
    const auto publish_snapshot = [&] {
        const std::uint64_t previous_revision = snapshot_cache.last_revision();
        std::string snapshot_json = snapshot_cache.encode([&] {
            mmltk::browser::StateSnapshot snapshot = app.browser_state_snapshot();
            apply_runtime_capabilities(snapshot, runtime_capabilities);
            apply_workspace_surface_config(snapshot, workspace_broker.descriptor(), workspace_broker.ready());
            return snapshot;
        });
        const bool changed = snapshot_cache.last_revision() != previous_revision;
        if (changed && !server.publish_snapshot(std::move(snapshot_json))) {
            throw std::runtime_error(server.last_error());
        }
        if (callbacks_ready && bridge_state.connected) {
            bridge_state.last_success_revision = snapshot_cache.last_revision();
        }
        return changed;
    };
    const auto publish_workspace_present = [&] {
        const std::optional<WorkspaceSurfacePresent> present = workspace_broker.poll_latest_present();
        const std::optional<mmltk::live::WorkspaceSwapchainDescriptor> descriptor = workspace_broker.descriptor();
        if (!present.has_value() || !descriptor.has_value() || descriptor->generation != present->generation) {
            return;
        }
        const std::uint64_t generation = present->generation;
        const std::uint32_t slot = present->slot;
        const std::uint64_t revision = present->revision;
        if (!server.publish_workspace_present(
                workspace_present_json(*present).dump(), [&workspace_broker, generation, slot, revision] {
                    mmltk::live::trace_workspace("host", "workspace.present_superseded", [&] {
                        return nlohmann::json{{"generation", generation}, {"slot", slot}, {"revision", revision}};
                    });
                    workspace_broker.release(generation, slot, revision);
                })) {
            workspace_broker.release(generation, slot, revision);
            throw std::runtime_error(server.last_error());
        }
    };
    bool workspace_ever_ready = false;
    const auto sync_workspace_capability = [&](const bool force) {
        const bool workspace_ready = workspace_broker.ready();
        const auto status = workspace_ready        ? mmltk::browser::BrowserRuntimeCapabilityStatus::Available
                            : workspace_ever_ready ? mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable
                                                   : mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
        const bool changed = force || runtime_capabilities.workspace_surface_bridge != status ||
                             runtime_capabilities.workspace_surface_zero_copy != status;
        if (!changed) {
            return false;
        }
        runtime_capabilities.workspace_surface_bridge = status;
        runtime_capabilities.workspace_surface_zero_copy = status;
        bridge_state.runtime_capabilities = runtime_capabilities;
        bridge_state.capabilities["workspace_surface_bridge"] = {
            {"status", workspace_ready        ? "ready"
                       : workspace_ever_ready ? "blocked"
                                              : "pending"},
            {"summary", workspace_ready        ? "native workspace surface ready"
                        : workspace_ever_ready ? "native workspace surface lost"
                                               : "workspace negotiation pending"},
            {"detail",
             workspace_ready ? "Firefox imported every DMA-BUF slot and completed the initial Vulkan transitions."
             : workspace_ever_ready ? "The imported workspace generation is no longer synchronized."
                                    : "Waiting for adapter identity, modifier, image import, and slot readiness."},
        };
        bridge_state.capabilities["workspace_surface_zero_copy"] = {
            {"status", workspace_ready        ? "ready"
                       : workspace_ever_ready ? "blocked"
                                              : "pending"},
            {"summary", workspace_ready        ? "zero-copy workspace ready"
                        : workspace_ever_ready ? "timeline synchronization lost"
                                               : "timeline synchronization pending"},
            {"detail", workspace_ready ? "CUDA imported every Firefox Vulkan timeline semaphore."
                       : workspace_ever_ready
                           ? "The active workspace generation no longer has complete timeline synchronization."
                           : "Waiting for every exported timeline semaphore to be imported by CUDA."},
        };
        workspace_ever_ready = workspace_ever_ready || workspace_ready;
        return changed;
    };

    std::shared_ptr<mmltk::live::WorkspaceSurfacePool> active_workspace_pool =
        app.active_workspace_surface_pool_handle();
    workspace_broker.update_pool(active_workspace_pool);
    sync_workspace_capability(true);
    publish_snapshot();
    publish_bridge();
    auto next_snapshot = std::chrono::steady_clock::now() + kSnapshotInterval;
    std::optional<std::chrono::steady_clock::time_point> readiness_deadline =
        std::chrono::steady_clock::now() + kGpuCapabilityTimeout;
    std::optional<std::chrono::steady_clock::time_point> workspace_readiness_deadline;
    std::uint64_t workspace_readiness_generation = 0U;
    if (active_workspace_pool != nullptr && !workspace_broker.ready()) {
        workspace_readiness_generation = active_workspace_pool->generation();
        workspace_readiness_deadline = std::chrono::steady_clock::now() + kGpuCapabilityTimeout;
    }
    int runtime_exit_code = 0;
    bool stop_requested = false;
    const auto reset_browser_handshake = [&] {
        callbacks_ready = false;
        gpu_ready = false;
        runtime_capabilities.navigator_gpu = mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
        bridge_state.runtime_capabilities = runtime_capabilities;
        bridge_state.capabilities = nlohmann::json::object();
        bridge_state.connected = false;
        bridge_state.phase = mmltk::browser::BrowserBridgePhase::Polling;
        bridge_state.last_error.clear();
        bridge_state.last_success_revision.reset();
        readiness_deadline = std::chrono::steady_clock::now() + kGpuCapabilityTimeout;
        publish_snapshot();
    };
    const auto request_runtime_failure = [&](const std::string& message, const bool report_to_page) {
        if (stop_requested) {
            return;
        }
        bridge_state.connected = false;
        bridge_state.phase = mmltk::browser::BrowserBridgePhase::Polling;
        bridge_state.last_error = message;
        if (report_to_page) {
            static_cast<void>(server.publish_error(message));
        }
        publish_bridge();
        mmltk::logging::logger("gui")->error("Firefox GUI runtime failure: {}", message);
        if (trace.enabled()) {
            trace.write("runtime.failure", {{"error", message}});
        }
        runtime_exit_code = 1;
        stop_requested = true;
    };
    while (!stop_requested) {
        std::array<pollfd, 3> descriptors{
            pollfd{event_fd.get(), POLLIN, 0},
            pollfd{firefox.pid_fd(), POLLIN, 0},
            pollfd{workspace_broker.poll_fd(), POLLIN, 0},
        };
        const nfds_t descriptor_count = descriptors.size();
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next_snapshot - now);
        const int timeout_ms =
            static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, kWorkspacePresentPollInterval.count()));
        const int poll_result = ::poll(descriptors.data(), descriptor_count, timeout_ms);
        if (poll_result < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "browser runtime poll failed");
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            drain_event_fd(event_fd.get());
        }
        if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            workspace_broker.pump();
        }
        if (signals.received_signal() != 0) {
            runtime_exit_code = 128 + signals.received_signal();
            stop_requested = true;
            continue;
        }
        if (const std::optional<int> firefox_exit = firefox.try_reap(); firefox_exit.has_value()) {
            runtime_exit_code = *firefox_exit;
            stop_requested = true;
            continue;
        }

        app.drain_background_work();
        active_workspace_pool = app.active_workspace_surface_pool_handle();
        workspace_broker.update_pool(active_workspace_pool);
        if (active_workspace_pool == nullptr) {
            workspace_readiness_generation = 0U;
            workspace_readiness_deadline.reset();
        } else if (workspace_broker.ready()) {
            workspace_readiness_generation = active_workspace_pool->generation();
            workspace_readiness_deadline.reset();
        } else if (!workspace_readiness_deadline.has_value() ||
                   workspace_readiness_generation != active_workspace_pool->generation()) {
            workspace_readiness_generation = active_workspace_pool->generation();
            workspace_readiness_deadline = std::chrono::steady_clock::now() + kGpuCapabilityTimeout;
            if (trace.enabled()) {
                trace.write("workspace.negotiation_started", {{"generation", workspace_readiness_generation}});
            }
        }
        if (sync_workspace_capability(false)) {
            publish_snapshot();
            publish_bridge();
        }
        publish_workspace_present();
        server.drain_inbound(inbound_messages, kMaxInboundMessagesPerWake);
        const std::uint64_t observed_connection_epoch = server.connection_epoch();
        const bool transport_connected = server.connected();
        if (observed_connection_epoch != current_connection_epoch) {
            current_connection_epoch = observed_connection_epoch;
            reset_browser_handshake();
            publish_bridge();
            if (trace.enabled()) {
                trace.write("transport.epoch", {{"epoch", current_connection_epoch}});
            }
        }
        if (transport_connected != previous_transport_connected) {
            previous_transport_connected = transport_connected;
            bridge_state.connected = transport_connected && callbacks_ready && gpu_ready;
            bridge_state.phase = bridge_state.connected ? mmltk::browser::BrowserBridgePhase::Idle
                                                        : mmltk::browser::BrowserBridgePhase::Polling;
            if (!transport_connected) {
                reset_browser_handshake();
            }
            publish_bridge();
            if (trace.enabled()) {
                trace.write("transport.connection", {{"connected", transport_connected}});
            }
        }

        for (const BrowserAppInboundMessage& message : inbound_messages) {
            if (stop_requested) {
                break;
            }
            if (message.connection_epoch != current_connection_epoch ||
                message.connection_epoch != server.connection_epoch() || !server.connected()) {
                if (trace.enabled()) {
                    trace.write("transport.message_stale", {{"message_epoch", message.connection_epoch},
                                                            {"active_epoch", server.connection_epoch()}});
                }
                continue;
            }
            try {
                switch (message.kind) {
                    case BrowserAppInboundMessage::Kind::CallbacksReady:
                        callbacks_ready = true;
                        bridge_state.connected = transport_connected && gpu_ready;
                        bridge_state.phase = bridge_state.connected ? mmltk::browser::BrowserBridgePhase::Idle
                                                                    : mmltk::browser::BrowserBridgePhase::Polling;
                        bridge_state.last_error.clear();
                        if (bridge_state.connected) {
                            readiness_deadline.reset();
                            bridge_state.last_success_revision = snapshot_cache.last_revision();
                        }
                        publish_bridge();
                        break;
                    case BrowserAppInboundMessage::Kind::RuntimeCapabilities:
                        runtime_capabilities.navigator_gpu = message.navigator_gpu;
                        bridge_state.runtime_capabilities = runtime_capabilities;
                        bridge_state.capabilities = message.capabilities;
                        sync_workspace_capability(true);
                        gpu_ready = message.navigator_gpu == mmltk::browser::BrowserRuntimeCapabilityStatus::Available;
                        bridge_state.connected = transport_connected && callbacks_ready && gpu_ready;
                        bridge_state.phase = bridge_state.connected ? mmltk::browser::BrowserBridgePhase::Idle
                                                                    : mmltk::browser::BrowserBridgePhase::Polling;
                        if (bridge_state.connected) {
                            readiness_deadline.reset();
                            bridge_state.last_error.clear();
                            bridge_state.last_success_revision = snapshot_cache.last_revision();
                        }
                        publish_snapshot();
                        publish_bridge();
                        if (message.navigator_gpu == mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable) {
                            request_runtime_failure("Firefox WebGPU is unavailable; CPU display fallback is disabled",
                                                    true);
                        }
                        break;
                    case BrowserAppInboundMessage::Kind::Intent:
                        if (!transport_connected || !callbacks_ready || !gpu_ready) {
                            throw std::runtime_error("browser intent rejected until Firefox WebGPU is ready");
                        }
                        if (!message.intent.has_value()) {
                            throw std::runtime_error("parsed browser intent payload is absent");
                        }
                        bridge_state.phase = mmltk::browser::BrowserBridgePhase::Dispatch;
                        app.apply_browser_intent(*message.intent);
                        bridge_state.phase = mmltk::browser::BrowserBridgePhase::Idle;
                        bridge_state.last_error.clear();
                        publish_snapshot();
                        publish_bridge();
                        break;
                    case BrowserAppInboundMessage::Kind::WorkspaceRelease:
                        workspace_broker.release(message.workspace_generation, message.workspace_slot,
                                                 message.workspace_revision);
                        break;
                    case BrowserAppInboundMessage::Kind::Rejected:
                        throw std::runtime_error(message.error.empty() ? "browser message rejected" : message.error);
                }
            } catch (const std::exception& error) {
                bridge_state.phase = bridge_state.connected ? mmltk::browser::BrowserBridgePhase::Idle
                                                            : mmltk::browser::BrowserBridgePhase::Polling;
                bridge_state.last_error = error.what();
                if (!server.publish_error(error.what())) {
                    request_runtime_failure(server.last_error(), false);
                    break;
                }
                publish_bridge();
                if (trace.enabled()) {
                    trace.write("transport.message_rejected", {{"error", error.what()}});
                }
            }
        }

        if (!stop_requested && readiness_deadline.has_value() &&
            std::chrono::steady_clock::now() >= *readiness_deadline) {
            request_runtime_failure("Firefox did not report an available WebGPU runtime before the startup deadline",
                                    true);
        }
        if (!stop_requested && workspace_readiness_deadline.has_value() &&
            std::chrono::steady_clock::now() >= *workspace_readiness_deadline) {
            request_runtime_failure(
                "Firefox WebGPU workspace negotiation did not complete adapter identity, explicit modifier, "
                "DMA-BUF import, initial layout, and CUDA timeline synchronization before the deadline; all "
                "display fallbacks are disabled",
                true);
        }

        if (!stop_requested && std::chrono::steady_clock::now() >= next_snapshot) {
            publish_snapshot();
            publish_bridge();
            next_snapshot = std::chrono::steady_clock::now() + kSnapshotInterval;
        }
    }

    if (trace.enabled()) {
        trace.write("runtime.stopping", {{"exit_code", runtime_exit_code}});
    }
    server.stop();
    firefox.terminate_and_reap();
    return runtime_exit_code;
}

}  
