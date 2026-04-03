#include "rfdetr/onnx_tool_args.h"

#include "support/catch2_compat.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

void test_parse_single_onnx_tool_args_supports_logging_flags() {
    std::vector<std::string> argv_storage{
        "mmltk-rfdetr-onnx-info",
        "--log-level=warn",
        "--log-file",
        "/tmp/onnx-info.log",
        "--log-dir",
        "/tmp/onnx-info-logs",
        "/tmp/model.onnx",
    };
    std::vector<char*> argv;
    argv.reserve(argv_storage.size());
    for (std::string& arg : argv_storage) {
        argv.push_back(arg.data());
    }

    const auto parsed = mmltk::rfdetr::parse_single_onnx_tool_args(
        static_cast<int>(argv.size()),
        argv.data(),
        "usage: mmltk-rfdetr-onnx-info [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx");
    assert(parsed.model_path == "/tmp/model.onnx");
    assert(parsed.logging.level.has_value());
    assert(*parsed.logging.level == spdlog::level::warn);
    assert(parsed.logging.log_file == "/tmp/onnx-info.log");
    assert(parsed.logging.log_dir == "/tmp/onnx-info-logs");
}

void test_parse_single_onnx_tool_args_requires_model_path() {
    std::vector<std::string> argv_storage{
        "mmltk-rfdetr-onnx-simplify",
        "--log-level",
        "info",
    };
    std::vector<char*> argv;
    argv.reserve(argv_storage.size());
    for (std::string& arg : argv_storage) {
        argv.push_back(arg.data());
    }

    bool threw = false;
    try {
        (void)mmltk::rfdetr::parse_single_onnx_tool_args(
            static_cast<int>(argv.size()),
            argv.data(),
            "usage: mmltk-rfdetr-onnx-simplify [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx");
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()) ==
               "usage: mmltk-rfdetr-onnx-simplify [--log-level LEVEL] [--log-file PATH] [--log-dir PATH] MODEL.onnx");
    }
    assert(threw);
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_tool_args]", test_parse_single_onnx_tool_args_supports_logging_flags);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][onnx_tool_args]", test_parse_single_onnx_tool_args_requires_model_path);
