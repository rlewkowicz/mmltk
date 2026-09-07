#include "detail/onnx_tool_main.h"

#include <filesystem>
#include <string>
#include <vector>

#include "catch2_compat.hpp"

namespace {

std::filesystem::path captured_model_path;

void capture_model_path(const std::filesystem::path& path) { captured_model_path = path; }

constexpr mmltk::entrypoints::tools::OnnxToolMainConfig kTestConfig{
    .usage = "usage: mmltk-rfdetr-onnx-info [logging options] MODEL.onnx",
    .application_name = "mmltk-rfdetr-onnx-info-test",
    .logger_name = "onnx-info-test",
    .error_prefix = "onnx-info-test: ",
};

[[nodiscard]] int run_onnx_tool(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments)
        argv.push_back(argument.data());

    captured_model_path.clear();
    return mmltk::entrypoints::tools::run_onnx_tool_main(static_cast<int>(argv.size()), argv.data(), kTestConfig, &capture_model_path);
}

void test_onnx_tool_main_routes_one_model_after_logging_options() {
    CHECK(run_onnx_tool({"mmltk-rfdetr-onnx-info", "--log-level=off", "--log-file", "/tmp/mmltk-onnx-info-test.log", "--log-dir", "/tmp",
                         "/tmp/model.onnx"}) == 0);
    CHECK(captured_model_path == "/tmp/model.onnx");
}

void test_onnx_tool_main_rejects_a_missing_model() {
    CHECK(run_onnx_tool({"mmltk-rfdetr-onnx-info", "--log-level", "off"}) == 1);
    CHECK(captured_model_path.empty());
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[entrypoints][tools][onnx]", test_onnx_tool_main_routes_one_model_after_logging_options);
MMLTK_REGISTER_TEST_CASE("[entrypoints][tools][onnx]", test_onnx_tool_main_rejects_a_missing_model);
