#include "filesystem_test_utils.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "src/common/io/filesystem_utils.h"
namespace mmltk::testsupport {
std::filesystem::path make_temp_root(const char* const name_prefix) {
    std::string pattern = (std::filesystem::temp_directory_path() / (std::string{name_prefix} + ".XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* const created = ::mkdtemp(writable.data());
    if (created == nullptr) throw std::runtime_error(std::string{"mkdtemp failed: "} + std::strerror(errno));
    return std::filesystem::path{created};
}
void write_text_file(const std::filesystem::path& path, const std::string_view contents) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::trunc};
    if (!stream.is_open()) throw std::runtime_error("failed to write file: " + path.string());
    stream << contents;
}
void write_text_file(const std::filesystem::path& path, const std::function_ref<void(std::ostream&)> write_contents) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::trunc};
    if (!stream.is_open()) throw std::runtime_error("failed to write file: " + path.string());
    write_contents(stream);
}
void make_executable(const std::filesystem::path& path, const std::filesystem::perms permissions, const std::filesystem::perm_options options) {
    std::filesystem::permissions(path, permissions, options);
}
void write_executable_file(const std::filesystem::path& path, const std::string_view contents) {
    write_text_file(path, contents);
    make_executable(path);
}
void write_executable_file(const std::filesystem::path& path, const std::function_ref<void(std::ostream&)> write_contents) {
    write_text_file(path, write_contents);
    make_executable(path);
}
ScopedTempDir::ScopedTempDir(const char* const name_prefix) : path_(make_temp_root(name_prefix)) {}
ScopedTempDir::~ScopedTempDir() {
    if (!path_.empty()) mmltk::common::io::filesystem_utils::remove_path_recursively_best_effort(path_);
}
const std::filesystem::path& ScopedTempDir::path() const noexcept { return path_; }
BrowserAssetDirectory::BrowserAssetDirectory(const char* const name_prefix) : root_(name_prefix) {
    write_text_file(root_.path() / "index.html", "<!doctype html>");
}
const std::filesystem::path& BrowserAssetDirectory::path() const noexcept { return root_.path(); }
}  // namespace mmltk::testsupport
