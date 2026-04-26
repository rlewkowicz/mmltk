#pragma once

#include "gui/app_options.h"

namespace mmltk::gui {

class App;

[[nodiscard]] int run_cef_browser_runtime(App& app, const AppLaunchOptions& options, int argc, char** argv);

}  // namespace mmltk::gui
