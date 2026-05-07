#include "include/base/cef_logging.h"

#include "gui/browser_runtime_cef.h"

#include "gui/browser_runtime_backend.h"

#include "browser/host_api.h"
#include "gui/app.h"
#include "gui/app_api.h"
#include "gui/cef_workspace_gpu_bridge.h"
#include "gui/browser_host_helpers.h"
#include "gui/browser_runtime_shared.h"
#include "gui/browser_websocket_server.h"
#include "gui/browser_workspace_surface_bridge.h"
#include "gui/cef_signal_restore.h"
#include "gui/cef_subprocess_app.h"
#include "gui/cef_views/cef_app_runner.h"
#include "gui/cef_views/cef_browser_shell.h"
#include "live/live_worker_logging.h"
#include "mmltk/live/live_pipeline_trace.h"
#include "mmltk_logging.h"

#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_display_handler.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/cef_resource_handler.h"
#include "include/cef_response.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/cef_task.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"
#include "include/internal/cef_linux.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_byte_read_handler.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

namespace {

constexpr auto kRuntimeCapabilityProbeTimeout = std::chrono::seconds(2);
constexpr auto kNativePresentSlotBusyRetryDelay = std::chrono::milliseconds(50);
constexpr int kMainWindowWidth = 1600;
constexpr int kMainWindowHeight = 1000;
constexpr int kPopupWindowWidth = 1200;
constexpr int kPopupWindowHeight = 800;

struct EventPumpWakeState {
    std::atomic<bool> alive{true};
    std::atomic<bool> queued{false};
};

struct RelaunchRequest {
    std::string current_directory;
    std::optional<std::string> settings_path;
    std::optional<std::string> browser_app_dir;
    bool seed_from_cli = false;
    std::vector<std::string> forwarded_arguments;
    std::vector<std::string> argv;
};

namespace runtime_shared = mmltk::gui::browser_runtime_shared;
using namespace runtime_shared;

[[nodiscard]] std::optional<std::string> read_non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] std::string cef_ascii_lowercase_copy(std::string_view value) {
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

[[nodiscard]] bool cef_parse_bool_text(std::string_view value, bool fallback) {
    const std::string lowered = cef_ascii_lowercase_copy(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fallback;
}

[[nodiscard]] bool cef_verbose_logging_enabled() {
    if (const auto value = read_non_empty_env("MMLTK_CEF_VERBOSE_LOGGING"); value.has_value()) {
        return cef_parse_bool_text(*value, false);
    }
    return false;
}

[[nodiscard]] std::filesystem::path resolve_cef_root_cache_path() {
    if (const std::optional<std::string> configured_root = read_non_empty_env("MMLTK_CEF_ROOT_CACHE_PATH");
        configured_root.has_value()) {
        return {*configured_root};
    }

    if (const std::optional<std::string> home = read_non_empty_env("HOME"); home.has_value()) {
        return (std::filesystem::path(*home) / ".config" / "mmltk" / "cef_user_data").lexically_normal();
    }

    return (std::filesystem::temp_directory_path() / "mmltk" / "cef_user_data").lexically_normal();
}

[[nodiscard]] std::filesystem::path resolve_cef_log_file_path(const std::filesystem::path& cef_root_cache_path) {
    if (const auto configured_log_file = read_non_empty_env("MMLTK_CEF_LOG_FILE"); configured_log_file.has_value()) {
        return std::filesystem::path(*configured_log_file).lexically_normal();
    }
    return (cef_root_cache_path / "chrome_debug.log").lexically_normal();
}

[[nodiscard]] mmltk::browser::BrowserRuntimeCapabilities configured_runtime_capabilities() {
    return runtime_shared::configured_runtime_capabilities(mmltk::browser::BrowserHostBackend::Cef);
}

struct CefBundlePaths {
    std::filesystem::path root;
    std::filesystem::path binary_root;
    std::filesystem::path resources_dir;
    std::filesystem::path locales_dir;
    std::filesystem::path libcef;
    std::filesystem::path icudtl;
};

[[nodiscard]] bool has_packaged_cef_bundle_layout(const CefBundlePaths& paths) {
    return std::filesystem::exists(paths.binary_root) && std::filesystem::exists(paths.resources_dir) &&
           std::filesystem::exists(paths.libcef) && std::filesystem::exists(paths.binary_root / "snapshot_blob.bin") &&
           std::filesystem::exists(paths.binary_root / "v8_context_snapshot.bin") &&
           std::filesystem::exists(paths.binary_root / "libEGL.so") &&
           std::filesystem::exists(paths.binary_root / "libGLESv2.so") && std::filesystem::exists(paths.icudtl) &&
           std::filesystem::exists(paths.resources_dir / "resources.pak") &&
           std::filesystem::exists(paths.resources_dir / "chrome_100_percent.pak") &&
           std::filesystem::exists(paths.resources_dir / "chrome_200_percent.pak") &&
           std::filesystem::exists(paths.locales_dir);
}

[[nodiscard]] std::string app_origin() {
    return runtime_shared::app_origin(kAppScheme, kAppHost);
}

[[nodiscard]] std::string workspace_gpu_bridge_publication_key(const std::string_view surface_id,
                                                               const std::string_view revision) {
    return std::string(surface_id) + '\n' + std::string(revision);
}

[[nodiscard]] bool should_log_cadence(const std::uint64_t count) noexcept {
    return count <= 3U || count % 300U == 0U;
}

[[nodiscard]] std::int32_t cef_rect_int(const double value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    constexpr double kMin = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr double kMax = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(std::clamp(std::llround(value), static_cast<long long>(kMin),
                                               static_cast<long long>(kMax)));
}

[[nodiscard]] std::int32_t cef_rect_size(const double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }
    constexpr double kMax = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(std::clamp(std::llround(value), 0LL, static_cast<long long>(kMax)));
}

[[nodiscard]] WorkspaceGpuBridgePresentRects workspace_present_rects(
    const mmltk::browser::WorkspaceBoundsIntent& bounds, const mmltk::live::WorkspacePresentSnapshot& present) {
    const mmltk::browser::WorkspaceSurfaceRect& viewport = bounds.viewport_css;
    WorkspaceGpuBridgePresentRects rects;
    rects.bounds_x = cef_rect_int(viewport.x);
    rects.bounds_y = cef_rect_int(viewport.y);
    rects.bounds_width = cef_rect_size(viewport.width);
    rects.bounds_height = cef_rect_size(viewport.height);

    const mmltk::live::LiveCaptureRegion& dirty = present.dirty_rect;
    const bool dirty_valid = dirty.width > 0U && dirty.height > 0U && present.dims.width > 0U && present.dims.height > 0U;
    if (!dirty_valid) {
        rects.damage_x = rects.bounds_x;
        rects.damage_y = rects.bounds_y;
        rects.damage_width = rects.bounds_width;
        rects.damage_height = rects.bounds_height;
        return rects;
    }

    const double scale_x = viewport.width / static_cast<double>(present.dims.width);
    const double scale_y = viewport.height / static_cast<double>(present.dims.height);
    rects.damage_x = cef_rect_int(viewport.x + static_cast<double>(dirty.x) * scale_x);
    rects.damage_y = cef_rect_int(viewport.y + static_cast<double>(dirty.y) * scale_y);
    rects.damage_width = cef_rect_size(static_cast<double>(dirty.width) * scale_x);
    rects.damage_height = cef_rect_size(static_cast<double>(dirty.height) * scale_y);
    if (rects.damage_width <= 0 || rects.damage_height <= 0) {
        rects.damage_x = rects.bounds_x;
        rects.damage_y = rects.bounds_y;
        rects.damage_width = rects.bounds_width;
        rects.damage_height = rects.bounds_height;
    }
    return rects;
}

[[nodiscard]] std::string browser_entry_uri(const BrowserHostAssetPaths& assets,
                                            const std::optional<std::string>& websocket_url = std::nullopt) {
    std::string uri = runtime_shared::browser_entry_uri(assets, kAppScheme, kAppHost);
    if (websocket_url.has_value() && !websocket_url->empty()) {
        uri += "?mmltk_ws_url=" + runtime_shared::percent_encode(*websocket_url, false);
    }
    return uri;
}

[[nodiscard]] CefBundlePaths classify_cef_bundle_root(const std::filesystem::path& root) {
    CefBundlePaths paths;
    paths.root = root;
    paths.binary_root = root / "Debug";
    paths.resources_dir = root / "Resources";
    paths.locales_dir = paths.resources_dir / "locales";
    paths.libcef = paths.binary_root / "libcef.so";
    paths.icudtl = paths.resources_dir / "icudtl.dat";
    return paths;
}

[[nodiscard]] CefBundlePaths resolve_cef_bundle_paths() {
    std::vector<std::filesystem::path> candidates;
    if (const char* env_root = std::getenv("MMLTK_CEF_RUNTIME_ROOT"); env_root != nullptr && *env_root != '\0') {
        candidates.emplace_back(env_root);
    }
#ifdef MMLTK_CEF_RUNTIME_ROOT_SOURCE
    candidates.emplace_back(MMLTK_CEF_RUNTIME_ROOT_SOURCE);
#endif
    candidates.emplace_back(mmltk::runtime_paths::cef_runtime_root());

    std::vector<std::string> attempted_roots;
    for (const auto& candidate_root : candidates) {
        if (candidate_root.empty()) {
            continue;
        }
        attempted_roots.emplace_back(candidate_root.string());
        const CefBundlePaths paths = classify_cef_bundle_root(candidate_root);
        if (has_packaged_cef_bundle_layout(paths)) {
            return paths;
        }
    }

    std::string message =
        "failed to resolve packaged CEF runtime bundle; expected Debug/"
        "libcef.so, Debug/snapshot_blob.bin, Debug/"
        "v8_context_snapshot.bin, Debug/libEGL.so, Debug/libGLESv2.so, "
        "Resources/icudtl.dat, Resources/resources.pak, Resources/"
        "chrome_100_percent.pak, Resources/chrome_200_percent.pak, and "
        "Resources/locales under one of: ";
    for (std::size_t index = 0; index < attempted_roots.size(); ++index) {
        if (index != 0U) {
            message += ", ";
        }
        message += attempted_roots[index];
    }
    throw std::runtime_error(message);
}

[[nodiscard]] std::optional<std::string> loaded_cef_library_path() {
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&CefInitialize), &info) == 0 || info.dli_fname == nullptr ||
        *info.dli_fname == '\0') {
        return std::nullopt;
    }
    return std::string(info.dli_fname);
}

