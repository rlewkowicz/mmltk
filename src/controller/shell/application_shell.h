#pragma once
#include <sys/types.h>
#include "src/frameworks/reflection/field_policy.h"
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include "src/controller/browser/application_browser_host.h"
#include "src/controller/services/application_lifecycle_event.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/file_dialog_client.h"
#include "src/controller/services/firefox_process_owner.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/services/settings_location.h"
#include "src/controller/services/vast_provider_owner.h"
#include "src/controller/shell/direct_visual_systems.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/transport/browser_server.h"

namespace mmltk::controller::shell {
namespace browser = mmltk::controller::browser;
namespace services = mmltk::controller::services;
namespace transport = mmltk::frameworks::transport;

struct ApplicationShellConfig final {
    struct PresentationConfig final {
        std::filesystem::path import_socket;
        [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int cuda_device_index = 0;
        [[= mmltk::frameworks::reflection::Minimum<int>{-1}]] int numa_node = -1;
        VisualExtent extent{.width = 1'200U, .height = 800U};
        std::size_t pitch_bytes = 1'200U * 4U;
        std::size_t minimum_allocation_bytes = 0U;
    };
    PresentationConfig presentation{
        .import_socket = {}, .cuda_device_index = 0, .extent = {.width = 1'200U, .height = 800U}, .pitch_bytes = 1'200U * 4U};
    LiveNativeConfiguration live{};
    ExploreNativeConfiguration explore{};
    services::DiagnosticsClient diagnostics{};
    services::RuntimeDiagnosticDelivery diagnostic_delivery = services::RuntimeDiagnosticDelivery::BestEffort;
    bool pixel_probes = false;
    std::optional<services::VastBridgeConfig> vast_provider{};
    std::string_view file_dialog_helper{"zenity"};
    std::string_view file_dialog_launch_directory{"/host"};
    services::SettingsLocation settings_location{std::string_view{}};
    bool h2d_dataloader = true;
    bool pending_supersession_acceptance = false;
    std::shared_ptr<PresentationAcceptanceGate> completion_acceptance{};
    std::filesystem::path training_executable{};
};
MMLTK_REFLECT_FIELDS(ApplicationShellConfig::PresentationConfig)

class ApplicationShell final {
   public:
    explicit ApplicationShell(ApplicationShellConfig);
    ~ApplicationShell() noexcept;
    ApplicationShell(const ApplicationShell&) = delete;
    ApplicationShell& operator=(const ApplicationShell&) = delete;
    [[nodiscard]] bool start_browser_host(transport::BrowserServer::Config) noexcept;
    [[nodiscard]] services::FirefoxProcessStartResult start_firefox(services::FirefoxProcessConfig) noexcept;
    [[nodiscard]] services::FirefoxProcessLifecycle firefox_lifecycle() const noexcept;
    void run();
    void request_shutdown(services::ApplicationShutdownReason) noexcept;
    [[nodiscard]] bool shutdown() noexcept;
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] transport::BrowserServer& browser_server() noexcept { return browser_server_; }
    [[nodiscard]] const transport::BrowserServer& browser_server() const noexcept { return browser_server_; }
    [[nodiscard]] const std::filesystem::path& presentation_import_socket_path() const noexcept;

   private:
    static bool InstallFirefoxProcessGroup(void*, pid_t) noexcept;
    static void FirefoxPhysicalObservation(void*, services::FirefoxPhysicalObservation) noexcept;
    [[nodiscard]] bool install_firefox_process_group(pid_t) noexcept;
    void on_firefox_observation(services::FirefoxPhysicalObservation) noexcept;
    void emit_shutdown_event(std::string_view) noexcept;
    void request_system_stops() noexcept;
    [[nodiscard]] bool join_systems() noexcept;

    services::DiagnosticsClient diagnostics_client_;
    services::RuntimeDiagnostics runtime_diagnostics_;
    services::RuntimeDiagnosticTarget visual_diagnostic_target_;
    transport::BrowserServer browser_server_;
    browser::ApplicationBrowserHost browser_host_;
    services::VastProviderOwner provider_owner_;
    services::FileDialogClientOwner file_dialog_owner_;
    ApplicationShellConfig::PresentationConfig presentation_config_{};
    LiveNativeConfiguration live_configuration_{};
    std::unique_ptr<ApplicationSystemStorage> systems_;
    std::optional<services::FirefoxProcessOwner> firefox_process_;
    std::atomic_bool shutdown_requested_ = false;
    std::once_flag shutdown_request_once_;
    bool shutdown_complete_ = false;
    bool systems_stopped_ = false;
    bool healthy_ = true;
};
}  // namespace mmltk::controller::shell
