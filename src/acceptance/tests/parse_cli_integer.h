#pragma once

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace mmltk::testsupport {

[[nodiscard]] inline int parse_cli_integer(const char* value, const std::string_view option_name) {
    if (value == nullptr) { throw std::invalid_argument("missing integer for " + std::string(option_name)); }
    const std::string_view text(value);
    int result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("invalid integer for " + std::string(option_name) + ": " + std::string(text));
    }
    return result;
}

}  // namespace mmltk::testsupport
