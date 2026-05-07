#include "gui/browser_runtime.h"
#include "gui/browser_runtime_backend.h"
#include "gui/browser_runtime_shared.h"
#include "support/catch2_compat.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

struct CefBackendCallState {
    int runtime_call_count = 0;
    const void* last_app = nullptr;
    const mmltk::gui::AppLaunchOptions* last_options = nullptr;
    int last_argc = 0;
    std::vector<std::string> last_argv;
    int runtime_return_value = 0;
};

CefBackendCallState g_cef_backend_state;

void reset_backend_state() {
    g_cef_backend_state = {};
}

[[nodiscard]] std::vector<std::string> copy_argv(const int argc, char** argv) {
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        values.emplace_back(argv[index] == nullptr ? "" : argv[index]);
    }
    return values;
}

template <std::size_t N>
[[nodiscard]] mmltk::gui::App& app_ref_from_storage(std::array<std::byte, N>& storage) {
    return *reinterpret_cast<mmltk::gui::App*>(storage.data());
}

}  // namespace

namespace mmltk::gui {

int run_cef_browser_runtime(App& app, const AppLaunchOptions& options, const int argc, char** argv) {
    ++g_cef_backend_state.runtime_call_count;
    g_cef_backend_state.last_app = static_cast<const void*>(&app);
    g_cef_backend_state.last_options = &options;
    g_cef_backend_state.last_argc = argc;
    g_cef_backend_state.last_argv = copy_argv(argc, argv);
    return g_cef_backend_state.runtime_return_value;
}

struct RuntimeGateTestApp {
    int applied_intent_count = 0;
    mmltk::browser::Workflow last_workflow = mmltk::browser::Workflow::Train;
    std::string last_intent;
};

void apply_browser_intent(RuntimeGateTestApp& app, const mmltk::browser::IntentMessage& intent) {
    ++app.applied_intent_count;
    app.last_workflow = intent.workflow;
    app.last_intent = intent.intent;
}

}  // namespace mmltk::gui

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/gui/browser_runtime_entry.cpp"

namespace {

void test_runtime_dispatches_to_cef_backend() {
    reset_backend_state();
    g_cef_backend_state.runtime_return_value = 29;

    mmltk::gui::AppLaunchOptions options;
    options.browser_app_dir = "/tmp/browser-app";
    alignas(std::max_align_t) std::array<std::byte, 64> app_storage{};
    auto& app = app_ref_from_storage(app_storage);
    std::array<char*, 3> argv{
        const_cast<char*>("mmltk-browser-host"),
        const_cast<char*>("--type=zygote"),
        const_cast<char*>("--no-zygote-sandbox"),
    };

    const int exit_code =
        mmltk::gui::run_embedded_browser_runtime(app, options, static_cast<int>(argv.size()), argv.data());

    assert(exit_code == 29);
    assert(g_cef_backend_state.runtime_call_count == 1);
    assert(g_cef_backend_state.last_app == static_cast<const void*>(&app));
    assert(g_cef_backend_state.last_options == &options);
    assert(g_cef_backend_state.last_argc == static_cast<int>(argv.size()));
    assert(g_cef_backend_state.last_argv.size() == argv.size());
    assert(g_cef_backend_state.last_argv.at(1) == "--type=zygote");
    assert(g_cef_backend_state.last_argv.at(2) == "--no-zygote-sandbox");
}

struct RuntimeCapabilityGateHarness {
    bool runtime_capabilities_reported = false;
    bool runtime_capability_gate_passed = false;
    bool quit_requested = false;
    std::optional<std::chrono::steady_clock::time_point> runtime_capability_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    mmltk::browser::BrowserRuntimeCapabilities reported_capabilities =
        mmltk::gui::browser_runtime_shared::configured_runtime_capabilities(mmltk::browser::BrowserHostBackend::Cef);
    mmltk::browser::BrowserBridgeState bridge_state;
    int refresh_before_verify_count = 0;
    int capability_changed_count = 0;
    int verify_count = 0;
    int queue_count = 0;
    int refresh_snapshot_count = 0;
    int flush_count = 0;

