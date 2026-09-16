
#include "detail/staging_file_cleanup.h"
#include <filesystem>
#include <utility>
namespace mmltk::backend::data {
StagingFileCleanup::StagingFileCleanup(std::filesystem::path path) : path_(std::move(path)) {}
StagingFileCleanup::~StagingFileCleanup() {
    if (published_) return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}
void StagingFileCleanup::published() noexcept { published_ = true; }
}  // namespace mmltk::backend::data