[[nodiscard]] RelaunchRequest parse_relaunch_request(const CefRefPtr<CefCommandLine>& command_line,
                                                     const CefString& current_directory) {
    RelaunchRequest request;
    request.current_directory = current_directory.ToString();
    if (command_line == nullptr) {
        return request;
    }

    std::vector<CefString> cef_argv;
    command_line->GetArgv(cef_argv);
    request.argv.reserve(cef_argv.size());
    for (const CefString& value : cef_argv) {
        request.argv.push_back(value.ToString());
    }

    const std::string settings_path = command_line->GetSwitchValue("settings-path").ToString();
    if (!settings_path.empty()) {
        request.settings_path = settings_path;
    }
    const std::string browser_app_dir = command_line->GetSwitchValue("browser-app-dir").ToString();
    if (!browser_app_dir.empty()) {
        request.browser_app_dir = browser_app_dir;
    }
    request.seed_from_cli = command_line->HasSwitch("seed-from-cli");

    CefCommandLine::ArgumentList arguments;
    command_line->GetArguments(arguments);
    request.forwarded_arguments.reserve(arguments.size());
    for (const CefString& value : arguments) {
        request.forwarded_arguments.push_back(value.ToString());
    }
    return request;
}

class CefRuntimeClient;
class AppSchemeHandlerFactory;
class CefApplication;

}  // namespace

class EmbeddedCefBrowserRuntime::Impl {
   public:
    Impl(App& app, const AppLaunchOptions& options)
        : app_(app),
          options_(options),
          assets_(resolve_browser_host_asset_paths(options.browser_app_dir)),
          cef_bundle_(resolve_cef_bundle_paths()) {
        bridge_state_.runtime_capabilities = configured_runtime_capabilities();
        reported_runtime_capabilities_ = bridge_state_.runtime_capabilities;
    }

    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void attach_shell(CefBrowserShell& shell) {
        shell_ = &shell;
    }
    [[nodiscard]] bool initialize(int argc, char** argv);
    void run_message_loop();
    void shutdown();
    [[nodiscard]] int exit_code() const noexcept {
        return exit_code_;
    }
    [[nodiscard]] int main_window_width() const noexcept {
        return kMainWindowWidth;
    }
    [[nodiscard]] int main_window_height() const noexcept {
        return kMainWindowHeight;
    }
    [[nodiscard]] int popup_window_width(bool) const noexcept {
        return kPopupWindowWidth;
    }
    [[nodiscard]] int popup_window_height(bool) const noexcept {
        return kPopupWindowHeight;
    }
    [[nodiscard]] std::string_view window_title() const noexcept {
        return window_title_;
    }

    [[nodiscard]] CefRefPtr<CefBrowserView> create_browser_view(const CefRefPtr<CefBrowserViewDelegate>& delegate);
    void on_window_created(const CefRefPtr<CefWindow>& window, bool is_popup);
    void on_window_destroyed(const CefRefPtr<CefWindow>& window, bool is_popup);
    [[nodiscard]] bool on_window_can_close(const CefRefPtr<CefBrowserView>& browser_view) const;
    CefRefPtr<CefBrowser> browser() {
        return browser_;
    }

    void on_context_initialized();
    bool on_already_running_app_relaunch(const CefRefPtr<CefCommandLine>& command_line,
                                         const CefString& current_directory);
    void on_title_change(const CefRefPtr<CefBrowser>& browser, std::string_view title);
    void on_browser_created(const CefRefPtr<CefBrowser>& browser);
    [[nodiscard]] bool on_browser_do_close(const CefRefPtr<CefBrowser>& browser);
    void on_before_close(const CefRefPtr<CefBrowser>& browser);
    void on_load_start(const CefRefPtr<CefBrowser>& browser);
    void on_load_end(const CefRefPtr<CefBrowser>& browser, std::string_view url);
    void on_load_error(const CefRefPtr<CefBrowser>& browser, std::string_view error_text, std::string_view failed_url,
                       int error_code);
    void on_renderer_message(const CefRefPtr<CefBrowser>& browser, const std::string& message_text);
    void on_websocket_message(const std::string& message_text);
    void on_renderer_error(const CefRefPtr<CefBrowser>& browser, const std::string& message_text);
    void on_workspace_gpu_bridge_release(const CefRefPtr<CefBrowser>& browser, std::string_view surface_id,
                                         std::string_view revision);
    void fail_startup(std::string message);

    [[nodiscard]] std::filesystem::path resolve_asset_request_path(std::string_view request_path) const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> load_asset_bytes(
        const std::filesystem::path& resolved);

   private:
    void install_evented_work_wake_callback();
    void pump_evented_work();
    void close_all_browsers(bool force_close);
    void request_quit(bool force_close);
    void fatal_error(std::string message);
    void handle_bridge_message_text(const std::string& payload);
    void note_startup_checkpoint(std::string_view checkpoint);
    void fail_if_startup_checkpoints_missing();
    void reset_runtime_capability_probe();
    void refresh_snapshot_runtime_capabilities(mmltk::browser::StateSnapshot& snapshot);
    void refresh_snapshot();
    void sync_workspace_gpu_bridge_publication();
    [[nodiscard]] bool sync_workspace_native_presented_swapchain();
    void clear_workspace_gpu_bridge_renderer_publication();
    void destroy_workspace_native_presented_swapchain(std::string_view reason);
    void track_workspace_gpu_bridge_publication(const WorkspaceGpuBridgeSurfacePublication& publication);
    void release_tracked_workspace_gpu_bridge_publication(std::string_view surface_id, std::string_view revision);
    void release_all_tracked_workspace_gpu_bridge_publications();
    void flush_pending_messages();
    [[nodiscard]] bool is_main_browser(const CefRefPtr<CefBrowser>& browser) const noexcept;

    App& app_;
    const AppLaunchOptions& options_;
    BrowserHostAssetPaths assets_;
    CefBundlePaths cef_bundle_;
    CefBrowserShell* shell_ = nullptr;
    CefRefPtr<CefApplication> application_cef_;
    CefRefPtr<CefRuntimeClient> client_;
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefWindow> main_window_;
    BrowserSnapshotCache snapshot_cache_;
    BrowserWebSocketServer websocket_server_;
    mmltk::browser::BrowserBridgeState bridge_state_{};
    mmltk::browser::BrowserRuntimeCapabilities reported_runtime_capabilities_{};
    std::string current_snapshot_json_;
    std::string delivered_snapshot_json_;
    std::string delivered_bridge_state_json_;
    std::string pending_error_text_;
    std::string window_title_ = "mmltk";
    std::uint64_t current_snapshot_revision_ = 0U;
    int exit_code_ = 0;
    bool initialized_ = false;
    bool cef_initialized_ = false;
    bool shutdown_ = false;
    bool page_callbacks_ready_ = false;
    bool runtime_capabilities_reported_ = false;
    bool runtime_capability_gate_passed_ = false;
    bool snapshot_dirty_ = true;
    bool bridge_state_dirty_ = true;
    bool error_dirty_ = false;
    bool dispatch_pending_idle_ = false;
    bool workspace_gpu_bridge_state_synced_ = false;
    bool quit_requested_ = false;
    bool cef_context_initialized_ = false;
    bool startup_cef_initialized_ = false;
    bool startup_browser_child_created_ = false;
    bool startup_main_frame_load_started_ = false;
    bool startup_first_browser_visible_ = false;
    std::optional<RelaunchRequest> last_relaunch_request_;
    std::optional<std::chrono::steady_clock::time_point> runtime_capability_deadline_;
    std::optional<WorkspaceGpuBridgeSurfacePublication> delivered_workspace_gpu_bridge_publication_;
    std::unordered_set<std::string> tracked_workspace_gpu_bridge_publication_keys_;
    std::optional<std::chrono::steady_clock::time_point> startup_checkpoint_deadline_;
    std::uint64_t surface_ready_log_count_ = 0;
    std::uint64_t surface_release_log_count_ = 0;
    std::uint64_t native_present_log_count_ = 0;
    std::uint64_t native_present_missing_bounds_log_count_ = 0;
    std::uint64_t native_present_stale_log_count_ = 0;
    std::uint64_t native_present_error_log_count_ = 0;
    std::uint64_t native_present_slot_busy_log_count_ = 0;
    std::uint64_t native_swapchain_configured_generation_ = 0;
    std::uint64_t native_swapchain_last_present_revision_ = 0;
    std::chrono::steady_clock::time_point native_present_slot_busy_retry_after_{};
    bool native_swapchain_configured_ = false;
    std::shared_ptr<EventPumpWakeState> event_pump_wake_state_ = std::make_shared<EventPumpWakeState>();
    std::mutex asset_cache_mutex_;
    std::unordered_map<int, CefRefPtr<CefBrowser>> live_browsers_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> asset_bytes_cache_;

