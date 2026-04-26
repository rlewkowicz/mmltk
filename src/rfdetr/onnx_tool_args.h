#pragma once

#include "mmltk_logging.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mmltk::rfdetr {

struct SingleOnnxToolArgs {
    std::filesystem::path model_path;
    mmltk::logging::CliOverrides logging;
};

inline bool is_onnx_logging_option(std::string_view arg, std::string_view name) {
    const std::string prefix = std::string(name) + "=";
    return arg == name || arg.starts_with(prefix);
}

inline SingleOnnxToolArgs parse_single_onnx_tool_args(int argc, char** argv, std::string_view usage) {
    SingleOnnxToolArgs parsed;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (is_onnx_logging_option(arg, "--log-level")) {
            if (arg == "--log-level") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for --log-level");
                }
                parsed.logging.level = mmltk::logging::parse_level(argv[++index]);
            } else {
                parsed.logging.level = mmltk::logging::parse_level(arg.substr(std::string_view("--log-level=").size()));
            }
            continue;
        }
        if (is_onnx_logging_option(arg, "--log-file")) {
            if (arg == "--log-file") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for --log-file");
                }
                parsed.logging.log_file = std::filesystem::path(argv[++index]);
            } else {
                parsed.logging.log_file = std::filesystem::path(arg.substr(std::string_view("--log-file=").size()));
            }
            continue;
        }
        if (is_onnx_logging_option(arg, "--log-dir")) {
            if (arg == "--log-dir") {
                if (index + 1 >= argc) {
                    throw std::runtime_error("missing value for --log-dir");
                }
                parsed.logging.log_dir = std::filesystem::path(argv[++index]);
            } else {
                parsed.logging.log_dir = std::filesystem::path(arg.substr(std::string_view("--log-dir=").size()));
            }
            continue;
        }
        if (!parsed.model_path.empty()) {
            throw std::runtime_error(std::string(usage));
        }
        parsed.model_path = std::filesystem::path(arg);
    }
    if (parsed.model_path.empty()) {
        throw std::runtime_error(std::string(usage));
    }
    return parsed;
}

}  // namespace mmltk::rfdetr
