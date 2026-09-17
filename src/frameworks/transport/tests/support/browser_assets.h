#pragma once
#include "src/test_support/filesystem_test_utils.hpp"
namespace mmltk::testsupport {
class BrowserAssetDirectory final {
   public:
    explicit BrowserAssetDirectory(const char* name_prefix);
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

   private:
    ScopedTempDir root_;
};
}  // namespace mmltk::testsupport
