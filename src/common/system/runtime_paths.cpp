#include "src/common/system/runtime_paths.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/common/system/time_utils.h"

namespace mmltk::common::system {

std::uint64_t steady_clock_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace runtime_paths {
std::filesystem::path repository_root() {
    if (const char* root = std::getenv("MMLTK_REPO_ROOT"); root != nullptr && root[0] != '\0') return root;
    return std::filesystem::current_path();
}
std::filesystem::path current_executable_path() {
    std::vector<char> buffer(256U, '\0');
    for (;;) {
        const ssize_t bytes = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
        if (bytes < 0) throw std::runtime_error(std::string("failed to resolve current executable path: ") + std::strerror(errno));
        if (static_cast<std::size_t>(bytes) < buffer.size() - 1U) {
            buffer[static_cast<std::size_t>(bytes)] = '\0';
            return buffer.data();
        }
        buffer.resize(buffer.size() * 2U, '\0');
    }
}
std::filesystem::path install_prefix() { return current_executable_path().parent_path().parent_path(); }
std::filesystem::path share_root() { return install_prefix() / "share" / "mmltk"; }
std::filesystem::path browser_app_root() { return share_root() / "browser-app"; }
std::filesystem::path python_asset_path(const char* filename) { return share_root() / "python" / filename; }
std::filesystem::path font_asset_path(const char* filename) { return share_root() / "fonts" / filename; }
}  // namespace runtime_paths
}  // namespace mmltk::common::system
