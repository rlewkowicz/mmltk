#include "gui/app_options.h"

#include "support/catch2_compat.hpp"
#include <string>
#include <vector>

namespace {

using namespace mmltk::gui;

AppLaunchOptions parse_options(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_app_launch_options(static_cast<int>(argv.size()), argv.data());
}

void test_flag_wins_over_env() {
    assert(resolve_vast_api_key("flag-key", "env-key") == "flag-key");
}

void test_env_fallback() {
    assert(resolve_vast_api_key("", "env-key") == "env-key");
}

void test_missing_values_resolve_empty() {
    assert(resolve_vast_api_key("", nullptr).empty());
}

void test_settings_path_and_seed_args_are_parsed() {
    const AppLaunchOptions options = parse_options({
        "mmltk-gui",
        "--settings-path",
        "/tmp/gui.json",
        "--seed-from-cli",
        "--",
        "rfdetr",
        "predict",
        "--compiled",
        "./val.bin",
        "--output",
        "./predictions.json",
        "--weights",
        "./rf-detr-seg-medium.pt",
    });

    assert(options.settings_path == "/tmp/gui.json");
    assert(options.seed_from_cli);
    assert(options.seed_cli_args.size() == 8U);
    assert(options.seed_cli_args.front() == "rfdetr");
    assert(options.seed_cli_args[1] == "predict");
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_flag_wins_over_env);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_env_fallback);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_missing_values_resolve_empty);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_settings_path_and_seed_args_are_parsed);
