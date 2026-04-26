#include "gui/browser_runtime.h"

#include "gui/browser_runtime_backend.h"

namespace mmltk::gui {

int run_embedded_browser_runtime(App& app, const AppLaunchOptions& options, int argc, char** argv) {
    return run_cef_browser_runtime(app, options, argc, argv);
}

}  // namespace mmltk::gui