    friend class CefApplication;
    friend class CefRuntimeClient;
};

namespace {

class CefRuntimeClient : public CefClient, public CefDisplayHandler, public CefLifeSpanHandler, public CefLoadHandler {
   public:
    explicit CefRuntimeClient(EmbeddedCefBrowserRuntime::Impl* runtime) : runtime_(runtime) {}

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                     const CefString& errorText, const CefString& failedUrl) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message,
                          const CefString& source, int line) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

   private:
    EmbeddedCefBrowserRuntime::Impl* runtime_ = nullptr;

    IMPLEMENT_REFCOUNTING(CefRuntimeClient);
};

class AssetBytesOwner : public CefBaseRefCounted {
   public:
    explicit AssetBytesOwner(std::shared_ptr<const std::vector<std::uint8_t>> bytes) : bytes_(std::move(bytes)) {}

   private:
    std::shared_ptr<const std::vector<std::uint8_t>> bytes_;

    IMPLEMENT_REFCOUNTING(AssetBytesOwner);
};

class AppSchemeHandlerFactory : public CefSchemeHandlerFactory {
   public:
    explicit AppSchemeHandlerFactory(EmbeddedCefBrowserRuntime::Impl* runtime) : runtime_(runtime) {}

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                         const CefString& scheme_name, CefRefPtr<CefRequest> request) override;

   private:
    EmbeddedCefBrowserRuntime::Impl* runtime_ = nullptr;

    IMPLEMENT_REFCOUNTING(AppSchemeHandlerFactory);
};

class CefApplication : public CefRenderProcessApplication<CefApplication>, public CefBrowserProcessHandler {
   public:
    explicit CefApplication(EmbeddedCefBrowserRuntime::Impl* runtime) : runtime_(runtime) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return runtime_ != nullptr ? this : nullptr;
    }

    void OnContextInitialized() override;
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                     const CefString& current_directory) override;

    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        install_renderer_workspace_context(workspace_gpu_bridge_, browser, frame, context);
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override {
        CEF_REQUIRE_RENDERER_THREAD();
        release_renderer_workspace_context(workspace_gpu_bridge_, browser, frame, context);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
        CEF_REQUIRE_RENDERER_THREAD();
        return dispatch_renderer_workspace_message(workspace_gpu_bridge_, browser, frame, source_process, message);
    }

   private:
    EmbeddedCefBrowserRuntime::Impl* runtime_ = nullptr;
    CefWorkspaceGpuBridge workspace_gpu_bridge_;

    IMPLEMENT_REFCOUNTING(CefApplication);
};

}  // namespace

EmbeddedCefBrowserRuntime::Impl::~Impl() {
    app_.set_evented_work_wake_callback({});
    if (event_pump_wake_state_) {
        event_pump_wake_state_->alive.store(false, std::memory_order_release);
        event_pump_wake_state_->queued.store(false, std::memory_order_release);
    }
    shutdown();
}

bool EmbeddedCefBrowserRuntime::Impl::initialize(const int argc, char** argv) {
    if (initialized_) {
        return cef_initialized_;
    }

    if (!std::filesystem::exists(assets_.index_html)) {
        throw std::runtime_error("missing packaged browser app index: " + assets_.index_html.string());
    }

    CefMainArgs main_args(argc, argv);

    CefSettings settings;
    settings.no_sandbox = 1;
    settings.multi_threaded_message_loop = 0;
    settings.external_message_pump = 0;
    settings.windowless_rendering_enabled = 0;
    const std::filesystem::path cef_root_cache_path = resolve_cef_root_cache_path();
    std::error_code cache_dir_error;
    (void)std::filesystem::create_directories(cef_root_cache_path, cache_dir_error);
    const std::filesystem::path cef_log_file_path = resolve_cef_log_file_path(cef_root_cache_path);
    std::error_code log_dir_error;
    if (const std::filesystem::path cef_log_dir = cef_log_file_path.parent_path(); !cef_log_dir.empty()) {
        (void)std::filesystem::create_directories(cef_log_dir, log_dir_error);
    }
    CefString(&settings.root_cache_path) = cef_root_cache_path.string();
    CefString(&settings.cache_path) = cef_root_cache_path.string();
    const std::filesystem::path helper_path =
        mmltk::runtime_paths::current_executable_path().parent_path() / "mmltk-browser-helper";
    const std::string cef_runtime_root = cef_bundle_.root.string();
    const std::string cef_resources_dir = cef_bundle_.resources_dir.string();
    const std::string cef_locales_dir = cef_bundle_.locales_dir.string();
    (void)::setenv("MMLTK_CEF_RUNTIME_ROOT", cef_runtime_root.c_str(), 1);
    (void)::setenv("MMLTK_CEF_RESOURCES_DIR", cef_resources_dir.c_str(), 1);
    (void)::setenv("MMLTK_CEF_LOCALES_DIR", cef_locales_dir.c_str(), 1);
    const std::string cef_log_file = cef_log_file_path.string();
    (void)::setenv("MMLTK_CEF_LOG_FILE", cef_log_file.c_str(), 1);
    CefString(&settings.browser_subprocess_path) = helper_path.string();
    CefString(&settings.resources_dir_path) = cef_resources_dir;
    CefString(&settings.locales_dir_path) = cef_locales_dir;
    CefString(&settings.log_file) = cef_log_file;
    settings.log_severity = cef_verbose_logging_enabled() ? LOGSEVERITY_INFO : LOGSEVERITY_WARNING;

    const auto logger = mmltk::logging::logger("gui");
    const CefRuntimeLaunchConfig& launch_config = cef_runtime_launch_config();
    logger->info("embedded browser runtime serving packaged app {}", assets_.index_html.string());
    logger->info("embedded browser runtime entry {}", browser_entry_uri(assets_));
    logger->info("embedded CEF WebGPU runtime selection `{}`", cef_webgpu_runtime_name(launch_config.webgpu_runtime));
    logger->info("embedded CEF runtime bundle root `{}`", cef_bundle_.root.string());
    logger->info("embedded CEF runtime binary root `{}`", cef_bundle_.binary_root.string());
    logger->info("embedded CEF runtime libcef `{}`", cef_bundle_.libcef.string());
    if (const auto loaded_libcef = loaded_cef_library_path(); loaded_libcef.has_value()) {
        logger->info("embedded CEF loaded libcef `{}`", *loaded_libcef);
    }
    logger->info(
        "embedded CEF compositor startup: windowless_rendering_enabled={} multi_threaded_message_loop={} "
        "external_message_pump={} ozone_platform=wayland webgpu_runtime={} unsafe_webgpu={} "
        "force_high_performance_gpu={} gpu_switches_logged_by_command_line_hook=true",
        settings.windowless_rendering_enabled, settings.multi_threaded_message_loop, settings.external_message_pump,
        cef_webgpu_runtime_name(launch_config.webgpu_runtime), launch_config.enable_unsafe_webgpu,
        launch_config.force_high_performance_gpu);
    logger->info("embedded CEF root cache path `{}`", cef_root_cache_path.string());
    logger->info("embedded CEF log file path `{}`", cef_log_file);
    if (cache_dir_error) {
        logger->warn("embedded CEF root cache directory create failed: {}", cache_dir_error.message());
    }
    if (log_dir_error) {
        logger->warn("embedded CEF log directory create failed: {}", log_dir_error.message());
    }

    application_cef_ = new CefApplication(this);
    backup_posix_signal_handlers();
    const bool cef_initialized_ok = CefInitialize(main_args, settings, application_cef_, nullptr);
    restore_posix_signal_handlers();
    if (!cef_initialized_ok) {
        exit_code_ = CefGetExitCode();
        application_cef_ = nullptr;
        return false;
    }

    cef_initialized_ = true;
    install_evented_work_wake_callback();
    if (!websocket_server_.start({.on_message = [this](std::string message_text) {
            CefPostTask(TID_UI, base::BindOnce(
                                    [](EmbeddedCefBrowserRuntime::Impl* runtime, const std::string& payload) {
                                        if (runtime != nullptr) {
                                            runtime->on_websocket_message(payload);
                                        }
                                    },
                                    this, std::move(message_text)));
        }})) {
        CefShutdown();
        cef_initialized_ = false;
        application_cef_ = nullptr;
        throw std::runtime_error("failed to start embedded browser WebSocket transport");
    }
    logger->info("embedded browser runtime WebSocket entry {}", browser_entry_uri(assets_, websocket_server_.url()));
    initialized_ = true;
    startup_checkpoint_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pump_evented_work();
    return true;
}

void EmbeddedCefBrowserRuntime::Impl::run_message_loop() {
    if (!cef_initialized_) {
        return;
    }
    CefRunMessageLoop();
}

void EmbeddedCefBrowserRuntime::Impl::shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    app_.set_evented_work_wake_callback({});
    if (event_pump_wake_state_) {
        event_pump_wake_state_->alive.store(false, std::memory_order_release);
        event_pump_wake_state_->queued.store(false, std::memory_order_release);
    }
    websocket_server_.stop();

    bool browsers_closed = live_browsers_.empty();
    if (cef_initialized_ && !browsers_closed) {
        close_all_browsers(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!live_browsers_.empty() && std::chrono::steady_clock::now() < deadline) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        browsers_closed = live_browsers_.empty();
    }

    if (cef_initialized_) {
        destroy_workspace_native_presented_swapchain("runtime_shutdown");
        release_all_tracked_workspace_gpu_bridge_publications();
        app_.release_all_workspace_surface_publications();
        if (!browsers_closed) {
            mmltk::logging::logger("gui")->error(
                "embedded browser runtime shutdown timed out waiting for browser close; "
                "skipping CefShutdown with live browsers");
        } else {
            CefShutdown();
        }
    }

    cef_initialized_ = false;
    initialized_ = false;
    live_browsers_.clear();
    main_window_ = nullptr;
    browser_ = nullptr;
    client_ = nullptr;
    application_cef_ = nullptr;
}

