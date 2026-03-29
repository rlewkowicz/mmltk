#include "app_options.h"

#include "CLI11.hpp"

#include <cstdlib>

namespace fastloader::gui {

namespace {

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

} // namespace

std::string resolve_vast_api_key(const std::string& explicit_value, const char* env_value) {
    if (!explicit_value.empty()) {
        return explicit_value;
    }
    if (env_value != nullptr) {
        return env_value;
    }
    return {};
}

AppLaunchOptions parse_app_launch_options(int argc, char** argv) {
    AppLaunchOptions options;

    CLI::App app{"fastloader GUI"};
    app.set_help_all_flag("--help-all", "Show all options");
    app.add_option("--vast-api-key", options.vast_api_key, "Optional Vast.ai API key for remote offer queries");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        app.exit(error);
        std::exit(error.get_exit_code() == kCliParseSuccess ? 0 : 1);
    }

    options.vast_api_key = resolve_vast_api_key(options.vast_api_key, std::getenv("VAST_API_KEY"));
    return options;
}

} // namespace fastloader::gui
