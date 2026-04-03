#pragma once

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace mmltk::runtime_paths {

[[nodiscard]] inline std::filesystem::path current_executable_path() {
    std::vector<char> buffer(256, '\0');
    while (true) {
        const ssize_t bytes = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
        if (bytes < 0) {
            throw std::runtime_error(std::string("failed to resolve current executable path: ") +
                                     std::strerror(errno));
        }
        if (static_cast<std::size_t>(bytes) < buffer.size() - 1U) {
            buffer[static_cast<std::size_t>(bytes)] = '\0';
            return {buffer.data()};
        }
        buffer.resize(buffer.size() * 2U, '\0');
    }
}

[[nodiscard]] inline std::filesystem::path install_prefix() {
    return current_executable_path().parent_path().parent_path();
}

[[nodiscard]] inline std::filesystem::path share_root() {
    return install_prefix() / "share" / "mmltk";
}

[[nodiscard]] inline std::filesystem::path python_asset_path(const char* filename) {
    return share_root() / "python" / filename;
}

[[nodiscard]] inline std::filesystem::path font_asset_path(const char* filename) {
    return share_root() / "fonts" / filename;
}

} // namespace mmltk::runtime_paths