CefRefPtr<CefBrowserView> EmbeddedCefBrowserRuntime::Impl::create_browser_view(
    const CefRefPtr<CefBrowserViewDelegate>& delegate) {
    CEF_REQUIRE_UI_THREAD();
    if (!cef_context_initialized_) {
        throw std::runtime_error("attempted to create a CEF browser view before context initialization");
    }
    if (delegate == nullptr) {
        throw std::runtime_error("CEF browser view delegate must not be null");
    }
    if (client_ == nullptr) {
        client_ = new CefRuntimeClient(this);
    }

    CefBrowserSettings browser_settings;
    browser_settings.background_color = CefColorSetARGB(255, 0x07, 0x13, 0x1A);

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        client_, browser_entry_uri(assets_, websocket_server_.url()), browser_settings, nullptr, nullptr, delegate);
    if (browser_view == nullptr) {
        throw std::runtime_error("failed to create the CEF browser view");
    }

    refresh_snapshot();
    runtime_shared::queue_bridge_state(bridge_state_dirty_);
    return browser_view;
}

void EmbeddedCefBrowserRuntime::Impl::on_window_created(const CefRefPtr<CefWindow>& window, const bool is_popup) {
    CEF_REQUIRE_UI_THREAD();
    if (window == nullptr) {
        return;
    }

    if (!is_popup) {
        main_window_ = window;
        main_window_->SetTitle(CefString(window_title_));
        note_startup_checkpoint("first browser visible");
    }
}

void EmbeddedCefBrowserRuntime::Impl::on_window_destroyed(const CefRefPtr<CefWindow>& window, const bool is_popup) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_popup && main_window_ != nullptr && window != nullptr && main_window_->IsSame(window)) {
        main_window_ = nullptr;
    }
}

bool EmbeddedCefBrowserRuntime::Impl::on_window_can_close(const CefRefPtr<CefBrowserView>& browser_view) const {
    CEF_REQUIRE_UI_THREAD();
    if (browser_view == nullptr) {
        return true;
    }
    CefRefPtr<CefBrowser> browser = browser_view->GetBrowser();
    return browser == nullptr ? true : browser->GetHost()->TryCloseBrowser();
}

void EmbeddedCefBrowserRuntime::Impl::on_context_initialized() {
    CEF_REQUIRE_UI_THREAD();

    if (!CefRegisterSchemeHandlerFactory(kAppScheme.data(), kAppHost.data(), new AppSchemeHandlerFactory(this))) {
        throw std::runtime_error("failed to register CEF app:// scheme handler");
    }
    if (shell_ == nullptr) {
        throw std::runtime_error("embedded CEF Views shell is not attached");
    }

    cef_context_initialized_ = true;
    note_startup_checkpoint("cef initialized");
    refresh_snapshot();
    runtime_shared::queue_bridge_state(bridge_state_dirty_);
    shell_->create_main_window();
    flush_pending_messages();
}

bool EmbeddedCefBrowserRuntime::Impl::on_already_running_app_relaunch(const CefRefPtr<CefCommandLine>& command_line,
                                                                      const CefString& current_directory) {
    CEF_REQUIRE_UI_THREAD();
    last_relaunch_request_ = parse_relaunch_request(command_line, current_directory);
    const auto logger = mmltk::logging::logger("gui");
    logger->info(
        "embedded CEF relaunch received settings_path=`{}`, browser_app_dir=`{}`, "
        "seed_from_cli={}, forwarded_args={}",
        last_relaunch_request_->settings_path.value_or(options_.settings_path),
        last_relaunch_request_->browser_app_dir.value_or(options_.browser_app_dir),
        last_relaunch_request_->seed_from_cli, last_relaunch_request_->forwarded_arguments.size());

    if (main_window_ != nullptr) {
        main_window_->Activate();
        main_window_->BringToTop();
    }
    if (browser_ != nullptr) {
        browser_->GetHost()->SetFocus(true);
    }
    return true;
}

void EmbeddedCefBrowserRuntime::Impl::on_title_change(const CefRefPtr<CefBrowser>& browser,
                                                      const std::string_view title) {
    CEF_REQUIRE_UI_THREAD();

    if (const CefRefPtr<CefBrowserView> browser_view = CefBrowserView::GetForBrowser(browser);
        browser_view != nullptr) {
        if (const CefRefPtr<CefWindow> window = browser_view->GetWindow(); window != nullptr) {
            window->SetTitle(CefString(std::string(title)));
        }
    }

    if (!is_main_browser(browser)) {
        return;
    }

    if (!title.empty()) {
        window_title_.assign(title);
    }
    if (main_window_ != nullptr) {
        main_window_->SetTitle(CefString(window_title_));
    }
}

void EmbeddedCefBrowserRuntime::Impl::on_browser_created(const CefRefPtr<CefBrowser>& browser) {
    CEF_REQUIRE_UI_THREAD();
    if (browser == nullptr) {
        return;
    }

    live_browsers_[browser->GetIdentifier()] = browser;
    if (!browser->IsPopup() && browser_ == nullptr) {
        browser_ = browser;
        note_startup_checkpoint("browser created");
        pump_evented_work();
    }
    if (quit_requested_) {
        browser->GetHost()->CloseBrowser(true);
    }
}

bool EmbeddedCefBrowserRuntime::Impl::is_main_browser(const CefRefPtr<CefBrowser>& browser) const noexcept {
    return browser != nullptr && browser_ != nullptr && browser_->IsSame(browser);
}

bool EmbeddedCefBrowserRuntime::Impl::on_browser_do_close(const CefRefPtr<CefBrowser>&) {
    CEF_REQUIRE_UI_THREAD();
    return false;
}

void EmbeddedCefBrowserRuntime::Impl::on_before_close(const CefRefPtr<CefBrowser>& browser) {
    CEF_REQUIRE_UI_THREAD();
    if (browser == nullptr) {
        return;
    }

    const bool was_main = is_main_browser(browser);
    live_browsers_.erase(browser->GetIdentifier());
    if (was_main) {
        destroy_workspace_native_presented_swapchain("browser_before_close");
        release_all_tracked_workspace_gpu_bridge_publications();
        workspace_gpu_bridge_state_synced_ = false;
        delivered_workspace_gpu_bridge_publication_.reset();
        browser_ = nullptr;
        request_quit(true);
    } else if (quit_requested_ && live_browsers_.empty()) {
        CefQuitMessageLoop();
    }
}

void EmbeddedCefBrowserRuntime::Impl::on_load_start(const CefRefPtr<CefBrowser>& browser) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser)) {
        return;
    }

    note_startup_checkpoint("main-frame load start");
    destroy_workspace_native_presented_swapchain("main_frame_load_start");
    release_all_tracked_workspace_gpu_bridge_publications();
    app_.release_all_workspace_surface_publications();
    runtime_shared::clear_delivery_state(
        page_callbacks_ready_, delivered_snapshot_json_, delivered_bridge_state_json_, pending_error_text_,
        error_dirty_, dispatch_pending_idle_, current_snapshot_json_, snapshot_dirty_, bridge_state_,
        [this] { reset_runtime_capability_probe(); }, bridge_state_dirty_);
    workspace_gpu_bridge_state_synced_ = false;
    delivered_workspace_gpu_bridge_publication_.reset();
}

void EmbeddedCefBrowserRuntime::Impl::on_load_end(const CefRefPtr<CefBrowser>& browser, std::string_view url) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser)) {
        return;
    }

    mmltk::logging::logger("gui")->info("embedded browser runtime loaded {}", url);
    CefRefPtr<CefFrame> main_frame = browser->GetMainFrame();
    if (main_frame != nullptr) {
        main_frame->ExecuteJavaScript(ready_watcher_script().c_str(), CefString(std::string(url)), 0);
    }
    pump_evented_work();
}

void EmbeddedCefBrowserRuntime::Impl::on_load_error(const CefRefPtr<CefBrowser>& browser, std::string_view error_text,
                                                    std::string_view failed_url, const int error_code) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser)) {
        return;
    }

    fatal_error("failed to load browser URL `" + std::string(failed_url) + "`: " + std::string(error_text) + " (" +
                std::to_string(error_code) + ")");
}

void EmbeddedCefBrowserRuntime::Impl::on_renderer_message(const CefRefPtr<CefBrowser>& browser,
                                                          const std::string& message_text) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser)) {
        return;
    }
    handle_bridge_message_text(message_text);
    pump_evented_work();
}

void EmbeddedCefBrowserRuntime::Impl::on_websocket_message(const std::string& message_text) {
    CEF_REQUIRE_UI_THREAD();
    if (shutdown_) {
        return;
    }
    handle_bridge_message_text(message_text);
    pump_evented_work();
}

void EmbeddedCefBrowserRuntime::Impl::on_renderer_error(const CefRefPtr<CefBrowser>& browser,
                                                        const std::string& message_text) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser)) {
        return;
    }
    runtime_shared::set_bridge_error(bridge_state_, message_text, true, bridge_state_dirty_, pending_error_text_,
                                     error_dirty_);
    flush_pending_messages();
}

