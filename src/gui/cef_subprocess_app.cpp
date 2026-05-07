#include "gui/cef_subprocess_app.h"

#include "gui/browser_runtime_shared.h"

#include "include/cef_process_message.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace mmltk::gui {

namespace {

#ifdef MMLTK_CEF_WEBGPU_RUNTIME
constexpr std::string_view kConfiguredCefWebGpuRuntime = MMLTK_CEF_WEBGPU_RUNTIME;
#else
constexpr std::string_view kConfiguredCefWebGpuRuntime = "auto";
#endif
#ifdef MMLTK_CEF_ENABLE_UNSAFE_WEBGPU
constexpr bool kConfiguredCefEnableUnsafeWebGpu = MMLTK_CEF_ENABLE_UNSAFE_WEBGPU != 0;
#else
constexpr bool kConfiguredCefEnableUnsafeWebGpu = false;
#endif
#ifdef MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU
constexpr bool kConfiguredCefForceHighPerformanceGpu = MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU != 0;
#else
constexpr bool kConfiguredCefForceHighPerformanceGpu = false;
#endif

constexpr std::string_view kRendererBridgeExtensionCode = R"JS(
(function() {
  native function SendMessage();
  const root =
    typeof globalThis === "object" && globalThis !== null ? globalThis : this;
  const postMessage = function postMessage(messageText) {
    SendMessage(String(messageText));
  };
  const existingBridge =
    typeof root.__MMLTK_NATIVE_BRIDGE__ === "object" &&
      root.__MMLTK_NATIVE_BRIDGE__ !== null
      ? root.__MMLTK_NATIVE_BRIDGE__
      : {};
  Object.defineProperty(existingBridge, "postMessage", {
    configurable: true,
    enumerable: false,
    writable: false,
    value: postMessage,
  });
  Object.defineProperty(root, "__MMLTK_NATIVE_BRIDGE__", {
    configurable: true,
    enumerable: false,
    writable: true,
    value: existingBridge,
  });
})();
)JS";
constexpr std::string_view kRendererBridgeGlobalName = "__MMLTK_NATIVE_BRIDGE__";

[[nodiscard]] std::string ascii_lowercase_copy(std::string_view value) {
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower;
}

[[nodiscard]] std::optional<std::string> read_non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] std::string read_non_empty_env_or(const char* name, const std::string_view fallback) {
    if (auto value = read_non_empty_env(name); value.has_value()) {
        return *value;
    }
    return std::string(fallback);
}

[[nodiscard]] bool parse_bool_text(std::string_view value, bool fallback) {
    const std::string lowered = ascii_lowercase_copy(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return fallback;
}

[[nodiscard]] CefWebGpuRuntime parse_cef_webgpu_runtime(std::string_view value) {
    const std::string lowered = ascii_lowercase_copy(value);
    if (lowered == "vulkan" || lowered == "linux_vulkan") {
        return CefWebGpuRuntime::Vulkan;
    }
    if (lowered == "opengles" || lowered == "gles" || lowered == "linux_opengles" || lowered == "compat") {
        return CefWebGpuRuntime::OpenGles;
    }
    if (lowered == "auto" || lowered == "default" || lowered == "system") {
        return CefWebGpuRuntime::Auto;
    }
    return CefWebGpuRuntime::Auto;
}

[[nodiscard]] CefRuntimeLaunchConfig resolve_cef_runtime_launch_config() {
    CefRuntimeLaunchConfig config;
    config.enable_unsafe_webgpu = kConfiguredCefEnableUnsafeWebGpu;
    config.force_high_performance_gpu = kConfiguredCefForceHighPerformanceGpu;
    if (auto runtime = read_non_empty_env("MMLTK_CEF_WEBGPU_RUNTIME"); runtime.has_value()) {
        config.webgpu_runtime = parse_cef_webgpu_runtime(*runtime);
    } else {
        config.webgpu_runtime = parse_cef_webgpu_runtime(kConfiguredCefWebGpuRuntime);
    }
    if (auto enable_unsafe_webgpu = read_non_empty_env("MMLTK_CEF_ENABLE_UNSAFE_WEBGPU");
        enable_unsafe_webgpu.has_value()) {
        config.enable_unsafe_webgpu = parse_bool_text(*enable_unsafe_webgpu, kConfiguredCefEnableUnsafeWebGpu);
    }
    if (auto force_high_performance_gpu = read_non_empty_env("MMLTK_CEF_FORCE_HIGH_PERFORMANCE_GPU");
        force_high_performance_gpu.has_value()) {
        config.force_high_performance_gpu =
            parse_bool_text(*force_high_performance_gpu, kConfiguredCefForceHighPerformanceGpu);
    }
    return config;
}

[[nodiscard]] std::vector<std::string> split_csv_values(std::string_view text) {
    std::vector<std::string> values;
    std::size_t start = 0U;
    while (start < text.size()) {
        std::size_t end = text.find(',', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string value(text.substr(start, end - start));
        if (!value.empty()) {
            values.push_back(std::move(value));
        }
        start = end + 1U;
    }
    return values;
}

[[nodiscard]] std::string join_csv_values(const std::vector<std::string>& values) {
    std::string joined;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            joined += ",";
        }
        joined += values[index];
    }
    return joined;
}

