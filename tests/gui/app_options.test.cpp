#include "gui/app_options.h"

#include "support/catch2_compat.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
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
        "mmltk-browser-host",
        "--settings-path",
        "/tmp/gui.json",
        "--browser-app-dir",
        "/tmp/browser-app",
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
    assert(options.browser_app_dir == "/tmp/browser-app");
    assert(options.seed_from_cli);
    assert(options.seed_cli_args.size() == 8U);
    assert(options.seed_cli_args.front() == "rfdetr");
    assert(options.seed_cli_args[1] == "predict");
}

void test_forwarded_seed_args_require_seed_from_cli_flag() {
    bool threw = false;
    try {
        (void)parse_options({
            "mmltk-browser-host",
            "--",
            "rfdetr",
            "predict",
        });
    } catch (const std::runtime_error& error) {
        threw = std::string_view(error.what()).find("--seed-from-cli") != std::string_view::npos;
    }
    assert(threw);
}

void test_seed_from_cli_requires_forwarded_args() {
    bool threw = false;
    try {
        (void)parse_options({
            "mmltk-browser-host",
            "--seed-from-cli",
        });
    } catch (const std::runtime_error& error) {
        threw = std::string_view(error.what()).find("requires mmltk CLI arguments") != std::string_view::npos;
    }
    assert(threw);
}

void test_browser_app_dir_defaults_to_empty() {
    const AppLaunchOptions options = parse_options({
        "mmltk-browser-host",
    });

    assert(options.browser_app_dir.empty());
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_flag_wins_over_env);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_env_fallback);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_missing_values_resolve_empty);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_settings_path_and_seed_args_are_parsed);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_forwarded_seed_args_require_seed_from_cli_flag);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_seed_from_cli_requires_forwarded_args);
MMLTK_REGISTER_TEST_CASE("[gui][app_options]", test_browser_app_dir_defaults_to_empty);
