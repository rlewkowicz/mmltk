#include "tool_launch.h"
#include <cstdlib>
#include "src/common/system/runtime_paths.h"
namespace mmltk::entrypoints::cli {
std::filesystem::path resolve_sibling_tool_path(std::string_view tool_name, std::string_view env_override) {
    if (!env_override.empty()) {
        const std::string env_name(env_override);
        if (const char* override_path = std::getenv(env_name.c_str()); override_path != nullptr && override_path[0] != '\0') { return {override_path}; }
    }
    return mmltk::common::system::runtime_paths::current_executable_path().parent_path() / std::string(tool_name);
}
std::vector<char*> make_exec_argv(std::vector<std::string>& args) {
    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1U);
    for (std::string& arg : args) { raw_args.push_back(arg.data()); }
    raw_args.push_back(nullptr);
    return raw_args;
}
}  // namespace mmltk::entrypoints::cli
