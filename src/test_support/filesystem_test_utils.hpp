#pragma once
#include <filesystem>
#include <cstdint>
#include <functional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
namespace mmltk::testsupport {
[[nodiscard]] std::filesystem::path make_temp_root(const char* name_prefix);
void write_text_file(const std::filesystem::path& path, std::string_view contents);
void write_text_file(const std::filesystem::path& path, std::function_ref<void(std::ostream&)> write_contents);
void write_binary_file(const std::filesystem::path& path, std::span<const std::uint8_t> contents);
void make_executable(const std::filesystem::path& path, std::filesystem::perms permissions = std::filesystem::perms::owner_exec,
                     std::filesystem::perm_options options = std::filesystem::perm_options::add);
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
