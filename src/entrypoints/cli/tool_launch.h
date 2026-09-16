#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace mmltk::entrypoints::cli {
std::filesystem::path resolve_sibling_tool_path(std::string_view tool_name, std::string_view env_override = {});
std::vector<char*> make_exec_argv(std::vector<std::string>& args);
}
