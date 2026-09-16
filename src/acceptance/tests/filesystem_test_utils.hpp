#pragma once
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
namespace mmltk::testsupport {
[[nodiscard]] std::filesystem::path make_temp_root(const char* name_prefix);
void write_text_file(const std::filesystem::path& path, std::string_view contents);
void write_text_file(const std::filesystem::path& path, std::function_ref<void(std::ostream&)> write_contents);
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
class BrowserAssetDirectory final {
   public:
    explicit BrowserAssetDirectory(const char* name_prefix);
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

   private:
    ScopedTempDir root_;
};
}  // namespace mmltk::testsupport
