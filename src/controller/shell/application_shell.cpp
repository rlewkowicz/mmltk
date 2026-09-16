#include "src/controller/shell/application_shell.h"
#include "src/controller/browser/client_record.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/presentation/visual_runtime.h"
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
namespace mmltk::controller::shell {
namespace {
[[nodiscard]] std::filesystem::path default_import_socket() {
    static std::atomic<std::uint64_t> sequence{1U};
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path root = runtime && runtime[0] ? runtime : "/tmp";
    return root / ("mmltk-workspace-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                   std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)) + ".sock");
}
void normalize(ApplicationShellConfig::PresentationConfig& config) {
    if (config.numa_node < -1) throw std::invalid_argument("visual numa_node must be -1 or a nonnegative node");
    if (config.import_socket.empty()) config.import_socket = default_import_socket();
    if (config.extent.valid() && config.pitch_bytes == 0U) config.pitch_bytes = static_cast<std::size_t>(config.extent.width) * 4U;
    if (config.cuda_device_index < 0 || !config.extent.valid() || config.pitch_bytes < static_cast<std::size_t>(config.extent.width) * 4U)
        throw std::invalid_argument("invalid presentation configuration");
}
}  // namespace
ApplicationShell::ApplicationShell(ApplicationShellConfig config)
    : diagnostics_client_(std::move(config.diagnostics)),
      runtime_diagnostics_(diagnostics_client_.producer(), config.pixel_probes, config.diagnostic_delivery),
      visual_diagnostic_target_(runtime_diagnostics_.target()),
      browser_host_(browser_server_, runtime_diagnostics_.target()),
      provider_owner_(std::move(config.vast_provider)),
      file_dialog_owner_(config.file_dialog_helper, config.file_dialog_launch_directory),
      presentation_config_(std::move(config.presentation)),
      live_configuration_(std::move(config.live)) {
    normalize(presentation_config_);
    const VisualDeviceSettings base_visual{.device = presentation_config_.cuda_device_index,
                                           .maximum_width = std::max(presentation_config_.extent.width, live_configuration_.capture_width),
                                           .maximum_height = std::max(presentation_config_.extent.height, live_configuration_.capture_height),
                                           .numa_node = presentation_config_.numa_node};
    const auto output_extent = checked_upscale_output_extent({.width = base_visual.maximum_width, .height = base_visual.maximum_height});
    const VisualDeviceSettings output_visual{
        .device = base_visual.device,
        .maximum_width = output_extent.width,
        .maximum_height = output_extent.height,
        .numa_node = base_visual.numa_node,
    };
    const VisualDiagnosticSink diagnostics = visual_diagnostic_sink(visual_diagnostic_target_);
    auto explore_configuration = std::move(config.explore);
    if (explore_configuration.acceptance) browser_host_.install_integration(explore_configuration.acceptance, config.completion_acceptance);
    explore_configuration.diagnostics = diagnostics;
    systems_ = std::make_unique<ApplicationSystemStorage>(
        ApplicationSystemConfiguration{.base_visual = base_visual,
                                       .output_visual = output_visual,
                                       .explore_nproc = 0U,
                                       .explore = std::move(explore_configuration),
                                       .live = live_configuration_,
                                       .presentation = {.import_socket = presentation_config_.import_socket,
                                                        .minimum_allocation_bytes = presentation_config_.minimum_allocation_bytes,
                                                        .pending_supersession_acceptance = config.pending_supersession_acceptance,
                                                        .completion_acceptance = config.completion_acceptance},
                                       .settings_location = std::move(config.settings_location),
                                       .h2d_dataloader = config.h2d_dataloader,
                                       .file_dialog = file_dialog_owner_.client(),
                                       .provider = provider_owner_.client(),
                                       .training_executable = std::move(config.training_executable)},
        [this](browser::SystemEvent event) { browser_host_.publish(std::move(event)); }, diagnostics, [this] { browser_host_.continuity_lost(); });
    if (!browser_host_.install(systems_->application_systems())) {
        browser_host_.close_admission();
        request_system_stops();
        systems_->presentation().BrowserPeerLost();
        static_cast<void>(join_systems());
        throw std::runtime_error("failed to install direct systems");
    }
}
ApplicationShell::~ApplicationShell() noexcept { static_cast<void>(shutdown()); }
bool ApplicationShell::start_browser_host(transport::BrowserServer::Config config) noexcept {
    config.maximum_output_bytes = browser::kMaxRecordWireBytes;
    return !shutdown_requested_.load(std::memory_order_acquire) && browser_host_.accepting() &&
           browser_server_.start(std::move(config), browser_host_.callbacks());
}
services::FirefoxProcessStartResult ApplicationShell::start_firefox(services::FirefoxProcessConfig config) noexcept {
    if (firefox_process_ || shutdown_requested_.load(std::memory_order_acquire)) return services::FirefoxProcessStartResult::Terminal;
    firefox_process_.emplace(presentation_import_socket_path(), std::move(config),
                             services::FirefoxProcessObservationTarget{.context = this,
                                                                       .install_process_group = &ApplicationShell::InstallFirefoxProcessGroup,
                                                                       .submit_observation = &ApplicationShell::FirefoxPhysicalObservation},
                             runtime_diagnostics_.target());
    return firefox_process_->start();
}
services::FirefoxProcessLifecycle ApplicationShell::firefox_lifecycle() const noexcept {
    return firefox_process_ ? firefox_process_->lifecycle() : services::FirefoxProcessLifecycle{.terminal = services::FirefoxProcessTerminal::StartupFailed};
}
const std::filesystem::path& ApplicationShell::presentation_import_socket_path() const noexcept { return presentation_config_.import_socket; }
bool ApplicationShell::InstallFirefoxProcessGroup(void* context, const pid_t group) noexcept {
    return context && static_cast<ApplicationShell*>(context)->install_firefox_process_group(group);
}
void ApplicationShell::FirefoxPhysicalObservation(void* context, services::FirefoxPhysicalObservation observation) noexcept {
    if (context) static_cast<ApplicationShell*>(context)->on_firefox_observation(observation);
}
bool ApplicationShell::install_firefox_process_group(const pid_t group) noexcept {
    if (group <= 0 || shutdown_requested_.load(std::memory_order_acquire)) return false;
    try {
        systems_->presentation().SetExpectedBrowserProcessGroup(group);
        return true;
    } catch (...) { return false; }
}
void ApplicationShell::on_firefox_observation(const services::FirefoxPhysicalObservation observation) noexcept {
    if (observation.kind == services::FirefoxPhysicalObservationKind::ProcessTerminal) {
        emit_shutdown_event("shutdown.firefox_terminal");
        request_shutdown(observation.process.terminal == services::FirefoxProcessTerminal::Exited && observation.process.status == 0
                             ? services::ApplicationShutdownReason::WindowClose
                             : services::ApplicationShutdownReason::FirefoxExit);
        return;
    }
    request_shutdown(services::ApplicationShutdownReason::InfrastructureFailure);
}
void ApplicationShell::request_shutdown(const services::ApplicationShutdownReason reason) noexcept {
    std::call_once(shutdown_request_once_, [this, reason] {
        shutdown_requested_.store(true, std::memory_order_release);
        const auto diagnostics = runtime_diagnostics_.target();
        if (diagnostics.valid())
            diagnostics.Emit([&] {
                return services::RuntimeDiagnosticFact{
                    .owner = contracts::DiagnosticOwner::BrowserRuntime, .event = "shutdown.requested", .detail = static_cast<std::uint64_t>(reason)};
            });
        emit_shutdown_event("shutdown.ingress.started");
        browser_host_.close_admission();
        emit_shutdown_event("shutdown.ingress.completed");
        request_system_stops();
        browser_server_.stop();
        emit_shutdown_event("shutdown.browser_stop.completed");
    });
}
void ApplicationShell::run() {
    if (!browser_server_.running()) throw std::runtime_error("browser server is not running");
    browser_server_.run();
    static_cast<void>(shutdown());
}
bool ApplicationShell::shutdown() noexcept {
    if (shutdown_complete_) return healthy_;
    request_shutdown(services::ApplicationShutdownReason::WindowClose);
    emit_shutdown_event("shutdown.browser_close.started");
    const bool browser_closed = browser_server_.close();
    emit_shutdown_event(browser_closed ? "shutdown.browser_close.completed" : "shutdown.browser_close.failed");
    if (firefox_process_) {
        emit_shutdown_event("shutdown.firefox_wait.started");
        firefox_process_->request_stop();
        firefox_process_->wait();
        emit_shutdown_event("shutdown.firefox_wait.completed");
    }
    systems_->presentation().BrowserPeerLost();
    const bool systems_joined = join_systems();
    const bool resources_closed = browser_closed && systems_joined;
    healthy_ = resources_closed;
    if (const auto target = runtime_diagnostics_.target(); target.valid()) {
        target.write_required({.owner = contracts::DiagnosticOwner::BrowserRuntime,
                               .event = resources_closed ? std::string_view{"shutdown.complete"} : std::string_view{"shutdown.incomplete"}});
    }
    diagnostics_client_.close(services::DiagnosticsCloseMode::Flush);
    diagnostics_client_.wait_closed();
    healthy_ = diagnostics_client_.terminal() != services::DiagnosticsTerminal::Failed && healthy_;
    shutdown_complete_ = true;
    return healthy_;
}
void ApplicationShell::request_system_stops() noexcept {
    emit_shutdown_event("shutdown.system_stops.started");
    systems_->presentation().CloseAdmission();
    static_cast<void>(systems_->live().Stop());
    systems_->upscale().Stop();
    static_cast<void>(systems_->annotation().Stop());
    static_cast<void>(systems_->explore().Stop());
    static_cast<void>(systems_->predict().Stop({}));
    static_cast<void>(systems_->export_system().Stop());
    static_cast<void>(systems_->validation().Stop());
    static_cast<void>(systems_->training().Stop({}));
    static_cast<void>(systems_->model().Stop());
    static_cast<void>(systems_->dataset().Stop());
    static_cast<void>(systems_->file_dialog().Stop());
    emit_shutdown_event("shutdown.system_stops.completed");
}
bool ApplicationShell::join_systems() noexcept {
    if (systems_stopped_) return true;
    if (systems_->presentation().Shutdown() != PresentationShutdownResult::Stopped) return false;
    emit_shutdown_event("shutdown.live_join.started");
    systems_->live().Shutdown();
    emit_shutdown_event("shutdown.live_join.completed");
    emit_shutdown_event("shutdown.upscale_join.started");
    systems_->upscale().Shutdown();
    emit_shutdown_event("shutdown.upscale_join.completed");
    emit_shutdown_event("shutdown.annotation_join.started");
    systems_->annotation().Shutdown();
    emit_shutdown_event("shutdown.annotation_join.completed");
    emit_shutdown_event("shutdown.explore_join.started");
    systems_->explore().Shutdown();
    emit_shutdown_event("shutdown.explore_join.completed");
    emit_shutdown_event("shutdown.predict_join.started");
    systems_->predict().Shutdown();
    emit_shutdown_event("shutdown.predict_join.completed");
    emit_shutdown_event("shutdown.export_system_join.started");
    systems_->export_system().Shutdown();
    emit_shutdown_event("shutdown.export_system_join.completed");
    emit_shutdown_event("shutdown.validation_join.started");
    systems_->validation().Shutdown();
    emit_shutdown_event("shutdown.validation_join.completed");
    emit_shutdown_event("shutdown.training_join.started");
    systems_->training().Shutdown();
    emit_shutdown_event("shutdown.training_join.completed");
    emit_shutdown_event("shutdown.model_join.started");
    systems_->model().Shutdown();
    emit_shutdown_event("shutdown.model_join.completed");
    emit_shutdown_event("shutdown.dataset_join.started");
    systems_->dataset().Shutdown();
    emit_shutdown_event("shutdown.dataset_join.completed");
    emit_shutdown_event("shutdown.file_dialog_join.started");
    systems_->file_dialog().Shutdown();
    emit_shutdown_event("shutdown.file_dialog_join.completed");
    systems_stopped_ = systems_->presentation().stopped() && systems_->live().stopped() && systems_->upscale().stopped() && systems_->annotation().stopped() &&
                       systems_->explore().stopped();
    return systems_stopped_;
}
bool ApplicationShell::healthy() const noexcept { return shutdown_complete_ && healthy_; }
void ApplicationShell::emit_shutdown_event(const std::string_view event) noexcept {
    runtime_diagnostics_.target().Emit([&] {
        return services::RuntimeDiagnosticFact{
            .owner = contracts::DiagnosticOwner::BrowserRuntime,
            .event = event,
        };
    });
}
}  // namespace mmltk::controller::shell
