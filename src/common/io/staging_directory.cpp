#include "src/common/io/staging_directory.h"
#include <cstdlib>
#include <string>
#include <vector>
#include "src/common/io/file_memory.h"
namespace mmltk::common::io {
StagingDirectory::StagingDirectory(const std::filesystem::path& destination, const std::string_view prefix, const std::string_view suffix,
                                   const char* const failure_action) {
    const std::filesystem::path parent = mmltk::common::io::ensure_parent_directory(destination);
    const std::string pattern = (parent / (std::string(prefix) + destination.filename().string() + std::string(suffix))).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (::mkdtemp(writable.data()) == nullptr) throw errno_error(failure_action, pattern);
    path_ = writable.data();
}
StagingDirectory::~StagingDirectory() {
    if (!published_) {
        std::error_code ignored;
        static_cast<void>(remove_tree_no_follow(path_, ignored));
    }
}
const std::filesystem::path& StagingDirectory::path() const noexcept { return path_; }
void StagingDirectory::published() noexcept { published_ = true; }
}  // namespace mmltk::common::io
