#include "src/common/io/path_publication.h"
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include "src/common/io/file_memory.h"
namespace mmltk::common::io {
PathPublicationResult publish_staged_path_atomically(const std::filesystem::path& staging, const std::filesystem::path& destination, const bool overwrite) {
 // Inspect and commit absence in one syscall, including when replacement is
 // allowed: a racing occupant must participate in exchange and rollback.
 const bool moved = ::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0;
 if (!moved) {
  if (errno != EEXIST) throw errno_error("atomic no-replace publication failed", destination.string());
  if (!overwrite) throw std::runtime_error("publication destination already exists: " + destination.string());
  if (::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCHANGE) != 0) throw errno_error("atomic path exchange failed", destination.string());
 }
 try {
  sync_parent_directory(destination);
 } catch (...) {
  const auto flags = moved ? RENAME_NOREPLACE : RENAME_EXCHANGE;
  if (::syscall(SYS_renameat2, AT_FDCWD, destination.c_str(), AT_FDCWD, staging.c_str(), flags) != 0) std::terminate();
  throw;
 }
 PathPublicationResult result{.replaced = !moved};
 if (result.replaced) (void)remove_tree_no_follow(staging, result.cleanup_error);
 return result;
}
}  // namespace mmltk::common::io
