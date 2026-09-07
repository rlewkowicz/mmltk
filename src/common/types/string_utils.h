#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace mmltk::common::types {

[[nodiscard]] inline std::string_view trim_http_field_value(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] inline std::string to_lower(const std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

[[nodiscard]] inline bool constant_time_equal(const std::string_view left, const std::string_view right) noexcept {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t width = std::max(left.size(), right.size());
    for (std::size_t index = 0U; index < width; ++index) {
        const unsigned char lhs = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const unsigned char rhs = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= static_cast<std::size_t>(lhs ^ rhs);
    }
    return difference == 0U;
}

}  // namespace mmltk::common::types