void append_switch_if_absent(const CefRefPtr<CefCommandLine>& command_line, const char* name) {
    if (command_line == nullptr || command_line->HasSwitch(name)) {
        return;
    }
    command_line->AppendSwitch(name);
}

void append_switch_with_value_if_absent(const CefRefPtr<CefCommandLine>& command_line, const char* name,
                                        std::string_view value) {
    if (command_line == nullptr || value.empty() || command_line->HasSwitch(name)) {
        return;
    }
    command_line->AppendSwitchWithValue(name, std::string(value));
}

void append_path_switch_from_env(const CefRefPtr<CefCommandLine>& command_line, const char* switch_name,
                                 const char* env_name) {
    const char* value = std::getenv(env_name);
    if (value == nullptr || *value == '\0') {
        return;
    }
    append_switch_with_value_if_absent(command_line, switch_name, value);
}

void append_unique_csv_switch_token(const CefRefPtr<CefCommandLine>& command_line, const char* name,
                                    std::string_view token) {
    if (command_line == nullptr || token.empty()) {
        return;
    }
    std::vector<std::string> values = split_csv_values(command_line->GetSwitchValue(name).ToString());
    if (std::find_if(values.begin(), values.end(), [token](const std::string& value) { return value == token; }) !=
        values.end()) {
        return;
    }
    values.emplace_back(token);
    command_line->RemoveSwitch(name);
    command_line->AppendSwitchWithValue(name, join_csv_values(values));
}

void remove_csv_switch_token(const CefRefPtr<CefCommandLine>& command_line, const char* name, std::string_view token) {
    if (command_line == nullptr || token.empty() || !command_line->HasSwitch(name)) {
        return;
    }
    std::vector<std::string> values = split_csv_values(command_line->GetSwitchValue(name).ToString());
    const auto new_end =
        std::remove_if(values.begin(), values.end(), [token](const std::string& value) { return value == token; });
    if (new_end == values.end()) {
        return;
    }
    values.erase(new_end, values.end());
    command_line->RemoveSwitch(name);
    if (!values.empty()) {
        command_line->AppendSwitchWithValue(name, join_csv_values(values));
    }
}

[[nodiscard]] std::string process_type_name(std::string_view process_type) {
    return process_type.empty() ? "browser" : std::string(process_type);
}

[[nodiscard]] std::string switch_value_or_marker(const CefRefPtr<CefCommandLine>& command_line, const char* name) {
    if (command_line == nullptr || !command_line->HasSwitch(name)) {
        return "<unset>";
    }
    const std::string value = command_line->GetSwitchValue(name).ToString();
    return value.empty() ? "<flag>" : value;
}

