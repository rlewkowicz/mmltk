#include "common_utils.h"

#include <dirent.h>

#include <cerrno>
#include <new>
#include <system_error>

namespace mmltk {

namespace {

void assign_errno(std::error_code& error, const int value = errno) noexcept {
    error.assign(value, std::generic_category());
}

[[nodiscard]] bool is_dot_entry(const char* name) noexcept {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

[[nodiscard]] bool remove_entry_at(const int parent_fd, const char* name, const dev_t root_device,
                                   std::error_code& error) noexcept {
    struct stat entry_status {};
    if (::fstatat(parent_fd, name, &entry_status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        assign_errno(error);
        return false;
    }
    if (entry_status.st_dev != root_device) {
        assign_errno(error, EXDEV);
        return false;
    }
    if (!S_ISDIR(entry_status.st_mode)) {
        if (::unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT) {
            return true;
        }
        assign_errno(error);
        return false;
    }

    const int child_fd = ::openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
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

    struct stat opened_status {};
    if (::fstat(::dirfd(directory), &opened_status) != 0) {
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

    while (true) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                const int saved_errno = errno;
                ::closedir(directory);
                assign_errno(error, saved_errno);
                return false;
            }
            break;
        }
        if (is_dot_entry(entry->d_name)) {
            continue;
        }
        if (!remove_entry_at(::dirfd(directory), entry->d_name, root_device, error)) {
            ::closedir(directory);
            return false;
        }
    }
    if (::closedir(directory) != 0) {
        assign_errno(error);
        return false;
    }
    struct stat final_status {};
    if (::fstatat(parent_fd, name, &final_status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        assign_errno(error);
        return false;
    }
    if (final_status.st_dev != entry_status.st_dev || final_status.st_ino != entry_status.st_ino) {
        assign_errno(error, ESTALE);
        return false;
    }
    if (::unlinkat(parent_fd, name, AT_REMOVEDIR) == 0 || errno == ENOENT) {
        return true;
    }
    assign_errno(error);
    return false;
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
        const std::filesystem::path parent =
            path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
        const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parent_fd < 0) {
            if (errno == ENOENT) {
                return true;
            }
            assign_errno(error);
            return false;
        }
        const UniqueFd parent_directory(parent_fd);
        struct stat parent_status {};
        if (::fstat(parent_directory.get(), &parent_status) != 0) {
            assign_errno(error);
            return false;
        }
        return remove_entry_at(parent_directory.get(), name.c_str(), parent_status.st_dev, error);
    } catch (const std::filesystem::filesystem_error& exception) {
        error = exception.code();
    } catch (const std::bad_alloc&) {
        error = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
    return false;
}

}  
