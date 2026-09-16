#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <new>
#include <system_error>
#include "src/common/io/file_memory.h"
namespace mmltk::common::io {
namespace {
void assign_errno(std::error_code& error, const int value = errno) noexcept { error.assign(value, std::generic_category()); }
[[nodiscard]] bool is_dot_entry(const char* name) noexcept { return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')); }
enum class CleanupPolicy { FailFast, BestEffort };
template <typename Remove>
bool remove_directory_entries(DIR* directory, CleanupPolicy policy, Remove remove, std::error_code& error) {
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) { assign_errno(error); return false; }
            return true;
        }
        if (is_dot_entry(entry->d_name)) continue;
        if (!remove(entry->d_name) && policy == CleanupPolicy::FailFast) return false;
    }
}
[[nodiscard]] bool remove_entry_at(const int parent_fd, const char* name, const dev_t root_device, std::error_code& error) noexcept {
    if (parent_fd < 0) {
        assign_errno(error, EBADF);
        return false;
    }
    struct stat entry_status{};
    if (::fstatat(parent_fd, name, &entry_status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) { return true; }
        assign_errno(error);
        return false;
    }
    if (entry_status.st_dev != root_device) {
        assign_errno(error, EXDEV);
        return false;
    }
    if (!S_ISDIR(entry_status.st_mode)) {
        if (::unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT) { return true; }
        assign_errno(error);
        return false;
    }
    const int child_fd = ::openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_fd < 0) {
        if (errno == ENOENT) { return true; }
        assign_errno(error);
        return false;
    }
    DIR* directory = ::fdopendir(child_fd);
    if (directory == nullptr) {
        const int saved_errno = errno;
        ::close(child_fd);
        assign_errno(error, saved_errno);
        return false;
    }
    const int directory_fd = ::dirfd(directory);
    if (directory_fd < 0) {
        const int saved_errno = errno;
        ::closedir(directory);
        assign_errno(error, saved_errno);
        return false;
    }
    struct stat opened_status{};
    if (::fstat(directory_fd, &opened_status) != 0) {
        const int saved_errno = errno;
        ::closedir(directory);
        assign_errno(error, saved_errno);
        return false;
    }
    if (opened_status.st_dev != entry_status.st_dev || opened_status.st_ino != entry_status.st_ino) {
        ::closedir(directory);
        assign_errno(error, ESTALE);
        return false;
    }
    if (!remove_directory_entries(directory, CleanupPolicy::FailFast,
            [&](const char* child) { return remove_entry_at(directory_fd, child, root_device, error); }, error)) {
        const std::error_code saved = error;
        ::closedir(directory);
        error = saved;
        return false;
    }
    if (::closedir(directory) != 0) {
        assign_errno(error);
        return false;
    }
    struct stat final_status{};
    if (::fstatat(parent_fd, name, &final_status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) { return true; }
        assign_errno(error);
        return false;
    }
    if (final_status.st_dev != entry_status.st_dev || final_status.st_ino != entry_status.st_ino) {
        assign_errno(error, ESTALE);
        return false;
    }
    if (::unlinkat(parent_fd, name, AT_REMOVEDIR) == 0 || errno == ENOENT) { return true; }
    assign_errno(error);
    return false;
}
}  // namespace
void remove_path_recursively_best_effort(const std::filesystem::path& path) noexcept {
    try {
        // Preserve this entry's admission policy: dangling symlinks are left alone,
        // and mounted directories and symlinked ancestors are permitted.
        std::error_code error;
        if (!std::filesystem::exists(path, error) || error) return;
        struct stat status{};
        if (::lstat(path.c_str(), &status) != 0) return;
        if (!S_ISDIR(status.st_mode)) {
            static_cast<void>(::unlink(path.c_str()));
            return;
        }
        DIR* directory = ::opendir(path.c_str());
        if (directory != nullptr) {
            static_cast<void>(remove_directory_entries(directory, CleanupPolicy::BestEffort,
                [&](const char* child) noexcept {
                    try { remove_path_recursively_best_effort(path / child); return true; }
                    catch (...) { return false; }
                }, error));
            static_cast<void>(::closedir(directory));
        }
        static_cast<void>(::rmdir(path.c_str()));
    } catch (...) {
        // Best-effort destruction must never escape through a resource owner.
    }
}
bool remove_tree_no_follow(const std::filesystem::path& path, std::error_code& error) noexcept {
    error.clear();
    try {
        if (path.empty()) {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        const std::filesystem::path name = path.filename();
        if (name.empty() || name == "." || name == "..") {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
        const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent_fd < 0) {
            if (errno == ENOENT) { return true; }
            assign_errno(error);
            return false;
        }
        const ScopedFd parent_directory(parent_fd);
        struct stat parent_status{};
        if (::fstat(parent_directory.get(), &parent_status) != 0) {
            assign_errno(error);
            return false;
        }
        return remove_entry_at(parent_directory.get(), name.c_str(), parent_status.st_dev, error);
    } catch (const std::filesystem::filesystem_error& exception) { error = exception.code(); } catch (const std::bad_alloc&) {
        error = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) { error = std::make_error_code(std::errc::io_error); }
    return false;
}
}  // namespace mmltk::common::io
