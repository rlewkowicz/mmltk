#include "gui/browser_runtime.h"
#include "gui/browser_runtime_backend.h"
#include "support/catch2_compat.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][browser_runtime_dispatch]", test_runtime_dispatches_to_cef_backend);
