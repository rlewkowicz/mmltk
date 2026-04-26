#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mmltk::gui {

void apply_gui_cli_seed_file(const std::filesystem::path& settings_path, const std::vector<std::string>& cli_args);

}
