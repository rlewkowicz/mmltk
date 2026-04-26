#pragma once

#include "runtime_paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

inline std::filesystem::path resolve_sibling_tool_path(std::string_view tool_name, std::string_view env_override = {}) {
    if (!env_override.empty()) {
        const std::string env_name(env_override);
        if (const char* override_path = std::getenv(env_name.c_str());
            override_path != nullptr && override_path[0] != '\0') {
            return {override_path};
        }
    }
    return runtime_paths::current_executable_path().parent_path() / std::string(tool_name);
}

inline std::vector<char*> make_exec_argv(std::vector<std::string>& args) {
    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1U);
    for (std::string& arg : args) {
        raw_args.push_back(arg.data());
    }
    raw_args.push_back(nullptr);
    return raw_args;
}

}  // namespace mmltk::rfdetr
