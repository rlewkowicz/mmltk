#pragma once

#include <optional>
#include <string>

namespace fastloader::gui {

std::optional<std::string> pick_file_with_zenity(const char* title, const std::string& initial_path);
bool draw_file_picker_input(const char* label,
                            std::string& value,
                            bool browse_busy = false,
                            const char* browse_label = "Browse");

} // namespace fastloader::gui
