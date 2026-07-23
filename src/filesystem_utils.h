#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace mmltk::filesystem_utils {

inline void remove_path_recursively_best_effort(const std::filesystem::path& path) {
    namespace fs = std::filesystem;

    std::error_code exists_error;
    if (!fs::exists(path, exists_error) || exists_error) {
        return;
    }

    const std::string native = path.string();
    struct stat status {};
    if (::lstat(native.c_str(), &status) != 0) {
        return;
    }
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        (void)::unlink(native.c_str());
        return;
    }

    DIR* directory = ::opendir(native.c_str());
    if (directory == nullptr) {
        (void)::rmdir(native.c_str());
        return;
    }

    while (dirent* entry = ::readdir(directory)) {
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        remove_path_recursively_best_effort(path / name);
    }
    (void)::closedir(directory);
    (void)::rmdir(native.c_str());
}

}  
