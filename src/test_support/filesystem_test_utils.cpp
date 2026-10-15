#include "src/test_support/filesystem_test_utils.hpp"
#include <cerrno>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <fcntl.h>
#include <sys/file.h>
#include "src/common/io/file_memory.h"
namespace mmltk::testsupport {
mmltk::common::io::FileHandle open_test_directory(const std::filesystem::path& path) {
 mmltk::common::io::FileHandle directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
 REQUIRE(directory.get() >= 0);
 return directory;
}
FileLockProbe probe_file_lock(const std::filesystem::path& path) {
 mmltk::common::io::FileHandle descriptor(::open(path.c_str(), O_RDWR | O_CLOEXEC));
 if (descriptor.get() < 0) throw mmltk::common::io::errno_error("cannot open file lock probe", path.string());
 const bool available = ::flock(descriptor.get(), LOCK_EX | LOCK_NB) == 0;
 if (!available && errno != EWOULDBLOCK && errno != EAGAIN) throw mmltk::common::io::errno_error("cannot probe file lock", path.string());
 return {std::move(descriptor), available};
}
std::optional<std::filesystem::path> canonical_fixture_path(const std::filesystem::path& root, const std::filesystem::path& path) {
 auto absolute = std::filesystem::canonical(path.is_absolute() ? path : root / path);
 const auto relative = absolute.lexically_relative(root);
 if (relative.empty() || *relative.begin() == "..") return std::nullopt;
 return absolute;
}
std::filesystem::path make_temp_root(const char* const name_prefix) {
 std::string pattern = (std::filesystem::temp_directory_path() / (std::string{name_prefix} + ".XXXXXX")).string();
 const char* const created = ::mkdtemp(pattern.data());
 if (created == nullptr) throw std::runtime_error(std::string{"mkdtemp failed: "} + std::strerror(errno));
 return std::filesystem::path{created};
}
void write_text_file(const std::filesystem::path& path, const std::string_view contents) {
 write_text_file(path, [&](std::ostream& stream) { stream << contents; });
}
void write_text_file(const std::filesystem::path& path, std::string_view contents, std::filesystem::perms permissions) {
 write_text_file(path, contents);
 std::filesystem::permissions(path, permissions);
}
void write_private_text_file(const std::filesystem::path& path, std::string_view contents) {
 auto file = mmltk::common::io::FileHandle::create_output(path.string(), contents.size());
 file.pwrite_all(contents.data(), contents.size(), 0);
}
void write_text_file(const std::filesystem::path& path, const std::function_ref<void(std::ostream&)> write_contents) {
 if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
 std::ofstream stream{path, std::ios::binary | std::ios::trunc};
 if (!stream.is_open()) throw std::runtime_error("failed to write file: " + path.string());
 write_contents(stream);
 stream.close();
 if (!stream) throw std::runtime_error("failed to finish writing file: " + path.string());
}
void write_binary_file(const std::filesystem::path& path, const std::span<const std::uint8_t> contents) {
 write_text_file(path, [&](std::ostream& stream) {
  if (!contents.empty()) stream.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
 });
}
namespace {
template <class Storage>
Storage read_file(const std::filesystem::path& path) {
 const auto file = mmltk::common::io::FileHandle::open_readonly(path.string());
 Storage bytes(file.size(), typename Storage::value_type{});
 file.pread_all(bytes.data(), bytes.size(), 0);
 return bytes;
}
}  // namespace
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) { return read_file<std::vector<std::uint8_t>>(path); }
std::string read_text_file(const std::filesystem::path& path) { return read_file<std::string>(path); }
nlohmann::json read_json_line(std::istream& input) {
 std::string line;
 REQUIRE(static_cast<bool>(std::getline(input, line)));
 return nlohmann::json::parse(line);
}
struct stat file_status(const std::filesystem::path& path) {
 struct stat status{};
 REQUIRE(::stat(path.c_str(), &status) == 0);
 return status;
}
ScopedWorkingDirectory::ScopedWorkingDirectory(const std::filesystem::path& path) : previous_(std::filesystem::current_path()) {
 std::filesystem::create_directories(path);
 std::filesystem::current_path(path);
}
ScopedWorkingDirectory::~ScopedWorkingDirectory() { std::filesystem::current_path(previous_); }
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
 if (!path_.empty()) mmltk::common::io::remove_path_recursively_best_effort(path_);
}
const std::filesystem::path& ScopedTempDir::path() const noexcept { return path_; }
}  // namespace mmltk::testsupport
