
#include "detail/benchmark_cache.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <thread>
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::io::errno_error;
using mmltk::common::io::FileHandle;
using mmltk::common::io::sync_parent_directory;
namespace {
[[nodiscard]] std::filesystem::path require_cache_root(const std::filesystem::path& root) {
 if (root.empty()) { throw std::runtime_error("benchmark cache root must not be empty"); }
 const std::filesystem::path normalized = std::filesystem::absolute(root).lexically_normal();
 if (normalized == normalized.root_path()) { throw std::runtime_error("benchmark cache root must not be the filesystem root"); }
 return normalized;
}
}  // namespace
ArtifactLease::ArtifactLease(const int descriptor) noexcept : descriptor_(descriptor) {}
BenchmarkCacheLayout BenchmarkCacheLayout::create(const std::filesystem::path& root_path) {
 BenchmarkCacheLayout layout;
 layout.root = require_cache_root(root_path);
 layout.downloads = layout.root / "downloads";
 layout.images = layout.root / "images";
 layout.indexes = layout.root / "indexes";
 layout.locks = layout.root / "locks";
 std::filesystem::create_directories(layout.downloads);
 std::filesystem::create_directories(layout.images);
 std::filesystem::create_directories(layout.indexes);
 std::filesystem::create_directories(layout.locks);
 return layout;
}
namespace {
// Validates the source name and returns base/<source>, creating the directory when missing.
[[nodiscard]] std::filesystem::path ensured_source_subdirectory(const std::filesystem::path& base, const std::string_view source) {
 if (!is_safe_cache_component(source)) { throw std::runtime_error("invalid benchmark source cache name"); }
 const std::filesystem::path path = base / std::string(source);
 std::filesystem::create_directories(path);
 return path;
}
}  // namespace
std::filesystem::path BenchmarkCacheLayout::source_downloads(const std::string_view source) const { return ensured_source_subdirectory(downloads, source); }
std::filesystem::path BenchmarkCacheLayout::source_images(const std::string_view source) const { return ensured_source_subdirectory(images, source); }
std::filesystem::path BenchmarkCacheLayout::source_indexes(const std::string_view source) const { return ensured_source_subdirectory(indexes, source); }
ArtifactLease::ArtifactLease(ArtifactLease&& other) noexcept : descriptor_(std::move(other.descriptor_)) {}
ArtifactLease& ArtifactLease::operator=(ArtifactLease&& other) noexcept {
 if (this != &other) {
  release();
  descriptor_ = std::move(other.descriptor_);
 }
 return *this;
}
ArtifactLease::~ArtifactLease() { release(); }
ArtifactLease ArtifactLease::acquire(const std::filesystem::path& lock_path, mmltk::common::concurrency::CancellationObservation cancel_requested) {
 (void)mmltk::common::io::ensure_parent_directory(lock_path);
 const int descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
 if (descriptor < 0) { throw errno_error("cannot open benchmark cache lock", lock_path.string()); }
 mmltk::common::io::ScopedFd owned(descriptor);
 while (::flock(owned.get(), LOCK_EX | LOCK_NB) != 0) {
  if (errno != EWOULDBLOCK && errno != EAGAIN) { throw errno_error("cannot acquire benchmark cache lock", lock_path.string()); }
  throw_if_benchmark_cancelled(cancel_requested);
  // flock has no readiness fd; this bounded retry exists solely to retain cancellation responsiveness.
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
 }
 return ArtifactLease(owned.release());
}
void ArtifactLease::release() noexcept {
 if (descriptor_.get() >= 0) {
  (void)::flock(descriptor_.get(), LOCK_UN);
  descriptor_.reset();
 }
}
void throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation cancel_requested) {
 if (cancel_requested.requested()) { throw std::runtime_error("benchmark dataset compilation cancelled"); }
}
void write_json_atomically(const std::filesystem::path& path, const nlohmann::json& value, const mmltk::common::concurrency::CancellationObservation cancellation) {
 (void)mmltk::common::io::ensure_parent_directory(path);
 const std::string serialized = value.dump(2);
 std::string staging_text = path.string() + ".next.XXXXXX";
 FileHandle staging = FileHandle::create_unique_output(staging_text, serialized.size());
 staging.pwrite_all(serialized.data(), serialized.size(), 0U);
 staging.sync_data();
 staging = FileHandle{};
 const std::filesystem::path staging_path(staging_text);
 try {
  throw_if_benchmark_cancelled(cancellation);
  std::filesystem::rename(staging_path, path);
  sync_parent_directory(path);
 } catch (...) {
  std::error_code ignored;
  std::filesystem::remove(staging_path, ignored);
  throw;
 }
}
nlohmann::json read_json_file(const std::filesystem::path& path) {
 std::ifstream input(path);
 if (!input.is_open()) { throw std::runtime_error("cannot open benchmark cache metadata: " + path.string()); }
 try {
  return nlohmann::json::parse(input);
 } catch (const nlohmann::json::exception& error) { throw std::runtime_error("invalid benchmark cache metadata " + path.string() + ": " + error.what()); }
}
bool is_safe_cache_component(const std::string_view value) noexcept {
 if (value.empty() || value == "." || value == "..") { return false; }
 for (const char character : value) {
  const bool valid =
   (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.';
  if (!valid) { return false; }
 }
 return true;
}
void remove_cache_path(const std::filesystem::path& path) {
 std::error_code error;
 (void)std::filesystem::remove(path, error);
 if (error) { throw std::filesystem::filesystem_error("cannot invalidate cached benchmark image", path, error); }
}
}  // namespace mmltk::backend::data::benchmark_internal
