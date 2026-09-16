#pragma once
#include <cstdio>
#include <string_view>
namespace mmltk::entrypoints::cli {
inline void print_command_help_line(const std::string_view name, const std::string_view description) noexcept {
    std::fprintf(stdout, "\n  %.*s  %.*s", static_cast<int>(name.size()), name.data(), static_cast<int>(description.size()), description.data());
}
}  // namespace mmltk::entrypoints::cli
