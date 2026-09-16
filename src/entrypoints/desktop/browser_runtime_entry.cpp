#include <Loop.h>
#include <spdlog/spdlog.h>
#include "src/entrypoints/desktop/browser_runtime_options.h"
#include <vector>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include "src/common/io/scoped_fd.h"
#include <cstdlib>
#include <cerrno>
#include <charconv>
#include <exception>
#include <filesystem>
#include <memory>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include "src/controller/services/firefox_process_owner.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/settings_location.h"
#include "src/controller/shell/application_shell.h"
import mmltk.common.logging.mmltk_logging;
namespace mmltk::entrypoints::desktop {
namespace {
class UwsLoopOwner final {
   public:
    UwsLoopOwner() : loop_(uWS::Loop::get()) {}
    ~UwsLoopOwner() {
        if (loop_ != nullptr) loop_->free();
    }
    UwsLoopOwner(const UwsLoopOwner&) = delete;
    UwsLoopOwner& operator=(const UwsLoopOwner&) = delete;

   private:
    uWS::Loop* loop_ = nullptr;
};
[[nodiscard]] bool integration_requested() noexcept {
    const char* const integration = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION");
    return integration != nullptr && std::string_view{integration} == "1";
}
void report_runtime_failure(const std::string_view stage, const std::string_view detail = {}) noexcept {
    mmltk::common::logging::error([&](auto& logger) { logger.error("browser runtime failure: stage={}, detail={}", stage, detail); });
}
[[nodiscard]] int fail_closed(mmltk::controller::shell::ApplicationShell& shell, const std::string_view stage) noexcept {
    report_runtime_failure(stage);
    shell.request_shutdown(mmltk::controller::services::ApplicationShutdownReason::InfrastructureFailure);
    static_cast<void>(shell.shutdown());
    return 1;
}
class SignalWaiter final {
   public:
    explicit SignalWaiter(mmltk::controller::shell::ApplicationShell& shell, const int diagnostics_terminal)
        : thread_([&shell, diagnostics_terminal](const std::stop_token stop) {
              sigset_t signals{};
              if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGINT) != 0 || ::sigaddset(&signals, SIGTERM) != 0) {
                  shell.request_shutdown(mmltk::controller::services::ApplicationShutdownReason::InfrastructureFailure);
                  return;
              }
              mmltk::common::io::ScopedFd signal_fd{::signalfd(-1, &signals, SFD_CLOEXEC)};
              pollfd events[]{{.fd = signal_fd.get(), .events = POLLIN, .revents = 0}, {.fd = diagnostics_terminal, .events = POLLIN, .revents = 0}};
              int ready = -1;
              if (signal_fd.get() >= 0) {
                  do { ready = ::poll(events, 2U, -1); } while (ready < 0 && errno == EINTR);
              }
              if (stop.stop_requested()) return;
              signalfd_siginfo signal{};
              if (ready < 0 || events[1].revents != 0 || ::read(signal_fd.get(), &signal, sizeof(signal)) != static_cast<ssize_t>(sizeof(signal))) {
                  shell.request_shutdown(mmltk::controller::services::ApplicationShutdownReason::InfrastructureFailure);
                  return;
              }
              shell.request_shutdown(signal.ssi_signo == SIGINT ? mmltk::controller::services::ApplicationShutdownReason::SignalInterrupt
                                                                : mmltk::controller::services::ApplicationShutdownReason::SignalTerminate);
          }) {}
    ~SignalWaiter() {
        thread_.request_stop();
        if (thread_.joinable()) static_cast<void>(::pthread_kill(thread_.native_handle(), SIGTERM));
    }
    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;

