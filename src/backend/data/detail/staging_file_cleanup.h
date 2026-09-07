#pragma once  // backend.data private implementation boundary

#include <filesystem>
#include <utility>

namespace mmltk::backend::data {

class StagingFileCleanup {
   public:
    explicit StagingFileCleanup(std::filesystem::path path);

    ~StagingFileCleanup();

    void published() noexcept;

   private:
    std::filesystem::path path_;
    bool published_ = false;
};

}  // namespace mmltk::backend::data
