#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::controller::services {

inline void append_command_option(std::vector<std::string>& arguments, const std::string_view name, const std::string_view value) {
    if (value.empty()) { return; }
    arguments.emplace_back(name);
    arguments.emplace_back(value);
}

inline void append_command_option(std::vector<std::string>& arguments, const std::string_view name, const std::string& value) {
    append_command_option(arguments, name, std::string_view{value});
}

inline void append_command_option(std::vector<std::string>& arguments, const std::string_view name, const std::filesystem::path& value) {
    const std::string encoded = value.string();
    append_command_option(arguments, name, std::string_view{encoded});
}

inline void append_command_option(std::vector<std::string>& arguments, const std::string_view name, const double value) {
    arguments.emplace_back(name);
    arguments.push_back(std::to_string(value));
}

inline void append_command_option(std::vector<std::string>& arguments, const std::string_view name, const std::optional<double> value) {
    if (value.has_value()) { append_command_option(arguments, name, *value); }
}

inline void append_command_flag(std::vector<std::string>& arguments, const std::string_view name, const bool enabled = true) {
    if (enabled) { arguments.emplace_back(name); }
}

}  // namespace mmltk::controller::services
