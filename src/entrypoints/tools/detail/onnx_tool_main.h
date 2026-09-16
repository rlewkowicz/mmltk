#pragma once
#include <filesystem>
#include <string_view>
namespace mmltk::entrypoints::tools {
struct OnnxToolMainConfig final {
    std::string_view usage;
    std::string_view application_name;
    std::string_view logger_name;
    std::string_view error_prefix;
};
using OnnxToolOperation = void (*)(const std::filesystem::path&);
int run_onnx_tool_main(int argc, char** argv, const OnnxToolMainConfig& config, OnnxToolOperation operation);
}  // namespace mmltk::entrypoints::tools
