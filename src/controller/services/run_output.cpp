#include "run_output.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/fs.h>
#include "src/common/io/staging_directory.h"
namespace mmltk::controller::services {
std::filesystem::path reserve_run_output(const std::filesystem::path& selected, bool use_empty_root) {
 if (selected.empty()) throw std::invalid_argument("run output directory is empty");
 const auto root = std::filesystem::absolute(selected).lexically_normal();
 if (use_empty_root && (!std::filesystem::exists(root) || std::filesystem::is_empty(root))) {
  std::filesystem::create_directories(root);
  if (std::filesystem::create_directory(root / ".mmltk-run-claim")) return root;
 }
 std::filesystem::create_directories(root);
 std::uint64_t next = 1;
 for (const auto& entry : std::filesystem::directory_iterator(root)) {
  const auto name = entry.path().filename().string();
  if (!name.starts_with("run-")) continue;
  std::uint64_t suffix = 0;
  const auto parsed = std::from_chars(name.data() + 4, name.data() + name.size(), suffix);
  if (parsed.ptr != name.data() + name.size()) continue;
  if (parsed.ec == std::errc::result_out_of_range) throw std::runtime_error("run output suffix exhausted");
  if (parsed.ec != std::errc{}) continue;
  if (suffix == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("run output suffix exhausted");
  next = std::max(next, suffix + 1);
 }
 // Publish the claim and directory together, so selecting a still-empty
 // numbered run manually cannot win a race with its original allocator.
 mmltk::common::io::StagingDirectory staged(root / "run", ".", "-claim-XXXXXX", "stage run reservation");
 std::filesystem::create_directory(staged.path() / ".mmltk-run-claim");
 for (; next != std::numeric_limits<std::uint64_t>::max(); ++next) {
  const auto candidate = root / std::format("run-{:04}", next);
  if (::renameat2(AT_FDCWD, staged.path().c_str(), AT_FDCWD, candidate.c_str(), RENAME_NOREPLACE) == 0) {
   staged.published();
   return candidate;
  }
  const std::error_code error{errno, std::generic_category()};
  if (error != std::errc::file_exists) throw std::filesystem::filesystem_error("cannot reserve run output", candidate, error);
 }
 throw std::runtime_error("cannot allocate a fresh run output directory");
}
}  // namespace mmltk::controller::services
