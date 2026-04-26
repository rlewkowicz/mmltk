#include "file_picker.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace mmltk::gui {

std::optional<std::string> pick_path_with_dialog(const char*, const std::string&, const FilePickerMode,
                                                 const std::span<const FilePickerFilter>) {
    throw std::runtime_error("native file dialogs are unsupported in the Wayland-only CEF cutover");
}

std::optional<std::string> pick_file_with_dialog(const char* title, const std::string& initial_path) {
    return pick_path_with_dialog(title, initial_path);
}

}  // namespace mmltk::gui
