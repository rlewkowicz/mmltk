#include "gui/cef_views/cef_app_runner.h"

namespace mmltk::gui {

CefAppRunner::CefAppRunner(App& app, const AppLaunchOptions& options) : runtime_(app, options), shell_(runtime_) {
    runtime_.attach_shell(shell_);
}

int CefAppRunner::run(const int argc, char** argv) {
    const bool initialized = runtime_.initialize(argc, argv);
    if (!initialized) {
        runtime_.shutdown();
        return runtime_.exit_code();
    }

    runtime_.run_message_loop();
    runtime_.shutdown();
    return runtime_.exit_code();
}

}  // namespace mmltk::gui