void EmbeddedCefBrowserRuntime::Impl::fail_startup(std::string message) {
    CEF_REQUIRE_UI_THREAD();
    fatal_error(std::move(message));
}

std::filesystem::path EmbeddedCefBrowserRuntime::Impl::resolve_asset_request_path(std::string_view request_path) const {
    std::string relative_text = request_path.empty() || request_path == "/" ? "/" : std::string(request_path);
    if (const std::size_t query_pos = relative_text.find_first_of("?#"); query_pos != std::string::npos) {
        relative_text.resize(query_pos);
    }
    if (relative_text.empty() || relative_text == "/") {
        relative_text = "/" + std::filesystem::relative(assets_.index_html, assets_.bundle_root).generic_string();
    }

    while (!relative_text.empty() && relative_text.front() == '/') {
        relative_text.erase(relative_text.begin());
    }

    const std::filesystem::path relative_path(percent_decode(relative_text));
    const std::filesystem::path resolved = (assets_.bundle_root / relative_path).lexically_normal();
    if (!path_is_within_root(assets_.bundle_root, resolved) || !std::filesystem::is_regular_file(resolved)) {
        throw std::runtime_error("asset not found");
    }
    return resolved;
}

std::shared_ptr<const std::vector<std::uint8_t>> EmbeddedCefBrowserRuntime::Impl::load_asset_bytes(
    const std::filesystem::path& resolved) {
    const std::string key = resolved.generic_string();
    {
        std::scoped_lock lock(asset_cache_mutex_);
        const auto cached = asset_bytes_cache_.find(key);
        if (cached != asset_bytes_cache_.end()) {
            return cached->second;
        }
    }

    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(read_binary_file(resolved));

    std::scoped_lock lock(asset_cache_mutex_);
    const auto [it, inserted] = asset_bytes_cache_.emplace(key, bytes);
    return inserted ? bytes : it->second;
}

