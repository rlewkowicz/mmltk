#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mmltk::gui {

enum class FilePickerMode : std::uint8_t {
    OpenFile = 0,
    OpenFolder = 1,
    SaveFile = 2,
};

struct FilePickerFilter {
    std::string name;
    std::vector<std::string> patterns;
};

std::optional<std::string> pick_path_with_dialog(const char* title, FilePickerMode mode = FilePickerMode::OpenFile,
                                                 std::span<const FilePickerFilter> filters = {});
std::optional<std::string> pick_file_with_dialog(const char* title);

}  