void log_cef_runtime_command_line_configuration(const CefRefPtr<CefCommandLine>& command_line,
                                                std::string_view process_type) {
    if (command_line == nullptr) {
        return;
    }

    const std::string process_name = process_type_name(process_type);
    const std::string program = command_line->GetProgram().ToString();
    const std::string command_line_text = command_line->GetCommandLineString().ToString();
    const long pid = static_cast<long>(::getpid());

    std::fprintf(stderr,
                 "[mmltk-cef-cmdline] pid=%ld process=%s program=%s "
                 "ozone-platform=%s use-angle=%s render-node-override=%s use-gl=%s "
                 "use-vulkan=%s disable-gpu=%s disable-gpu-compositing=%s "
                 "gtk-version=%s disable-software-rasterizer=%s ignore-gpu-blocklist=%s "
                 "enable-native-gpu-memory-buffers=%s disable-vulkan-fallback-to-gl-for-testing=%s "
                 "enable-unsafe-webgpu=%s enable-features=%s disable-features=%s\n",
                 pid, process_name.c_str(), program.c_str(),
                 switch_value_or_marker(command_line, "ozone-platform").c_str(),
                 switch_value_or_marker(command_line, "use-angle").c_str(),
                 switch_value_or_marker(command_line, "render-node-override").c_str(),
                 switch_value_or_marker(command_line, "use-gl").c_str(),
                 switch_value_or_marker(command_line, "use-vulkan").c_str(),
                 switch_value_or_marker(command_line, "disable-gpu").c_str(),
                 switch_value_or_marker(command_line, "disable-gpu-compositing").c_str(),
                 switch_value_or_marker(command_line, "gtk-version").c_str(),
                 switch_value_or_marker(command_line, "disable-software-rasterizer").c_str(),
                 switch_value_or_marker(command_line, "ignore-gpu-blocklist").c_str(),
                 switch_value_or_marker(command_line, "enable-native-gpu-memory-buffers").c_str(),
                 switch_value_or_marker(command_line, "disable-vulkan-fallback-to-gl-for-testing").c_str(),
                 switch_value_or_marker(command_line, "enable-unsafe-webgpu").c_str(),
                 switch_value_or_marker(command_line, "enable-features").c_str(),
                 switch_value_or_marker(command_line, "disable-features").c_str());
    std::fprintf(stderr, "[mmltk-cef-cmdline-argv] pid=%ld process=%s argv=%s\n", pid, process_name.c_str(),
                 command_line_text.c_str());
    std::fflush(stderr);
}

[[nodiscard]] std::string cef_ozone_platform() {
    if (auto platform = read_non_empty_env("MMLTK_CEF_OZONE_PLATFORM"); platform.has_value()) {
        const std::string lowered = ascii_lowercase_copy(*platform);
        if (lowered == "wayland") {
            return lowered;
        }
        std::fprintf(stderr,
                     "[mmltk-cef] ignoring unsupported ozone platform `%s`; "
                     "Wayland is required\n",
                     platform->c_str());
    }
    return "wayland";
}

[[nodiscard]] bool cef_verbose_logging_enabled() {
    if (auto value = read_non_empty_env("MMLTK_CEF_VERBOSE_LOGGING"); value.has_value()) {
        return parse_bool_text(*value, false);
    }
    return false;
}

void apply_cef_webgpu_runtime_configuration(const CefRefPtr<CefCommandLine>& command_line,
                                            const CefRuntimeLaunchConfig& config) {
    append_switch_if_absent(command_line, "disable-software-rasterizer");
    append_switch_if_absent(command_line, "enable-gpu-rasterization");
    append_switch_if_absent(command_line, "enable-zero-copy");

    switch (config.webgpu_runtime) {
        case CefWebGpuRuntime::Vulkan:
            command_line->RemoveSwitch("use-angle");
            command_line->RemoveSwitch("use-gl");
            command_line->RemoveSwitch("use-vulkan");
            remove_csv_switch_token(command_line, "disable-features", "Vulkan");
            remove_csv_switch_token(command_line, "disable-features", "ForceEnableWebGpuInterop");
            append_switch_if_absent(command_line, "ignore-gpu-blocklist");
            append_switch_if_absent(command_line, "enable-native-gpu-memory-buffers");
            command_line->AppendSwitchWithValue("use-vulkan", "native");
            append_unique_csv_switch_token(command_line, "enable-features", "ForceEnableWebGpuInterop");
            return;
        case CefWebGpuRuntime::Auto:
            return;
        case CefWebGpuRuntime::OpenGles:
            command_line->RemoveSwitch("use-angle");
            command_line->RemoveSwitch("use-gl");
            command_line->RemoveSwitch("use-vulkan");
            command_line->RemoveSwitch("ignore-gpu-blocklist");
            append_unique_csv_switch_token(command_line, "disable-features", "Vulkan");
            append_unique_csv_switch_token(command_line, "disable-features", "DefaultANGLEVulkan");
            append_unique_csv_switch_token(command_line, "disable-features", "VulkanFromANGLE");
            append_unique_csv_switch_token(command_line, "disable-features", "ForceEnableWebGpuInterop");
            command_line->AppendSwitchWithValue("use-gl", "angle");
            command_line->AppendSwitchWithValue("use-angle", "gles");
            return;
    }
}

