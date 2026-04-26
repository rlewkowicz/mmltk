#include "app_api.h"
#include "app_options.h"
#include "browser_runtime.h"
#include "cli_seed.h"
#include "execution_policy.h"
#include "mmltk_logging.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

void report_gui_error(std::string_view message) noexcept {
    try {
        mmltk::logging::logger("gui")->error("{}", message);
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        mmltk::logging::initialize(mmltk::logging::merge(mmltk::logging::config_from_env("mmltk_browser_host"),
                                                         mmltk::logging::scan_cli_overrides(argc, argv)));
        const mmltk::ExecutionPolicySnapshot execution_snapshot = mmltk::apply_process_execution_policy();
        mmltk::log_process_execution_policy("mmltk_browser_host", execution_snapshot, false, true);
    } catch (const std::exception& error) {
        report_gui_error(std::string("mmltk browser runtime error: ") + error.what());
        return 1;
    }

    mmltk::gui::AppLaunchOptions launch_options;
    try {
        launch_options = mmltk::gui::parse_app_launch_options(argc, argv);
        if (launch_options.seed_from_cli) {
            mmltk::gui::apply_gui_cli_seed_file(launch_options.settings_path, launch_options.seed_cli_args);
        }
    } catch (const std::exception& error) {
        report_gui_error(std::string("mmltk browser runtime error: ") + error.what());
        return 1;
    }

    int exit_code = 0;
    try {
        mmltk::gui::AppHandle app = mmltk::gui::make_app(launch_options.vast_api_key, launch_options.settings_path);
        exit_code = mmltk::gui::run_embedded_browser_runtime(*app, launch_options, argc, argv);
        mmltk::gui::shutdown(*app);
    } catch (const std::exception& error) {
        report_gui_error(std::string("mmltk browser runtime error: ") + error.what());
        exit_code = 1;
    }
    return exit_code;
}
