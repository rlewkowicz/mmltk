#include "src/entrypoints/cli/tests/support/cli_path.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace mmltk::testsupport {
std::string mmltk_cli_path() {
#ifndef MMLTK_TEST_MMLTK_CLI_PATH
#error "MMLTK_TEST_MMLTK_CLI_PATH must be defined for CLI subprocess tests"
#endif
    const std::filesystem::path configured_path = MMLTK_TEST_MMLTK_CLI_PATH;
    std::error_code error;
    if (std::filesystem::exists(configured_path, error) && !error) { return configured_path.string(); }
    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read > 0) {
        buffer[static_cast<std::size_t>(bytes_read)] = '\0';
        const std::filesystem::path sibling_cli = std::filesystem::path(buffer.data()).parent_path() / "mmltk";
        error.clear();
        if (std::filesystem::exists(sibling_cli, error) && !error) { return sibling_cli.string(); }
    }
    const std::filesystem::path installed_cli = "/opt/mmltk/bin/mmltk";
    error.clear();
    if (std::filesystem::exists(installed_cli, error) && !error) { return installed_cli.string(); }
    return configured_path.string();
}
}  // namespace mmltk::testsupport
