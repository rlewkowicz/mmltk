#pragma once

#include <string>
#include <vector>

namespace mmltk::gui {

struct AppLaunchOptions {
    std::string vast_api_key;
    std::string settings_path = "gui.json";
    bool seed_from_cli = false;
    std::vector<std::string> seed_cli_args;
};

AppLaunchOptions parse_app_launch_options(int argc, char** argv);
std::string resolve_vast_api_key(const std::string& explicit_value, const char* env_value);

} // namespace mmltk::gui