class RendererBridgeHandler : public CefV8Handler {
   public:
    RendererBridgeHandler() = default;

    bool Execute(const CefString& name, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval, CefString& exception) override {
        CEF_REQUIRE_RENDERER_THREAD();
        if (name != "SendMessage" && name != "postMessage") {
            return false;
        }
        if (arguments.size() != 1U || !arguments[0]->IsString()) {
            exception =
                "__MMLTK_NATIVE_BRIDGE__.postMessage requires a single string "
                "argument";
            retval = CefV8Value::CreateBool(false);
            return true;
        }
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        if (context == nullptr || !context->IsValid()) {
            exception = "renderer bridge missing V8 context";
            retval = CefV8Value::CreateBool(false);
            return true;
        }
        CefRefPtr<CefBrowser> browser = context->GetBrowser();
        if (browser == nullptr) {
            exception = "renderer bridge missing browser";
            retval = CefV8Value::CreateBool(false);
            return true;
        }
        CefRefPtr<CefFrame> main_frame = browser->GetMainFrame();
        if (main_frame == nullptr || !main_frame->IsValid()) {
            exception = "renderer bridge missing main frame";
            retval = CefV8Value::CreateBool(false);
            return true;
        }
        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kRendererBridgeMessageName.data());
        message->GetArgumentList()->SetString(0U, arguments[0]->GetStringValue());
        main_frame->SendProcessMessage(PID_BROWSER, message);
        retval = CefV8Value::CreateBool(true);
        return true;
    }

   private:
    IMPLEMENT_REFCOUNTING(RendererBridgeHandler);
};

}  // namespace

const CefRuntimeLaunchConfig& cef_runtime_launch_config() {
    static const CefRuntimeLaunchConfig config = resolve_cef_runtime_launch_config();
    return config;
}

std::string_view cef_webgpu_runtime_name(CefWebGpuRuntime runtime) noexcept {
    switch (runtime) {
        case CefWebGpuRuntime::Auto:
            return "auto";
        case CefWebGpuRuntime::Vulkan:
            return "vulkan";
        case CefWebGpuRuntime::OpenGles:
            return "opengles";
    }
    return "unknown";
}

std::string_view renderer_bridge_extension_code() {
    return kRendererBridgeExtensionCode;
}

const std::string& ready_watcher_script() {
    static const std::string script = browser_runtime_shared::make_ready_watcher_script({
        "void 0",
        false,
        true,
    });
    return script;
}

