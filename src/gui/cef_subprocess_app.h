#pragma once

#include "gui/cef_workspace_gpu_bridge.h"

#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_scheme.h"
#include "include/cef_v8.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mmltk::gui {

inline constexpr std::string_view kAppScheme = "app";
inline constexpr std::string_view kAppHost = "mmltk.invalid";

enum class CefWebGpuRuntime : std::uint8_t {
    Auto = 0,
    Vulkan = 1,
    OpenGles = 2,
};

struct CefRuntimeLaunchConfig {
    CefWebGpuRuntime webgpu_runtime = CefWebGpuRuntime::Auto;
    bool enable_unsafe_webgpu = true;
    bool force_high_performance_gpu = false;
};

[[nodiscard]] const CefRuntimeLaunchConfig& cef_runtime_launch_config();

[[nodiscard]] std::string_view cef_webgpu_runtime_name(CefWebGpuRuntime runtime) noexcept;

inline constexpr std::string_view kRendererBridgeMessageName = "mmltk.bridge.message";
inline constexpr std::string_view kRendererBridgeErrorMessageName = "mmltk.bridge.error";
inline constexpr std::string_view kRendererBridgeExtensionName = "mmltk/native_bridge";

[[nodiscard]] std::string_view renderer_bridge_extension_code();

[[nodiscard]] const std::string& ready_watcher_script();

void apply_cef_runtime_command_line_configuration(const CefRefPtr<CefCommandLine>& command_line,
                                                  std::string_view process_type = {});

void register_app_custom_scheme(CefRawPtr<CefSchemeRegistrar> registrar);

void register_renderer_bridge_extension();

void install_renderer_bridge_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                                        const CefRefPtr<CefV8Context>& context);

void install_ready_watcher_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                                      const CefRefPtr<CefV8Context>& context);

inline void install_renderer_workspace_context(CefWorkspaceGpuBridge& bridge, const CefRefPtr<CefBrowser>& browser,
                                               const CefRefPtr<CefFrame>& frame,
                                               const CefRefPtr<CefV8Context>& context) {
    install_renderer_bridge_in_context(browser, frame, context);
    bridge.install_in_context(browser, frame, context);
}

inline void release_renderer_workspace_context(CefWorkspaceGpuBridge& bridge, const CefRefPtr<CefBrowser>& browser,
                                               const CefRefPtr<CefFrame>& frame,
                                               const CefRefPtr<CefV8Context>& context) {
    (void)context;
    if (browser != nullptr && frame != nullptr && frame->IsMain()) {
        bridge.on_browser_destroyed(browser->GetIdentifier());
    }
}

inline bool dispatch_renderer_workspace_message(CefWorkspaceGpuBridge& bridge, const CefRefPtr<CefBrowser>& browser,
                                                const CefRefPtr<CefFrame>& frame, CefProcessId source_process,
                                                const CefRefPtr<CefProcessMessage>& message) {
    return bridge.on_process_message_received(browser, frame, source_process, message);
}

template <typename Derived>
class CefRenderProcessApplication : public CefApp, public CefRenderProcessHandler {
   public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return static_cast<Derived*>(this);
    }

    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        register_app_custom_scheme(registrar);
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        apply_cef_runtime_command_line_configuration(command_line, process_type.ToString());
    }
};

class BrowserSubprocessApp : public CefRenderProcessApplication<BrowserSubprocessApp> {
   public:
    BrowserSubprocessApp() = default;

    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        install_renderer_workspace_context(workspace_gpu_bridge_, browser, frame, context);
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override {
        release_renderer_workspace_context(workspace_gpu_bridge_, browser, frame, context);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
        return dispatch_renderer_workspace_message(workspace_gpu_bridge_, browser, frame, source_process, message);
    }

   private:
    CefWorkspaceGpuBridge workspace_gpu_bridge_;

    IMPLEMENT_REFCOUNTING(BrowserSubprocessApp);
};

}  // namespace mmltk::gui
