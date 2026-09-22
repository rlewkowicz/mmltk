#include "src/frameworks/transport/tests/support/browser_assets.h"
namespace mmltk::testsupport {
BrowserAssetDirectory::BrowserAssetDirectory(const char* const name_prefix) : root_(name_prefix) { write_text_file(root_.path() / "index.html", "<!doctype html>"); }
const std::filesystem::path& BrowserAssetDirectory::path() const noexcept { return root_.path(); }
}  // namespace mmltk::testsupport
