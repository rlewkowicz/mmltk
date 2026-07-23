#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

[[nodiscard]] bool path_exists(const char* path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw std::filesystem::filesystem_error("failed to inspect browser distribution path", path, error);
    }
    return exists;
}

void publish(const char* staged_path, const char* current_path) {
    if (!path_exists(staged_path) || !std::filesystem::is_directory(staged_path)) {
        throw std::runtime_error("staged browser distribution is missing or is not a directory");
    }

    if (!path_exists(current_path)) {
        if (::rename(staged_path, current_path) != 0) {
            throw std::runtime_error(std::string("initial browser distribution rename failed: ") +
                                     std::strerror(errno));
        }
        return;
    }

    if (::syscall(SYS_renameat2, AT_FDCWD, staged_path, AT_FDCWD, current_path, RENAME_EXCHANGE) != 0) {
        throw std::runtime_error(std::string("atomic browser distribution exchange failed: ") + std::strerror(errno));
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(staged_path, cleanup_error);
    if (cleanup_error) {
        std::cerr << "mmltk_browser_dist_publish: published bundle but could not remove previous bundle at "
                  << staged_path << ": " << cleanup_error.message() << '\n';
    }
}

}  

int main(const int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: mmltk_browser_dist_publish STAGED_PATH CURRENT_PATH\n";
        return 2;
    }

    try {
        publish(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mmltk_browser_dist_publish: " << error.what() << " (staged=" << std::string_view(argv[1])
                  << ", current=" << std::string_view(argv[2]) << ")\n";
        return 1;
    }
}