    void handle(const nlohmann::json& message) {
        using namespace mmltk::gui::browser_runtime_shared;

        const auto verify_runtime_capabilities = [&] { ++verify_count; };
        const auto queue_bridge_state = [&] { ++queue_count; };
        const auto refresh_snapshot = [&] { ++refresh_snapshot_count; };
        const auto flush_pending_messages = [&] { ++flush_count; };
        const auto maybe_complete_gate = [&] {
            maybe_complete_runtime_capability_gate(true, runtime_capabilities_reported, runtime_capability_gate_passed,
                                                   quit_requested, runtime_capability_deadline, bridge_state,
                                                   verify_runtime_capabilities, queue_bridge_state, refresh_snapshot,
                                                   flush_pending_messages);
        };

        handle_runtime_capabilities_message(
            message, runtime_capabilities_reported, runtime_capability_gate_passed, quit_requested,
            reported_capabilities, bridge_state, [&] { ++refresh_before_verify_count; },
            [&] { ++capability_changed_count; }, verify_runtime_capabilities, maybe_complete_gate);
    }
};

void test_runtime_capability_gate_blocks_intents_with_workspace_bridge_unknown() {
    using namespace mmltk::gui::browser_runtime_shared;

    RuntimeCapabilityGateHarness harness;
    harness.handle(nlohmann::json{{"type", std::string(kRuntimeCapabilitiesMessageType)},
                                  {"navigator_gpu", true},
                                  {"workspace_surface_bridge", "unknown"}});

    assert(!harness.runtime_capabilities_reported);
    assert(!harness.runtime_capability_gate_passed);
    assert(harness.reported_capabilities.navigator_gpu == mmltk::browser::BrowserRuntimeCapabilityStatus::Available);
    assert(harness.reported_capabilities.workspace_surface_bridge ==
           mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown);
    assert(harness.reported_capabilities.workspace_surface_zero_copy ==
           mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown);
    assert(!harness.bridge_state.connected);
    assert(harness.runtime_capability_deadline.has_value());
    assert(harness.refresh_before_verify_count == 1);
    assert(harness.capability_changed_count == 1);
    assert(harness.verify_count == 0);
    assert(harness.queue_count == 0);
    assert(harness.refresh_snapshot_count == 0);
    assert(harness.flush_count == 0);
}

void test_runtime_capability_gate_allows_intents_with_workspace_zero_copy_unknown() {
    using namespace mmltk::gui::browser_runtime_shared;

    RuntimeCapabilityGateHarness harness;
    harness.handle(nlohmann::json{{"type", std::string(kRuntimeCapabilitiesMessageType)},
                                  {"navigator_gpu", true},
                                  {"workspace_surface_bridge", "available"},
                                  {"workspace_surface_zero_copy", "unknown"}});

    assert(harness.runtime_capabilities_reported);
    assert(harness.runtime_capability_gate_passed);
    assert(harness.reported_capabilities.navigator_gpu == mmltk::browser::BrowserRuntimeCapabilityStatus::Available);
    assert(harness.reported_capabilities.workspace_surface_bridge ==
           mmltk::browser::BrowserRuntimeCapabilityStatus::Available);
    assert(harness.reported_capabilities.workspace_surface_zero_copy ==
           mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown);
    assert(harness.bridge_state.connected);
    assert(!harness.runtime_capability_deadline.has_value());
    assert(harness.refresh_before_verify_count == 1);
    assert(harness.capability_changed_count == 1);
    assert(harness.verify_count == 1);
    assert(harness.queue_count == 1);
    assert(harness.refresh_snapshot_count == 1);
    assert(harness.flush_count == 1);

    mmltk::gui::RuntimeGateTestApp app;
    mmltk::browser::IntentMessage intent;
    intent.workflow = mmltk::browser::Workflow::Annotate;
    intent.intent = "annotate.live.start";
    intent.payload = nlohmann::json::object();
    bool bridge_error_cleared = false;
    bool dispatch_pending_idle = false;
    mmltk::browser::BrowserBridgePhase updated_phase = mmltk::browser::BrowserBridgePhase::Polling;

    handle_intent_message(
        app, nlohmann::json(intent), harness.runtime_capability_gate_passed,
        [&](const std::string& message, const bool report_to_page) {
            assert(message.empty());
            assert(!report_to_page);
            bridge_error_cleared = true;
        },
        [&](const mmltk::browser::BrowserBridgePhase phase) { updated_phase = phase; }, dispatch_pending_idle);

    assert(app.applied_intent_count == 1);
    assert(app.last_workflow == mmltk::browser::Workflow::Annotate);
    assert(app.last_intent == "annotate.live.start");
    assert(bridge_error_cleared);
    assert(updated_phase == mmltk::browser::BrowserBridgePhase::Dispatch);
    assert(dispatch_pending_idle);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][browser_runtime_dispatch]", test_runtime_dispatches_to_cef_backend);
MMLTK_REGISTER_TEST_CASE("[gui][browser_runtime_dispatch]",
                         test_runtime_capability_gate_blocks_intents_with_workspace_bridge_unknown);
MMLTK_REGISTER_TEST_CASE("[gui][browser_runtime_dispatch]",
                         test_runtime_capability_gate_allows_intents_with_workspace_zero_copy_unknown);
