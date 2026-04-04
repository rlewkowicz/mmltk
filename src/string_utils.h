#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace mmltk::strings {

inline std::string to_lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char c) { return std::tolower(c); });
    return result;
}

} // namespace mmltk::strings
