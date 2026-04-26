#pragma once

#include "gui/app_options.h"
#include "gui/browser_runtime_cef.h"
#include "gui/cef_views/cef_browser_shell.h"

namespace mmltk::gui {

class App;

class CefAppRunner {
   public:
    CefAppRunner(App& app, const AppLaunchOptions& options);

    [[nodiscard]] int run(int argc, char** argv);

   private:
    EmbeddedCefBrowserRuntime runtime_;
    CefBrowserShell shell_;
};

}  // namespace mmltk::gui
