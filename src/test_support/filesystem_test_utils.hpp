#pragma once
#include "src/common/io/file_memory.h"
#include <filesystem>
#include <cstdint>
#include <functional>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
namespace mmltk::testsupport {
struct FileLockProbe {
 mmltk::common::io::FileHandle descriptor;
 bool available;
};
// Retain the opened inode and any acquired lock until the caller retires the probe.
[[nodiscard]] mmltk::common::io::FileHandle open_test_directory(const std::filesystem::path& path);
[[nodiscard]] FileLockProbe probe_file_lock(const std::filesystem::path& path);
// Root must already be canonical. Existing paths resolve through symlinks before containment is checked.
[[nodiscard]] std::optional<std::filesystem::path> canonical_fixture_path(const std::filesystem::path& root, const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path make_temp_root(const char* name_prefix);
void write_text_file(const std::filesystem::path& path, std::string_view contents);
void write_text_file(const std::filesystem::path& path, std::string_view contents, std::filesystem::perms permissions);
void write_private_text_file(const std::filesystem::path& path, std::string_view contents);
void write_text_file(const std::filesystem::path& path, std::function_ref<void(std::ostream&)> write_contents);
void write_binary_file(const std::filesystem::path& path, std::span<const std::uint8_t> contents);
[[nodiscard]] std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);
[[nodiscard]] nlohmann::json read_json_line(std::istream& input);
[[nodiscard]] struct stat file_status(const std::filesystem::path& path);
// Process-wide fixture state; only use in tests that own the working directory.
class ScopedWorkingDirectory final {
public:
 explicit ScopedWorkingDirectory(const std::filesystem::path& path);
 ~ScopedWorkingDirectory();
 ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
 ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

private:
 std::filesystem::path previous_;
};
void make_executable(
 const std::filesystem::path& path, std::filesystem::perms permissions = std::filesystem::perms::owner_exec, std::filesystem::perm_options options = std::filesystem::perm_options::add);
void write_executable_file(const std::filesystem::path& path, std::string_view contents);
void write_executable_file(const std::filesystem::path& path, std::function_ref<void(std::ostream&)> write_contents);
class ScopedTempDir {
public:
 explicit ScopedTempDir(const char* name_prefix);
 ~ScopedTempDir();
 [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
 std::filesystem::path path_;
};
}  // namespace mmltk::testsupport