void apply_cef_runtime_command_line_configuration(const CefRefPtr<CefCommandLine>& command_line,
                                                  std::string_view process_type) {
    if (command_line == nullptr) {
        return;
    }

    command_line->RemoveSwitch("ozone-platform");
    command_line->AppendSwitchWithValue("ozone-platform", cef_ozone_platform());
    command_line->RemoveSwitch("ui-toolkit");
    append_switch_with_value_if_absent(command_line, "gtk-version", "3");
    append_switch_if_absent(command_line, "disable-gtk-ime");
    append_switch_if_absent(command_line, "disable-renderer-accessibility");
    append_switch_with_value_if_absent(command_line, "autoplay-policy", "no-user-gesture-required");
    append_switch_if_absent(command_line, "no-sandbox");
    append_switch_if_absent(command_line, "test-type");
    append_path_switch_from_env(command_line, "resources-dir-path", "MMLTK_CEF_RESOURCES_DIR");
    append_path_switch_from_env(command_line, "locales-dir-path", "MMLTK_CEF_LOCALES_DIR");
    append_path_switch_from_env(command_line, "log-file", "MMLTK_CEF_LOG_FILE");
    if (process_type.empty()) {
        if (auto remote_debugging_port = read_non_empty_env("MMLTK_CEF_REMOTE_DEBUGGING_PORT");
            remote_debugging_port.has_value()) {
            append_switch_with_value_if_absent(command_line, "remote-debugging-port", *remote_debugging_port);
        }
    }
    append_switch_with_value_if_absent(command_line, "log-severity",
                                       cef_verbose_logging_enabled() ? "info" : "warning");

    append_switch_if_absent(command_line, "disable-background-networking");
    append_switch_if_absent(command_line, "disable-sync");
    append_switch_if_absent(command_line, "disable-component-update");
    append_switch_if_absent(command_line, "disable-component-extensions-with-background-pages");
    append_switch_if_absent(command_line, "disable-default-apps");
    append_switch_if_absent(command_line, "disable-extensions");
    append_switch_if_absent(command_line, "no-first-run");
    append_switch_if_absent(command_line, "no-default-browser-check");
    append_switch_if_absent(command_line, "disable-breakpad");
    append_switch_if_absent(command_line, "disable-domain-reliability");
    append_switch_if_absent(command_line, "disable-client-side-phishing-detection");
    append_switch_if_absent(command_line, "metrics-recording-only");
    append_switch_if_absent(command_line, "no-service-autorun");
    append_switch_if_absent(command_line, "disable-search-engine-choice-screen");
    append_switch_if_absent(command_line, "disable-hang-monitor");
    append_switch_if_absent(command_line, "disable-prompt-on-repost");
    append_switch_if_absent(command_line, "disable-popup-blocking");
    append_switch_if_absent(command_line, "disable-ipc-flooding-protection");
    append_switch_if_absent(command_line, "disable-renderer-backgrounding");
    append_switch_if_absent(command_line, "disable-background-timer-throttling");
    append_switch_if_absent(command_line, "disable-backgrounding-occluded-windows");
    append_switch_if_absent(command_line, "disable-dev-shm-usage");
    append_switch_with_value_if_absent(command_line, "password-store", "basic");
    append_switch_if_absent(command_line, "use-mock-keychain");

    append_switch_if_absent(command_line, "no-pings");
    append_switch_if_absent(command_line, "no-experiments");
    append_switch_if_absent(command_line, "disable-variations-seed-fetch");
    append_switch_if_absent(command_line, "disable-field-trial-config");

    for (std::string_view feature : std::array<std::string_view, 54>{
             "GwpAsanMalloc",
             "GwpAsanPartitionAlloc",
             "WebBluetooth",
             "HardwareMediaKeyHandling",
             "Translate",
             "MediaRouter",
             "AcceptCHFrame",
             "InterestFeedContentSuggestions",
             "GlobalMediaControls",
             "DialMediaRouteProvider",
             "BackForwardCache",
             "HeavyAdPrivacyMitigations",
             "NetworkTimeServiceQuerying",
             "OptimizationHints",
             "OptimizationTargetPrediction",
             "OptimizationGuidePageContentExtraction",
             "OptimizationHintsFetchingSRP",
             "OptimizationGuideMetadataValidation",
             "OptimizationGuideOnDeviceModel",
             "OptimizationGuideProactivePersonalizedHintsFetching",
             "OptimizationGuideIconView",
             "TextSafetyClassifier",
             "ModelQualityLogging",
             "AnnotatedPageContentWithActionableElements",
             "SegmentationPlatform",
             "SegmentationPlatformUkmEngine",
             "SegmentationPlatformAdaptiveToolbarV2",
             "SegmentationPlatformLowEngagement",
             "SegmentationPlatformFeedSegment",
             "ResumeHeavyUserSegment",
             "SegmentationPlatformPowerUser",
             "FrequentFeatureUserSegment",
             "ContextualPageActions",
             "ContextualPageActionTabGrouping",
             "SegmentationPlatformSearchUser",
             "SegmentationPlatformDeviceSwitcher",
             "ShoppingUserSegment",
             "SegmentationDefaultReportingSegments",
             "SegmentationPlatformDeviceTier",
             "SegmentationPlatformCrossDeviceUser",
             "SegmentationPlatformIntentionalUser",
             "SegmentationPlatformPasswordManagerUser",
             "SegmentationPlatformTabResumptionRanker",
             "SegmentationPlatformTimeDelaySampling",
             "SegmentationPlatformSignalDbCache",
             "SegmentationPlatformUmaFromSqlDb",
             "SegmentationPlatformURLVisitResumptionRanker",
             "SegmentationPlatformEphemeralBottomRank",
             "SegmentationPlatformEphemeralCardRanker",
             "SegmentationPlatformComposePromotion",
             "SegmentationPlatformFedCmUser",
             "SegmentationPlatformModelExecutionSampling",
             "PrivacySandboxSettings4",
             "EnforcePrivacySandboxAttestations",
         }) {
        append_unique_csv_switch_token(command_line, "disable-features", feature);
    }

    const CefRuntimeLaunchConfig& config = cef_runtime_launch_config();
    apply_cef_webgpu_runtime_configuration(command_line, config);
    if (config.enable_unsafe_webgpu) {
        append_switch_if_absent(command_line, "enable-unsafe-webgpu");
    }
    if (config.force_high_performance_gpu) {
        append_switch_if_absent(command_line, "force_high_performance_gpu");
        append_switch_with_value_if_absent(command_line, "use-webgpu-power-preference", "default-high-performance");
    }
    if (cef_verbose_logging_enabled()) {
        append_switch_with_value_if_absent(command_line, "enable-logging", "stderr");
        append_switch_with_value_if_absent(command_line, "v", read_non_empty_env_or("MMLTK_CEF_LOG_VERBOSITY", "0"));
        append_switch_with_value_if_absent(command_line, "vmodule",
                                           read_non_empty_env_or("MMLTK_CEF_VMODULE",
                                                                 "workspace_gpu_bridge*=4,"
                                                                 "webgpu*=3,"
                                                                 "gpu_device*=3,"
                                                                 "dawn*=2,"
                                                                 "frame_sink_video_capturer*=3,"
                                                                 "video_capture*=3,"
                                                                 "gpu_memory_buffer*=3,"
                                                                 "native_pixmap*=3,"
                                                                 "gbm_buffer*=3,"
                                                                 "skia_output_surface*=3,"
                                                                 "copy_output_request*=3,"
                                                                 "host_frame_sink*=2"));
    }

    log_cef_runtime_command_line_configuration(command_line, process_type);
}

