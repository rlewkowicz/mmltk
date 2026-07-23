#pragma once

#include "gui/app_options.h"

namespace mmltk::gui {

class App;

[[nodiscard]] int run_firefox_browser_runtime(App& app, const AppLaunchOptions& options, int argc, char** argv);

}  
