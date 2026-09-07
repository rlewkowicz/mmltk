#pragma once

#include <filesystem>

namespace mmltk::common::system::runtime_paths {

[[nodiscard]] std::filesystem::path repository_root();
[[nodiscard]] std::filesystem::path current_executable_path();
[[nodiscard]] std::filesystem::path install_prefix();
[[nodiscard]] std::filesystem::path share_root();
[[nodiscard]] std::filesystem::path browser_app_root();
[[nodiscard]] std::filesystem::path python_asset_path(const char* filename);
[[nodiscard]] std::filesystem::path font_asset_path(const char* filename);

}  // namespace mmltk::common::system::runtime_paths
