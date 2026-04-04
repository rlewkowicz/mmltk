#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include "string_utils.h"

namespace mmltk::rfdetr {



inline bool has_extension(const std::filesystem::path& path, const char* extension) {
    return strings::to_lower(path.extension().string()) == extension;
}

} // namespace mmltk::rfdetr
