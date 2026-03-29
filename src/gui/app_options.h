#pragma once

#include <string>

namespace fastloader::gui {

struct AppLaunchOptions {
    std::string vast_api_key;
};

AppLaunchOptions parse_app_launch_options(int argc, char** argv);
std::string resolve_vast_api_key(const std::string& explicit_value, const char* env_value);

} // namespace fastloader::gui
