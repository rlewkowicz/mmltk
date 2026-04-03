#include "app_options.h"

#include "CLI11.hpp"

#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace mmltk::gui {

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
    std::vector<std::string> launch_args;
    launch_args.reserve(static_cast<size_t>(argc));
    launch_args.emplace_back(argc > 0 ? argv[0] : "mmltk-gui");

    bool saw_separator = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (!saw_separator && arg == "--") {
            saw_separator = true;
            continue;
        }
        if (saw_separator) {
            options.seed_cli_args.push_back(arg);
        } else {
            launch_args.push_back(arg);
        }
    }

    std::vector<const char*> raw_launch_args;
    raw_launch_args.reserve(launch_args.size());
    for (const std::string& arg : launch_args) {
        raw_launch_args.push_back(arg.c_str());
    }

    CLI::App app{"mmltk GUI"};
    app.set_help_all_flag("--help-all", "Show all options");
    app.add_option("--vast-api-key", options.vast_api_key, "Optional Vast.ai API key for remote offer queries");
    app.add_option("--settings-path", options.settings_path, "Path to the GUI settings JSON file")
        ->type_name("PATH");
    std::string log_level_option;
    std::string log_file_option;
    std::string log_dir_option;
    app.add_option("--log-level", log_level_option,
                   "Logging level (trace, debug, info, warn, error, critical, off)");
    app.add_option("--log-file", log_file_option, "Explicit log file path")->type_name("PATH");
    app.add_option("--log-dir", log_dir_option,
                   "Log directory for the default rotating file sink")->type_name("PATH");
    app.add_flag("--seed-from-cli", options.seed_from_cli,
                 "Internal: seed GUI state from forwarded mmltk CLI arguments")
        ->group("");

    try {
        app.parse(static_cast<int>(raw_launch_args.size()), raw_launch_args.data());
    } catch (const CLI::ParseError& error) {
        app.exit(error);
        std::exit(error.get_exit_code() == kCliParseSuccess ? 0 : 1);
    }

    if (options.seed_from_cli && options.seed_cli_args.empty()) {
        throw std::runtime_error("--seed-from-cli requires mmltk CLI arguments after `--`");
    }
    if (!options.seed_from_cli && !options.seed_cli_args.empty()) {
        throw std::runtime_error("forwarded CLI arguments after `--` require --seed-from-cli");
    }

    options.vast_api_key = resolve_vast_api_key(options.vast_api_key, std::getenv("VAST_API_KEY"));
    return options;
}

} // namespace mmltk::gui
