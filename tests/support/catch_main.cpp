#include "mmltk_logging.h"

#include <catch2/catch_session.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool is_logging_option(std::string_view arg, std::string_view name) {
    const std::string prefix = std::string(name) + "=";
    return arg == name ||
           (arg.size() >= prefix.size() && arg.compare(0, prefix.size(), prefix) == 0);
}

std::vector<std::string> filter_logging_args(int argc, char** argv) {
    std::vector<std::string> filtered;
    filtered.reserve(static_cast<size_t>(argc));
    if (argc > 0 && argv[0] != nullptr) {
        filtered.emplace_back(argv[0]);
    }

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (is_logging_option(arg, "--log-level") ||
            is_logging_option(arg, "--log-file") ||
            is_logging_option(arg, "--log-dir")) {
            if ((arg == "--log-level" || arg == "--log-file" || arg == "--log-dir") &&
                index + 1 < argc) {
                ++index;
            }
            continue;
        }
        filtered.emplace_back(argv[index]);
    }

    return filtered;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string app_name = argc > 0 && argv[0] != nullptr
            ? std::filesystem::path(argv[0]).filename().string()
            : std::string("mmltk_tests");
        const mmltk::logging::CliOverrides overrides = mmltk::logging::scan_cli_overrides(argc, argv);
        mmltk::logging::initialize(mmltk::logging::merge(mmltk::logging::config_from_env(app_name), overrides));

        const std::vector<std::string> filtered_args = filter_logging_args(argc, argv);
        std::vector<char*> raw_args;
        raw_args.reserve(filtered_args.size());
        for (const std::string& arg : filtered_args) {
            raw_args.push_back(const_cast<char*>(arg.c_str()));
        }

        Catch::Session session;
        return session.run(static_cast<int>(raw_args.size()), raw_args.data());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "catch main error: %s\n", error.what());
        return 1;
    }
}