void EmbeddedCefBrowserRuntime::Impl::install_evented_work_wake_callback() {
    if (!event_pump_wake_state_) {
        event_pump_wake_state_ = std::make_shared<EventPumpWakeState>();
    }
    event_pump_wake_state_->alive.store(true, std::memory_order_release);
    event_pump_wake_state_->queued.store(false, std::memory_order_release);
    std::weak_ptr<EventPumpWakeState> weak_state(event_pump_wake_state_);
    app_.set_evented_work_wake_callback([this, weak_state]() {
        const std::shared_ptr<EventPumpWakeState> state = weak_state.lock();
        if (!state || !state->alive.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (!state->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return;
        }

        const bool posted = CefPostTask(
            TID_UI, base::BindOnce(
                        [](const std::weak_ptr<EventPumpWakeState>& posted_weak_state,
                           EmbeddedCefBrowserRuntime::Impl* runtime) {
                            const std::shared_ptr<EventPumpWakeState> posted_state = posted_weak_state.lock();
                            if (!posted_state || !posted_state->alive.load(std::memory_order_acquire)) {
                                return;
                            }
                            posted_state->queued.store(false, std::memory_order_release);
                            if (runtime != nullptr) {
                                runtime->pump_evented_work();
                            }
                        },
                        weak_state, this));
        if (!posted) {
            state->queued.store(false, std::memory_order_release);
        }
    });
}

void EmbeddedCefBrowserRuntime::Impl::pump_evented_work() {
    CEF_REQUIRE_UI_THREAD();
    if (!initialized_ || shutdown_) {
        return;
    }
    try {
        drain_background_work(app_);
        refresh_snapshot();
        fail_if_startup_checkpoints_missing();
        runtime_shared::fail_if_runtime_capability_probe_expired(
            page_callbacks_ready_, runtime_capabilities_reported_, runtime_capability_gate_passed_,
            runtime_capability_deadline_, [this](const std::string& message) { fatal_error(message); });
        if (dispatch_pending_idle_ && runtime_capability_gate_passed_) {
            dispatch_pending_idle_ = false;
            runtime_shared::update_bridge_phase(bridge_state_, mmltk::browser::BrowserBridgePhase::Idle,
                                                bridge_state_dirty_);
        }
        flush_pending_messages();
    } catch (const std::exception& error) {
        fatal_error(std::string("browser runtime event pump failed: ") + error.what());
    }
}

void EmbeddedCefBrowserRuntime::Impl::note_startup_checkpoint(std::string_view checkpoint) {
    bool* flag = nullptr;
    if (checkpoint == "cef initialized") {
        flag = &startup_cef_initialized_;
    } else if (checkpoint == "browser created") {
        flag = &startup_browser_child_created_;
    } else if (checkpoint == "main-frame load start") {
        flag = &startup_main_frame_load_started_;
    } else if (checkpoint == "first browser visible") {
        flag = &startup_first_browser_visible_;
    }
    if (flag == nullptr || *flag) {
        return;
    }
    *flag = true;
    mmltk::logging::logger("gui")->info("embedded browser runtime startup checkpoint: {}", checkpoint);
    if (startup_cef_initialized_ && startup_browser_child_created_ && startup_main_frame_load_started_ &&
        startup_first_browser_visible_) {
        startup_checkpoint_deadline_.reset();
    }
}

void EmbeddedCefBrowserRuntime::Impl::fail_if_startup_checkpoints_missing() {
    if (!startup_checkpoint_deadline_.has_value() || quit_requested_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < *startup_checkpoint_deadline_) {
        return;
    }

    const auto fail_missing = [this](std::string_view checkpoint) {
        fatal_error("embedded browser shell startup stalled before `" + std::string(checkpoint) + "`");
    };

    if (!startup_cef_initialized_) {
        fail_missing("cef initialized");
        return;
    }
    if (!startup_browser_child_created_) {
        fail_missing("browser created");
        return;
    }
    if (!startup_main_frame_load_started_) {
        fail_missing("main-frame load start");
        return;
    }
    if (!startup_first_browser_visible_) {
        fail_missing("first browser visible");
        return;
    }
    startup_checkpoint_deadline_.reset();
}

void EmbeddedCefBrowserRuntime::Impl::close_all_browsers(const bool force_close) {
    CEF_REQUIRE_UI_THREAD();
    std::vector<CefRefPtr<CefBrowser>> browsers;
    browsers.reserve(live_browsers_.size());
    for (const auto& [id, browser] : live_browsers_) {
        (void)id;
        if (browser != nullptr) {
            browsers.push_back(browser);
        }
    }
    for (const CefRefPtr<CefBrowser>& browser : browsers) {
        browser->GetHost()->CloseBrowser(force_close);
    }
}

void EmbeddedCefBrowserRuntime::Impl::request_quit(const bool force_close) {
    CEF_REQUIRE_UI_THREAD();
    if (!quit_requested_) {
        quit_requested_ = true;
        destroy_workspace_native_presented_swapchain("runtime_quit");
        release_all_tracked_workspace_gpu_bridge_publications();
        app_.release_all_workspace_surface_publications();
    }
    if (!live_browsers_.empty()) {
        close_all_browsers(force_close);
        return;
    }
    CefQuitMessageLoop();
}

void EmbeddedCefBrowserRuntime::Impl::reset_runtime_capability_probe() {
    runtime_shared::reset_runtime_capability_probe(
        bridge_state_, runtime_capabilities_reported_, runtime_capability_gate_passed_, runtime_capability_deadline_,
        [] { return configured_runtime_capabilities(); },
        [this](const mmltk::browser::BrowserRuntimeCapabilities& capabilities) {
            reported_runtime_capabilities_ = capabilities;
        });
}

void EmbeddedCefBrowserRuntime::Impl::refresh_snapshot_runtime_capabilities(mmltk::browser::StateSnapshot& snapshot) {
    mmltk::browser::BrowserRuntimeCapabilities capabilities = configured_runtime_capabilities();
    capabilities.navigator_gpu = reported_runtime_capabilities_.navigator_gpu;
    capabilities.workspace_surface_bridge = reported_runtime_capabilities_.workspace_surface_bridge;
    capabilities.workspace_surface_zero_copy = reported_runtime_capabilities_.workspace_surface_zero_copy;
    runtime_shared::publish_runtime_capabilities(capabilities, snapshot, bridge_state_, bridge_state_dirty_);
}

void EmbeddedCefBrowserRuntime::Impl::refresh_snapshot() {
    runtime_shared::refresh_snapshot(
        app_, snapshot_cache_,
        [this](mmltk::browser::StateSnapshot& snapshot) { refresh_snapshot_runtime_capabilities(snapshot); },
        current_snapshot_json_, current_snapshot_revision_, snapshot_dirty_, delivered_snapshot_json_);
}

void EmbeddedCefBrowserRuntime::Impl::track_workspace_gpu_bridge_publication(
    const WorkspaceGpuBridgeSurfacePublication& publication) {
    tracked_workspace_gpu_bridge_publication_keys_.insert(
        workspace_gpu_bridge_publication_key(publication.surface_info.surface_id, publication.surface_info.revision));
}

void EmbeddedCefBrowserRuntime::Impl::release_tracked_workspace_gpu_bridge_publication(
    const std::string_view surface_id, const std::string_view revision) {
    if (surface_id.empty() || revision.empty()) {
        return;
    }
    const std::string key = workspace_gpu_bridge_publication_key(surface_id, revision);
    if (tracked_workspace_gpu_bridge_publication_keys_.erase(key) == 0U) {
        return;
    }
    release_workspace_gpu_bridge_publication(surface_id, revision);
    if (delivered_workspace_gpu_bridge_publication_.has_value() &&
        delivered_workspace_gpu_bridge_publication_->surface_info.surface_id == surface_id &&
        delivered_workspace_gpu_bridge_publication_->surface_info.revision == revision) {
        delivered_workspace_gpu_bridge_publication_.reset();
        workspace_gpu_bridge_state_synced_ = false;
    }
}

void EmbeddedCefBrowserRuntime::Impl::release_all_tracked_workspace_gpu_bridge_publications() {
    for (const std::string& key : tracked_workspace_gpu_bridge_publication_keys_) {
        const std::size_t separator = key.find('\n');
        if (separator == std::string::npos) {
            continue;
        }
        release_workspace_gpu_bridge_publication(std::string_view(key).substr(0U, separator),
                                                 std::string_view(key).substr(separator + 1U));
    }
    tracked_workspace_gpu_bridge_publication_keys_.clear();
    delivered_workspace_gpu_bridge_publication_.reset();
    workspace_gpu_bridge_state_synced_ = false;
}

void EmbeddedCefBrowserRuntime::Impl::clear_workspace_gpu_bridge_renderer_publication() {
    if (!tracked_workspace_gpu_bridge_publication_keys_.empty() ||
        delivered_workspace_gpu_bridge_publication_.has_value()) {
        release_all_tracked_workspace_gpu_bridge_publications();
    }
    if (!workspace_gpu_bridge_state_synced_) {
        send_workspace_gpu_bridge_publication(browser_, std::nullopt);
        delivered_workspace_gpu_bridge_publication_.reset();
        workspace_gpu_bridge_state_synced_ = true;
    }
}

void EmbeddedCefBrowserRuntime::Impl::destroy_workspace_native_presented_swapchain(const std::string_view reason) {
    CEF_REQUIRE_UI_THREAD();
    if (!native_swapchain_configured_) {
        native_swapchain_configured_generation_ = 0U;
        native_swapchain_last_present_revision_ = 0U;
        return;
    }

    const WorkspaceGpuBridgeCefResult result =
        destroy_cef_workspace_gpu_bridge_swapchain(kBrowserWorkspaceSurfaceId);
    if (result.ok) {
        mmltk::logging::logger("gui")->info(
            "workspace GPU bridge native swapchain destroyed: generation={} reason={} detail={}",
            native_swapchain_configured_generation_, reason, result.message("ok"));
    } else {
        mmltk::logging::logger("gui")->warn(
            "workspace GPU bridge native swapchain destroy rejected: generation={} reason={} detail={}",
            native_swapchain_configured_generation_, reason, result.message("swapchain_destroy_failed"));
    }
    native_swapchain_configured_ = false;
    native_swapchain_configured_generation_ = 0U;
    native_swapchain_last_present_revision_ = 0U;
    native_present_log_count_ = 0U;
    native_present_slot_busy_log_count_ = 0U;
    native_present_slot_busy_retry_after_ = {};
}

bool EmbeddedCefBrowserRuntime::Impl::sync_workspace_native_presented_swapchain() {
    CEF_REQUIRE_UI_THREAD();
    const BrowserWorkspaceNativePresentState state = app_.browser_workspace_native_present_state();
    if (state.swapchain_descriptor == nullptr) {
        destroy_workspace_native_presented_swapchain("native_present_state_inactive");
        return false;
    }

    clear_workspace_gpu_bridge_renderer_publication();
    if (!state.ready()) {
        return true;
    }
    if (!cef_workspace_gpu_bridge_swapchain_present_supported()) {
        native_present_error_log_count_ += 1U;
        if (should_log_cadence(native_present_error_log_count_)) {
            mmltk::logging::logger("gui")->warn(
                "workspace GPU bridge native present unavailable: CEF swapchain C ABI helpers are absent");
        }
        app_.record_workspace_surface_bridge_error(
            "workspace GPU bridge native-present C ABI helpers are unavailable");
        return true;
    }
    if (!state.bounds.has_value()) {
        native_present_missing_bounds_log_count_ += 1U;
        const bool log_this_skip = should_log_cadence(native_present_missing_bounds_log_count_);
        if (log_this_skip) {
            mmltk::logging::logger("gui")->warn(
                "workspace GPU bridge native present skipped: missing browser workspace bounds generation={} "
                "revision={} slot={}",
                state.latest_present.swapchain_generation, state.latest_present.revision,
                state.latest_present.front_slot_index);
            app_.record_workspace_surface_bridge_error(
                "workspace GPU bridge native present is waiting for workspace bounds");
        }
        return true;
    }

    const mmltk::live::WorkspaceSwapchainDescriptor& descriptor = *state.swapchain_descriptor;
    if (!native_swapchain_configured_ || native_swapchain_configured_generation_ != descriptor.generation) {
        if (native_swapchain_configured_) {
            destroy_workspace_native_presented_swapchain("generation_replacement");
        }
        const WorkspaceGpuBridgeCefResult configure_result = configure_cef_workspace_gpu_bridge_swapchain(
            kBrowserWorkspaceSurfaceId, descriptor, state.cuda_device_index);
        if (!configure_result.ok) {
            native_present_error_log_count_ += 1U;
            if (should_log_cadence(native_present_error_log_count_)) {
                mmltk::logging::logger("gui")->warn(
                    "workspace GPU bridge native swapchain configure rejected: generation={} slots={} detail={}",
                    descriptor.generation, descriptor.slots.size(), configure_result.message("configure_failed"));
            }
            app_.record_workspace_surface_bridge_error("workspace GPU bridge native swapchain configure failed: " +
                                                       configure_result.message("configure_failed"));
            return true;
        }
        native_swapchain_configured_ = true;
        native_swapchain_configured_generation_ = descriptor.generation;
        native_swapchain_last_present_revision_ = 0U;
        mmltk::logging::logger("gui")->info(
            "workspace GPU bridge native swapchain configured: generation={} slots={} dimensions={}x{} detail={}",
            descriptor.generation, descriptor.slots.size(), descriptor.width, descriptor.height,
            configure_result.message("ok"));
    }

    const mmltk::live::WorkspacePresentSnapshot& present = state.latest_present;
    if (present.revision <= native_swapchain_last_present_revision_) {
        native_present_stale_log_count_ += 1U;
        if (should_log_cadence(native_present_stale_log_count_)) {
            mmltk::logging::logger("gui")->info(
                "workspace GPU bridge native present skipped stale revision: revision={} last={} generation={} slot={}",
                present.revision, native_swapchain_last_present_revision_, present.swapchain_generation,
                present.front_slot_index);
        }
        return true;
    }

    const WorkspaceGpuBridgePresentRects rects = workspace_present_rects(state.bounds->bounds, present);
    const auto now = std::chrono::steady_clock::now();
    if (native_present_slot_busy_retry_after_ != std::chrono::steady_clock::time_point{} &&
        now < native_present_slot_busy_retry_after_) {
        return true;
    }

    const WorkspaceGpuBridgeCefResult present_result =
        present_cef_workspace_gpu_bridge_front_slot(kBrowserWorkspaceSurfaceId, present, rects);
    if (!present_result.ok) {
        if (present_result.result_code == "slot_display_busy") {
            native_present_slot_busy_log_count_ += 1U;
            native_present_slot_busy_retry_after_ = now + kNativePresentSlotBusyRetryDelay;
            if (should_log_cadence(native_present_slot_busy_log_count_)) {
                mmltk::logging::logger("gui")->info(
                    "workspace GPU bridge native present skipped busy front slot: generation={} revision={} slot={} "
                    "busy_skips={} detail={}",
                    present.swapchain_generation, present.revision, present.front_slot_index,
                    native_present_slot_busy_log_count_, present_result.message("slot_busy"));
            }
            return true;
        }
        native_present_error_log_count_ += 1U;
        if (should_log_cadence(native_present_error_log_count_)) {
            mmltk::logging::logger("gui")->warn(
                "workspace GPU bridge native present rejected: generation={} revision={} slot={} bounds={}x{}+{},{} "
                "damage={}x{}+{},{} detail={}",
                present.swapchain_generation, present.revision, present.front_slot_index, rects.bounds_width,
                rects.bounds_height, rects.bounds_x, rects.bounds_y, rects.damage_width, rects.damage_height,
                rects.damage_x, rects.damage_y, present_result.message("present_failed"));
        }
        app_.record_workspace_surface_bridge_error("workspace GPU bridge native present failed: " +
                                                   present_result.message("present_failed"));
        return true;
    }

    native_swapchain_last_present_revision_ = present.revision;
    native_present_slot_busy_retry_after_ = {};
    app_.record_workspace_surface_pipeline_stage(mmltk::live::LivePipelineStage::CefPresentFrontSlot,
                                                 kBrowserWorkspaceSurfaceId, std::to_string(present.revision),
                                                 present.capture_ns);
    native_present_log_count_ += 1U;
    if (should_log_cadence(native_present_log_count_)) {
        mmltk::logging::logger("gui")->info(
            "workspace GPU bridge native present: frame={} revision={} slot={} generation={} bounds={}x{}+{},{} "
            "damage={}x{}+{},{} detail={}",
            present.frame_id.sequence, present.revision, present.front_slot_index, present.swapchain_generation,
            rects.bounds_width, rects.bounds_height, rects.bounds_x, rects.bounds_y, rects.damage_width,
            rects.damage_height, rects.damage_x, rects.damage_y, present_result.message("ok"));
    }
    app_.record_workspace_surface_bridge_error({});
    return true;
}

void EmbeddedCefBrowserRuntime::Impl::flush_pending_messages() {
    CEF_REQUIRE_UI_THREAD();
    if (!page_callbacks_ready_ || browser_ == nullptr) {
        return;
    }

    runtime_shared::mark_snapshot_delivery_revision(bridge_state_, snapshot_dirty_, current_snapshot_json_,
                                                    current_snapshot_revision_, bridge_state_dirty_);
    const std::optional<runtime_shared::PendingBridgeDispatch> dispatch = runtime_shared::build_pending_bridge_dispatch(
        bridge_state_, bridge_state_dirty_, runtime_capability_gate_passed_, snapshot_dirty_, current_snapshot_json_,
        error_dirty_, pending_error_text_, delivered_bridge_state_json_);
    if (dispatch.has_value()) {
        if (CefRefPtr<CefFrame> main_frame = browser_->GetMainFrame(); main_frame != nullptr) {
            main_frame->ExecuteJavaScript(
                runtime_shared::bridge_dispatch_script(dispatch->send_bridge ? &dispatch->bridge_json : nullptr,
                                                       dispatch->send_snapshot ? &current_snapshot_json_ : nullptr,
                                                       dispatch->send_error ? &pending_error_text_ : nullptr, nullptr),
                main_frame->GetURL(), 0);
        }
        if (dispatch->send_bridge) {
            websocket_server_.publish_bridge_state(dispatch->bridge_json);
        }
        if (dispatch->send_snapshot) {
            websocket_server_.publish_snapshot(current_snapshot_json_);
        }
        if (dispatch->send_error) {
            websocket_server_.publish_error(pending_error_text_);
        }
        runtime_shared::complete_pending_bridge_dispatch(
            *dispatch, current_snapshot_json_, delivered_bridge_state_json_, bridge_state_dirty_,
            delivered_snapshot_json_, snapshot_dirty_, pending_error_text_, error_dirty_);
    }
    sync_workspace_gpu_bridge_publication();
}

void EmbeddedCefBrowserRuntime::Impl::fatal_error(std::string message) {
    CEF_REQUIRE_UI_THREAD();
    exit_code_ = 1;
    runtime_shared::set_bridge_error(bridge_state_, std::move(message), true, bridge_state_dirty_, pending_error_text_,
                                     error_dirty_);
    flush_pending_messages();
    mmltk::logging::logger("gui")->error("{}", bridge_state_.last_error);
    request_quit(true);
}

void EmbeddedCefBrowserRuntime::Impl::handle_bridge_message_text(const std::string& payload) {
    CEF_REQUIRE_UI_THREAD();
    try {
        if (payload.empty()) {
            return;
        }

        const nlohmann::json message = nlohmann::json::parse(payload);
        const auto verify_runtime_capabilities_or_abort = [this] {
            runtime_shared::verify_runtime_capabilities_or_abort(bridge_state_.runtime_capabilities,
                                                                 [this](const std::string& m) { fatal_error(m); });
        };
        const auto maybe_complete_runtime_capability_gate = [this, &verify_runtime_capabilities_or_abort] {
            runtime_shared::maybe_complete_runtime_capability_gate(
                page_callbacks_ready_, runtime_capabilities_reported_, runtime_capability_gate_passed_, quit_requested_,
                runtime_capability_deadline_, bridge_state_, verify_runtime_capabilities_or_abort,
                [this] { runtime_shared::queue_bridge_state(bridge_state_dirty_); }, [this] { refresh_snapshot(); },
                [this] { flush_pending_messages(); });
        };
        runtime_shared::dispatch_bridge_message(
            message,
            [this, &maybe_complete_runtime_capability_gate] {
                const bool callbacks_ready_before = page_callbacks_ready_;
                runtime_shared::handle_callbacks_ready_message(
                    page_callbacks_ready_, bridge_state_,
                    [this](const mmltk::browser::BrowserBridgePhase phase) {
                        runtime_shared::update_bridge_phase(bridge_state_, phase, bridge_state_dirty_);
                    },
                    [this] {
                        runtime_shared::arm_runtime_capability_probe_deadline(runtime_capability_deadline_,
                                                                              kRuntimeCapabilityProbeTimeout);
                    },
                    [this] { runtime_shared::queue_bridge_state(bridge_state_dirty_); },
                    [this] { flush_pending_messages(); }, maybe_complete_runtime_capability_gate);
                if (!callbacks_ready_before && page_callbacks_ready_) {
                    mmltk::logging::logger("gui")->info(
                        "embedded browser runtime startup checkpoint: page callbacks ready");
                    if (startup_cef_initialized_ && startup_browser_child_created_ &&
                        startup_main_frame_load_started_ && startup_first_browser_visible_) {
                        startup_checkpoint_deadline_.reset();
                    }
                }
            },
            [this, &verify_runtime_capabilities_or_abort,
             &maybe_complete_runtime_capability_gate](const nlohmann::json& runtime_capabilities_message) {
                runtime_shared::handle_runtime_capabilities_message(
                    runtime_capabilities_message, runtime_capabilities_reported_, runtime_capability_gate_passed_,
                    quit_requested_, reported_runtime_capabilities_, bridge_state_, [this] { refresh_snapshot(); },
                    [this] {
                        runtime_shared::queue_bridge_state(bridge_state_dirty_);
                        flush_pending_messages();
                    },
                    verify_runtime_capabilities_or_abort, maybe_complete_runtime_capability_gate);
            },
            [this] {
                release_all_tracked_workspace_gpu_bridge_publications();
                app_.release_all_workspace_surface_publications();
            },
            [](const nlohmann::json& extra_message) {
                return runtime_shared::is_message_type(extra_message, "host.ime.context");
            },
            [this](const nlohmann::json& intent_message) {
                runtime_shared::handle_intent_message(
                    app_, intent_message, runtime_capability_gate_passed_,
                    [this](std::string message_text, const bool report_to_page) {
                        runtime_shared::set_bridge_error(bridge_state_, std::move(message_text), report_to_page,
                                                         bridge_state_dirty_, pending_error_text_, error_dirty_);
                    },
                    [this](const mmltk::browser::BrowserBridgePhase phase) {
                        runtime_shared::update_bridge_phase(bridge_state_, phase, bridge_state_dirty_);
                    },
                    dispatch_pending_idle_);
            });
    } catch (const std::exception& error) {
        runtime_shared::set_bridge_error(bridge_state_, error.what(), true, bridge_state_dirty_, pending_error_text_,
                                         error_dirty_);
        flush_pending_messages();
        mmltk::logging::logger("gui")->error("embedded browser runtime bridge error: {}", error.what());
    }
}

void EmbeddedCefBrowserRuntime::Impl::sync_workspace_gpu_bridge_publication() {
    CEF_REQUIRE_UI_THREAD();
    if (!page_callbacks_ready_ || browser_ == nullptr) {
        return;
    }
    if (sync_workspace_native_presented_swapchain()) {
        return;
    }

    std::optional<WorkspaceGpuBridgeSurfacePublication> publication;
    bool had_workspace_surface_source = false;
    if (const std::optional<mmltk::browser::WorkspaceSurfaceInfo> surface_info =
            app_.acquire_workspace_surface(kBrowserWorkspaceSurfaceId);
        surface_info.has_value()) {
        if (workspace_gpu_bridge_state_synced_ && delivered_workspace_gpu_bridge_publication_.has_value() &&
            delivered_workspace_gpu_bridge_publication_->surface_info == *surface_info) {
            publication = delivered_workspace_gpu_bridge_publication_;
            had_workspace_surface_source = true;
        } else {
            if (const std::optional<RetainedBrowserImportedFrameSource> source =
                    app_.acquire_workspace_surface_source(surface_info->surface_id, surface_info->revision);
                source.has_value()) {
                had_workspace_surface_source = true;
                const std::uint64_t export_start_ns = mmltk::live::live_steady_clock_now_ns();
                publication = make_workspace_gpu_bridge_surface_publication(*surface_info, *source);
                app_.record_workspace_surface_pipeline_stage(mmltk::live::LivePipelineStage::CefExportSharedImage,
                                                             surface_info->surface_id, surface_info->revision,
                                                             export_start_ns);
            }
        }
    }

    if (publication.has_value() && !publication->has_live_exported_shared_image()) {
        app_.record_workspace_surface_bridge_error(workspace_gpu_bridge_shared_image_export_rejected_message(
            publication->export_error.empty()
                ? std::string_view("SharedImage export rejected the current live surface")
                : std::string_view(publication->export_error.data(), publication->export_error.size())));
    } else if (publication.has_value() && publication->has_live_exported_shared_image()) {
        app_.record_workspace_surface_bridge_error({});
    } else if (had_workspace_surface_source) {
        app_.record_workspace_surface_bridge_error(
            "workspace GPU bridge rejected the current live surface publication");
    }

    if (workspace_gpu_bridge_state_synced_ && delivered_workspace_gpu_bridge_publication_ == publication) {
        return;
    }

    send_workspace_gpu_bridge_publication(browser_, publication);
    delivered_workspace_gpu_bridge_publication_ = publication;
    workspace_gpu_bridge_state_synced_ = true;
    if (publication.has_value()) {
        track_workspace_gpu_bridge_publication(*publication);
        const std::string surface_ready_json =
            nlohmann::json({{"type", "surface.ready"}, {"surface", publication->surface_info}}).dump();
        if (browser_ != nullptr) {
            if (CefRefPtr<CefFrame> main_frame = browser_->GetMainFrame(); main_frame != nullptr) {
                main_frame->ExecuteJavaScript(
                    runtime_shared::bridge_dispatch_script(nullptr, nullptr, nullptr, &surface_ready_json),
                    main_frame->GetURL(), 0);
            }
        }
        websocket_server_.publish_surface_ready(surface_ready_json);
        surface_ready_log_count_ += 1U;
        if (surface_ready_log_count_ <= 3U || surface_ready_log_count_ % 300U == 0U) {
            mmltk::logging::logger("gui")->info("surface.ready revision {} ({}x{})", publication->surface_info.revision,
                                                publication->surface_info.width, publication->surface_info.height);
        }
    }
}

void EmbeddedCefBrowserRuntime::Impl::on_workspace_gpu_bridge_release(const CefRefPtr<CefBrowser>& browser,
                                                                      const std::string_view surface_id,
                                                                      const std::string_view revision) {
    CEF_REQUIRE_UI_THREAD();
    if (!is_main_browser(browser) || surface_id.empty() || revision.empty()) {
        return;
    }
    if (!app_.release_workspace_surface(surface_id, revision)) {
        return;
    }
    release_tracked_workspace_gpu_bridge_publication(surface_id, revision);
    surface_release_log_count_ += 1U;
    if (surface_release_log_count_ <= 3U || surface_release_log_count_ % 300U == 0U) {
        mmltk::logging::logger("gui")->info("surface.release {} revision {}", surface_id, revision);
    }
    refresh_snapshot();
    flush_pending_messages();
}

namespace {

[[nodiscard]] std::string_view cef_log_severity_name(const cef_log_severity_t level) noexcept {
    switch (level) {
        case LOGSEVERITY_DEFAULT:
            return "default";
        case LOGSEVERITY_VERBOSE:
            return "verbose";
        case LOGSEVERITY_INFO:
            return "info";
        case LOGSEVERITY_WARNING:
            return "warning";
        case LOGSEVERITY_ERROR:
            return "error";
        case LOGSEVERITY_FATAL:
            return "fatal";
        case LOGSEVERITY_DISABLE:
            return "disabled";
    }
    return "unknown";
}

[[nodiscard]] bool should_log_cef_console_message(const cef_log_severity_t level,
                                                  const std::string_view message) noexcept {
    if (level >= LOGSEVERITY_WARNING) {
        return true;
    }
    return message.find("[mmltk.webgpu]") != std::string_view::npos ||
           message.find("WebGPU") != std::string_view::npos || message.find("webgpu") != std::string_view::npos ||
           message.find("workspace GPU bridge") != std::string_view::npos;
}

void CefRuntimeClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr) {
        runtime_->on_title_change(browser, title.ToString());
    }
}

void CefRuntimeClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr) {
        runtime_->on_browser_created(browser);
    }
}

bool CefRuntimeClient::DoClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    return runtime_ != nullptr ? runtime_->on_browser_do_close(browser) : false;
}

void CefRuntimeClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr) {
        runtime_->on_before_close(browser);
    }
}

void CefRuntimeClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr && frame != nullptr && frame->IsMain()) {
        runtime_->on_load_start(browser);
    }
}

void CefRuntimeClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr && frame != nullptr && frame->IsMain()) {
        runtime_->on_load_end(browser, frame->GetURL().ToString());
    }
}

void CefRuntimeClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                                   const CefString& errorText, const CefString& failedUrl) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ != nullptr && frame != nullptr && frame->IsMain()) {
        runtime_->on_load_error(browser, errorText.ToString(), failedUrl.ToString(), static_cast<int>(errorCode));
    }
}

bool CefRuntimeClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                                        const CefString& message, const CefString& source, int line) {
    CEF_REQUIRE_UI_THREAD();
    const std::string message_text = message.ToString();
    if (!should_log_cef_console_message(level, message_text)) {
        return false;
    }
    const int browser_id = browser != nullptr ? browser->GetIdentifier() : -1;
    const std::string source_text = source.ToString();
    const auto logger = mmltk::logging::logger("gui");
    const std::string_view severity = cef_log_severity_name(level);
    if (level >= LOGSEVERITY_ERROR) {
        logger->error("CEF console [{}] browser={} source={} line={} message={}", severity, browser_id, source_text,
                      line, message_text);
    } else if (level >= LOGSEVERITY_WARNING) {
        logger->warn("CEF console [{}] browser={} source={} line={} message={}", severity, browser_id, source_text,
                     line, message_text);
    } else {
        logger->info("CEF console [{}] browser={} source={} line={} message={}", severity, browser_id, source_text,
                     line, message_text);
    }
    return false;
}

