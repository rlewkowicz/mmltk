#pragma once

#include <filesystem>

namespace mmltk::common::io::filesystem_utils {

void remove_path_recursively_best_effort(const std::filesystem::path& path);

}
