#pragma once

#include <filesystem>
#include <string_view>

#include "src/common/io/file_memory.h"

namespace mmltk::common::io {

// Owns a uniquely named staging directory created beside `destination`, and removes the tree again on
// destruction unless the caller marked it published(). `name_prefix` and `name_suffix` wrap the
// destination file name to build the mkdtemp template, so the suffix must end in "XXXXXX".
class StagingDirectory {
   public:
    StagingDirectory(const std::filesystem::path& destination, const std::string_view name_prefix, const std::string_view name_suffix,
                     const char* failure_action);

    ~StagingDirectory();

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    StagingDirectory(StagingDirectory&&) = delete;
    StagingDirectory& operator=(StagingDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void published() noexcept;

   private:
    std::filesystem::path path_;
    bool published_ = false;
};

}  // namespace mmltk::common::io