void register_app_custom_scheme(CefRawPtr<CefSchemeRegistrar> registrar) {
    if (registrar == nullptr) {
        return;
    }
    registrar->AddCustomScheme(kAppScheme.data(), CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                                      CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void register_renderer_bridge_extension() {
    CEF_REQUIRE_RENDERER_THREAD();
    CefRegisterExtension(kRendererBridgeExtensionName.data(), kRendererBridgeExtensionCode.data(),
                         new RendererBridgeHandler());
}

void install_renderer_bridge_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                                        const CefRefPtr<CefV8Context>& context) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (browser == nullptr || frame == nullptr || !frame->IsMain() || context == nullptr || !context->IsValid()) {
        return;
    }

    (void)browser;
    CefRefPtr<CefV8Value> bridge = CefV8Value::CreateObject(nullptr, nullptr);
    const auto attributes = static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_DONTENUM);
    bridge->SetValue("postMessage", CefV8Value::CreateFunction("postMessage", new RendererBridgeHandler()), attributes);
    context->GetGlobal()->SetValue(kRendererBridgeGlobalName.data(), bridge, attributes);
}

void install_ready_watcher_in_context(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefFrame>& frame,
                                      const CefRefPtr<CefV8Context>& context) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (frame == nullptr || !frame->IsMain() || context == nullptr || browser == nullptr) {
        return;
    }
    CefRefPtr<CefV8Value> ignored;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(ready_watcher_script().c_str(), frame->GetURL(), 0, ignored, exception)) {
        return;
    }
    const std::string exception_message =
        exception != nullptr ? exception->GetMessage().ToString() : "unknown renderer bridge setup failure";
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kRendererBridgeErrorMessageName.data());
    message->GetArgumentList()->SetString(0U, exception_message);
    browser->GetMainFrame()->SendProcessMessage(PID_BROWSER, message);
}

}  // namespace mmltk::gui