bool CefRuntimeClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefProcessId,
                                                CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ == nullptr || message == nullptr) {
        return false;
    }
    const std::string name = message->GetName().ToString();
    if (name == kRendererBridgeMessageName) {
        runtime_->on_renderer_message(browser, message->GetArgumentList()->GetString(0U).ToString());
        return true;
    }
    if (name == kRendererBridgeErrorMessageName) {
        runtime_->on_renderer_error(browser, message->GetArgumentList()->GetString(0U).ToString());
        return true;
    }
    if (name == kRendererWorkspaceGpuBridgeReleaseMessageName) {
        runtime_->on_workspace_gpu_bridge_release(browser, message->GetArgumentList()->GetString(0U).ToString(),
                                                  message->GetArgumentList()->GetString(1U).ToString());
        return true;
    }
    return false;
}

CefRefPtr<CefResourceHandler> AppSchemeHandlerFactory::Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                                              const CefString&, CefRefPtr<CefRequest> request) {
    CEF_REQUIRE_IO_THREAD();
    const std::string url = request->GetURL().ToString();
    const std::string prefix = app_origin();
    if (!url.starts_with(prefix)) {
        return nullptr;
    }
    std::string request_path = url.substr(prefix.size());
    if (request_path.empty()) {
        request_path = "/";
    }
    try {
        const std::filesystem::path resolved = runtime_->resolve_asset_request_path(request_path);
        std::shared_ptr<const std::vector<std::uint8_t>> bytes = runtime_->load_asset_bytes(resolved);
        CefRefPtr<AssetBytesOwner> owner = new AssetBytesOwner(bytes);
        CefRefPtr<CefReadHandler> handler = new CefByteReadHandler(bytes->data(), bytes->size(), owner);
        CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForHandler(handler);
        if (stream == nullptr) {
            return nullptr;
        }
        return new CefStreamResourceHandler(mime_type_for_path(resolved), stream);
    } catch (...) {
        return nullptr;
    }
}

void CefApplication::OnContextInitialized() {
    CEF_REQUIRE_UI_THREAD();
    if (runtime_ == nullptr) {
        return;
    }
    try {
        runtime_->on_context_initialized();
    } catch (const std::exception& error) {
        runtime_->fail_startup(std::string("embedded CEF runtime initialization failed: ") + error.what());
    }
}

bool CefApplication::OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                                 const CefString& current_directory) {
    CEF_REQUIRE_UI_THREAD();
    return runtime_ != nullptr ? runtime_->on_already_running_app_relaunch(command_line, current_directory) : false;
}

}  // namespace

EmbeddedCefBrowserRuntime::EmbeddedCefBrowserRuntime(App& app, const AppLaunchOptions& options)
    : impl_(std::make_unique<Impl>(app, options)) {}

EmbeddedCefBrowserRuntime::~EmbeddedCefBrowserRuntime() = default;

void EmbeddedCefBrowserRuntime::attach_shell(CefBrowserShell& shell) {
    impl_->attach_shell(shell);
}

bool EmbeddedCefBrowserRuntime::initialize(int argc, char** argv) {
    return impl_->initialize(argc, argv);
}

void EmbeddedCefBrowserRuntime::run_message_loop() {
    impl_->run_message_loop();
}

void EmbeddedCefBrowserRuntime::shutdown() {
    impl_->shutdown();
}

int EmbeddedCefBrowserRuntime::exit_code() const noexcept {
    return impl_->exit_code();
}

int EmbeddedCefBrowserRuntime::main_window_width() const noexcept {
    return impl_->main_window_width();
}

int EmbeddedCefBrowserRuntime::main_window_height() const noexcept {
    return impl_->main_window_height();
}

int EmbeddedCefBrowserRuntime::popup_window_width(const bool is_devtools) const noexcept {
    return impl_->popup_window_width(is_devtools);
}

int EmbeddedCefBrowserRuntime::popup_window_height(const bool is_devtools) const noexcept {
    return impl_->popup_window_height(is_devtools);
}

std::string_view EmbeddedCefBrowserRuntime::window_title() const noexcept {
    return impl_->window_title();
}

CefRefPtr<CefBrowserView> EmbeddedCefBrowserRuntime::create_browser_view(
    const CefRefPtr<CefBrowserViewDelegate>& delegate) {
    return impl_->create_browser_view(delegate);
}

void EmbeddedCefBrowserRuntime::on_window_created(const CefRefPtr<CefWindow>& window, const bool is_popup) {
    impl_->on_window_created(window, is_popup);
}

void EmbeddedCefBrowserRuntime::on_window_destroyed(const CefRefPtr<CefWindow>& window, const bool is_popup) {
    impl_->on_window_destroyed(window, is_popup);
}

bool EmbeddedCefBrowserRuntime::on_window_can_close(const CefRefPtr<CefBrowserView>& browser_view) const {
    return impl_->on_window_can_close(browser_view);
}

CefRefPtr<CefBrowser> EmbeddedCefBrowserRuntime::browser() {
    return impl_->browser();
}

int run_cef_browser_runtime(App& app, const AppLaunchOptions& options, int argc, char** argv) {
    return CefAppRunner(app, options).run(argc, argv);
}

}  // namespace mmltk::gui
