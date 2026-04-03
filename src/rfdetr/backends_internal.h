#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace mmltk::rfdetr {

inline std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline bool has_extension(const std::filesystem::path& path, const char* extension) {
    return lower_copy(path.extension().string()) == extension;
}

} // namespace mmltk::rfdetr
