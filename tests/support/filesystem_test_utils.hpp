#pragma once

#include "../../src/filesystem_utils.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmltk::testsupport {

using mmltk::filesystem_utils::remove_path_recursively_best_effort;

class ScopedTempDir {
   public:
    explicit ScopedTempDir(const char* name_prefix) {
        std::string pattern =
            (std::filesystem::temp_directory_path() / (std::string(name_prefix) + ".XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path_ = std::filesystem::path(created);
    }

    ~ScopedTempDir() {
        if (!path_.empty()) {
            remove_path_recursively_best_effort(path_);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

   private:
    std::filesystem::path path_;
};

}  // namespace mmltk::testsupport