   private:
    std::jthread thread_;
};
[[nodiscard]] std::string session_token() {
    // The server checks this capability before accepting the only local page.
    // It is intentionally process-local and only authenticates this page.
    std::string value{"mmltk-"};
    value += std::to_string(static_cast<unsigned long long>(std::random_device{}()));
    return value;
}
[[nodiscard]] std::filesystem::path firefox_log_file() {
    const char* const configured = std::getenv("MMLTK_FIREFOX_LOG_FILE");
    return configured != nullptr && configured[0] != '\0' ? std::filesystem::path{configured} : std::filesystem::path{};
}
[[nodiscard]] std::filesystem::path configured_root(const char* const environment_name, const char* const installed_root) {
    const char* const configured = std::getenv(environment_name);
    return configured != nullptr && configured[0] != '\0' ? std::filesystem::path{configured} : std::filesystem::path{installed_root};
}
[[nodiscard]] int integration_control_fd() noexcept {
    const char* const configured = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_EXPLORE_CONTROL_FD");
    if (configured == nullptr || configured[0] == '\0') return -1;
    int descriptor = -1;
    const std::string_view text{configured};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || descriptor < 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) return -1;
    return descriptor;
}
[[nodiscard]] std::string query_value(const std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.' || character == '~') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(digits[character >> 4U]);
            result.push_back(digits[character & 0x0fU]);
        }
    }
    return result;
}
[[nodiscard]] mmltk::controller::services::SettingsLocation production_settings_location() {
    std::error_code error;
    const std::filesystem::path working_directory = std::filesystem::current_path(error);
    if (error) { return mmltk::controller::services::SettingsLocation{std::string_view{}}; }
    return mmltk::controller::services::SettingsLocation{(working_directory / ".mmltk-data" / "gui.json").string()};
}
}  // namespace
}  // namespace mmltk::entrypoints::desktop
int main(int argc, char** argv) {
    using namespace mmltk::entrypoints::desktop;
    const bool integration = integration_requested();
    const char* const integration_dpi = integration ? std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_DPI") : nullptr;
    const bool integration_high_dpi = integration_dpi != nullptr && std::string_view{integration_dpi} == "1.5";
    try {
        {
            auto logging_config = mmltk::common::logging::config_from_env({});
            if (logging_config.enabled()) { logging_config.app_name = "mmltk-browser-host"; }
            mmltk::common::logging::initialize(logging_config);
            mmltk::common::logging::trace([](auto& logger) { logger.trace("workspace Wayland native runtime logging initialized"); });
        }
        UwsLoopOwner loop_owner;
        if (!mmltk::controller::services::block_browser_runtime_signals()) {
            report_runtime_failure("signal-mask setup failed");
            return 1;
        }
        std::vector<std::string_view> arguments;
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        auto runtime = parse_browser_runtime_options(arguments);
        mmltk::controller::shell::ApplicationShellConfig config;
        const char* const pixel_trace = std::getenv("MMLTK_GUI_PIXEL_TRACE");
        const char* const lifecycle_trace = std::getenv("MMLTK_GUI_TRACE_FILE");
        const bool pixel_probes = pixel_trace != nullptr && std::string_view{pixel_trace} == "1" && lifecycle_trace != nullptr && *lifecycle_trace != '\0';
        config.pixel_probes = pixel_probes;
        config.presentation = std::move(runtime.presentation);
        config.h2d_dataloader = runtime.h2d_dataloader;
        std::string integration_file_dialog_root;
        if (integration) {
            std::error_code error;
            integration_file_dialog_root = std::filesystem::current_path(error).string();
            if (error || integration_file_dialog_root.empty()) {
                if (mmltk::common::logging::enabled(spdlog::level::err))
                    report_runtime_failure("integration working-directory discovery failed", error.message());
                return 1;
            }
            config.file_dialog_launch_directory = integration_file_dialog_root;
            const int explore_control = integration_control_fd();
            if (explore_control < 0) {
                report_runtime_failure("Explore acceptance control descriptor rejected");
                return 1;
            }
            config.explore.acceptance = std::make_shared<mmltk::controller::ExploreAcceptanceGate>(explore_control);
            const char* const completion_gate = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPLETION_GATE");
            if (completion_gate != nullptr && std::string_view{completion_gate} == "1") {
                config.completion_acceptance = std::make_shared<mmltk::controller::PresentationAcceptanceGate>();
                config.explore.acceptance->SetReadObserver(config.explore.acceptance.get(), [](void* owner, std::uint64_t generation, std::uint32_t index) {
                    static_cast<mmltk::controller::ExploreAcceptanceGate*>(owner)->AwaitVisibleRead(generation, index);
                });
            }
            const char* const pending_supersession = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_PENDING_SUPERSESSION");
            config.pending_supersession_acceptance = pending_supersession != nullptr && std::string_view{pending_supersession} == "1";
            if (config.pending_supersession_acceptance && !config.completion_acceptance)
                config.completion_acceptance = std::make_shared<mmltk::controller::PresentationAcceptanceGate>();
        }
        if (lifecycle_trace != nullptr && *lifecycle_trace != '\0') {
            config.diagnostics = mmltk::controller::services::DiagnosticsClient{std::filesystem::path{lifecycle_trace}};
            if (integration) config.diagnostic_delivery = mmltk::controller::services::RuntimeDiagnosticDelivery::Complete;
        }
        config.settings_location = production_settings_location();
        const int diagnostics_terminal =
            config.diagnostic_delivery == mmltk::controller::services::RuntimeDiagnosticDelivery::Complete ? config.diagnostics.terminal_fd() : -1;
        mmltk::controller::shell::ApplicationShell shell{std::move(config)};
        try {
            const std::filesystem::path assets = configured_root("MMLTK_BROWSER_APP_ASSET_ROOT_OVERRIDE", MMLTK_BROWSER_APP_ASSET_ROOT);
            std::string page_query;
            if (integration) {
                page_query = "mmltk_integration=1";
                if (const char* fixture = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_PIXEL_FIXTURE"); fixture != nullptr && std::string_view{fixture} == "1")
                    page_query += "&mmltk_integration_pixel_fixture=1";
                if (const char* scenario = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_VIEWER_SCENARIO"); scenario != nullptr)
                    page_query += "&mmltk_integration_viewer_scenario=" + query_value(scenario);
                if (const char* square = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_SQUARE_SOURCE"); square != nullptr)
                    page_query += "&mmltk_integration_square_source=" + query_value(square);
                if (const char* square = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_SQUARE_COMPILED"); square != nullptr)
                    page_query += "&mmltk_integration_square_compiled=" + query_value(square);
                const char* const window_close = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_WINDOW_CLOSE");
                if (window_close != nullptr && std::string_view{window_close} == "1") page_query += "&mmltk_integration_window_close=1";
                const char* const source = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_DATASET_SOURCE");
                const char* const compiled = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_COMPILED_DIRECTORY");
                const char* const resolution = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_RESOLUTION");
                if (source == nullptr || compiled == nullptr || resolution == nullptr) return fail_closed(shell, "integration dataset configuration missing");
                page_query += "&mmltk_integration_dataset_source=" + query_value(source);
                page_query += "&mmltk_integration_compiled_directory=" + query_value(compiled);
                page_query += "&mmltk_integration_resolution=" + query_value(resolution);
            }
            if (lifecycle_trace != nullptr && *lifecycle_trace != '\0') {
                if (!page_query.empty()) page_query += '&';
                page_query += "mmltk_surface_trace=1";
            }
            if (pixel_probes) {
                if (!page_query.empty()) page_query += '&';
                page_query += "mmltk_pixel_trace=1";
            }
            if (!shell.start_browser_host({.asset_root = assets, .session_token = session_token(), .page_query = std::move(page_query)})) {
                return fail_closed(shell, "browser host start failed");
            }
            auto& server = shell.browser_server();
            if (!server.running()) { return fail_closed(shell, "browser server stopped during startup"); }
            auto page = server.page_url();
            if (!page) return fail_closed(shell, "browser page URL unavailable");
            const auto process_start =
                shell.start_firefox({.executable = configured_root("MMLTK_FIREFOX_RUNTIME_ROOT_OVERRIDE", MMLTK_FIREFOX_RUNTIME_ROOT) / "firefox",
                                     .page_url = std::move(*page),
                                     .log_file = firefox_log_file(),
                                     .integration = integration,
                                     .integration_high_dpi = integration_high_dpi});
            if (process_start == mmltk::controller::services::FirefoxProcessStartResult::Terminal) return fail_closed(shell, "Firefox process start failed");
            SignalWaiter signal_waiter{shell, diagnostics_terminal};
            shell.run();
            return mmltk::controller::services::browser_runtime_exit_status(shell.firefox_lifecycle(), shell.healthy());
        } catch (const std::exception& error) {
            report_runtime_failure("browser host runtime exception", error.what());
            return fail_closed(shell, "browser host fail-closed shutdown");
        } catch (...) { return fail_closed(shell, "browser host unknown runtime exception"); }
    } catch (const std::exception& error) {
        report_runtime_failure("browser host construction exception", error.what());
        return 1;
    } catch (...) {
        report_runtime_failure("browser host unknown construction exception");
        return 1;
    }
}
