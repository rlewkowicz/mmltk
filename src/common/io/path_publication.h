#pragma once
#include <filesystem>
#include <system_error>
namespace mmltk::common::io {
struct PathPublicationResult final {
 bool replaced = false;
 // Publication has committed on return. A cleanup error only means that the
 // old generation was not fully removed; it never invalidates the new output.
 std::error_code cleanup_error;
};
// Staging and destination must share a filesystem. Refuses an occupied
// destination when overwrite is false, otherwise exchanges existing paths.
// Parent-sync failure restores both paths; an impossible rollback is fatal.
// Old-generation cleanup is best effort and never follows symlinks.
PathPublicationResult publish_staged_path_atomically(const std::filesystem::path& staging, const std::filesystem::path& destination, bool overwrite = true);
}  // namespace mmltk::common::io
