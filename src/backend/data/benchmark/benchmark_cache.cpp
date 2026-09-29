
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include <cerrno>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include "src/common/io/file_memory.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::io::errno_error;
namespace common_io = mmltk::common::io;
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
ArtifactLease::ArtifactLease(ArtifactLease&& other) noexcept : allowance_(std::move(other.allowance_)), descriptor_(std::move(other.descriptor_)) {}
ArtifactLease& ArtifactLease::operator=(ArtifactLease&& other) noexcept {
 if (this != &other) {
  release();
  allowance_ = std::move(other.allowance_);
  descriptor_ = std::move(other.descriptor_);
 }
 return *this;
}
ArtifactLease::~ArtifactLease() { release(); }
namespace {
enum class LeaseWait { Once, UntilAcquired };
[[nodiscard]] common_io::ScopedFd acquire_physical_lease(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation, LeaseWait wait) {
 throw_if_benchmark_cancelled(cancellation);
 (void)common_io::ensure_parent_directory(path);
 common_io::ScopedFd descriptor(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644));
 if (descriptor.get() < 0) throw errno_error("cannot open benchmark cache lock", path.string());
 while (::flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
  if (errno != EWOULDBLOCK && errno != EAGAIN) throw errno_error("cannot acquire benchmark cache lock", path.string());
  if (wait == LeaseWait::Once) return {};
  throw_if_benchmark_cancelled(cancellation);
  // flock has no readiness fd. Only ordinary blocking callers retain this fd.
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
 }
 throw_if_benchmark_cancelled(cancellation);
 return descriptor;
}
}  // namespace
ArtifactLease ArtifactLease::acquire(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation) {
 return ArtifactLease(acquire_physical_lease(path, cancellation, LeaseWait::UntilAcquired).release());
}
std::shared_ptr<ArtifactLease> ArtifactLease::acquire_charged(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkCompilePipeline* execution,
 BenchmarkResources demand, const BenchmarkAllowance& parent) {
 if (!demand.descriptors) throw std::invalid_argument("benchmark lease requires a descriptor allowance");
 for (;;) {
  auto allowance = execution ? execution->reserve(demand, parent) : BenchmarkAllowance{};
  if (auto lease = try_acquire(path, cancellation, std::move(allowance))) return lease;
  throw_if_benchmark_cancelled(cancellation);
  // Contended flock owns no admitted descriptor while its controller waits.
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
 }
}
std::shared_ptr<ArtifactLease> ArtifactLease::acquire_charged(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkAllowance admitted) {
 if (admitted && !admitted.descriptors()) throw std::invalid_argument("benchmark lease envelope has no descriptor");
 auto result = std::make_shared<ArtifactLease>();
 result->allowance_ = std::move(admitted);
 // Assign only the physical handle: custody must precede open and survive it.
 auto physical = acquire(path, cancellation);
 result->descriptor_ = std::move(physical.descriptor_);
 return result;
}
std::shared_ptr<ArtifactLease> ArtifactLease::try_acquire_charged(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkAllowance admitted) {
 if (admitted && !admitted.descriptors()) throw std::invalid_argument("benchmark lease allowance has no descriptor");
 return try_acquire(path, cancellation, std::move(admitted));
}
std::shared_ptr<ArtifactLease> ArtifactLease::try_acquire_charged(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation,
 BenchmarkCompilePipeline* execution, BenchmarkResources demand, const BenchmarkAllowance& parent) {
 if (!demand.descriptors) throw std::invalid_argument("benchmark lease requires a descriptor allowance");
 throw_if_benchmark_cancelled(cancellation);
 BenchmarkAllowance allowance;
 if (execution) {
  execution->require_feasible(demand);
  auto admitted = execution->try_reserve(demand, parent);
  if (!admitted) return {};
  allowance = std::move(*admitted);
 }
 return try_acquire(path, cancellation, std::move(allowance));
}
std::shared_ptr<ArtifactLease> ArtifactLease::try_acquire(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkAllowance allowance) {
 auto descriptor = acquire_physical_lease(path, cancellation, LeaseWait::Once);
 if (descriptor.get() < 0) return {};
 auto result = std::make_shared<ArtifactLease>();
 result->allowance_ = std::move(allowance);
 result->descriptor_ = std::move(descriptor);
 return result;
}
void ArtifactLease::release() noexcept {
 if (descriptor_.get() >= 0) {
  (void)::flock(descriptor_.get(), LOCK_UN);
  descriptor_.reset();
 }
 allowance_.retire_descriptors();
 allowance_.retire_workspace();
 allowance_ = {};
}
void throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation cancel_requested) {
 if (cancel_requested.requested()) { throw std::runtime_error("benchmark dataset compilation cancelled"); }
}
bool is_benchmark_capacity_failure(const std::exception& error) noexcept {
 return dynamic_cast<const std::bad_alloc*>(&error) != nullptr || dynamic_cast<const std::length_error*>(&error) != nullptr || dynamic_cast<const std::overflow_error*>(&error) != nullptr;
}
std::uint64_t write_json_atomically(
 const std::filesystem::path& path, const nlohmann::json& value, const mmltk::common::concurrency::CancellationObservation cancellation, StorageReservationPool* storage) {
 (void)common_io::ensure_parent_directory(path);
 const std::string serialized = value.dump(2);
 StorageReservationPool destination(path, {}, storage);
 auto staging = BenchmarkStagedArtifact::create(destination, path, serialized.size(), "benchmark metadata staging", ".next.XXXXXX");
 staging.preallocate(serialized.size());
 staging.file().pwrite_all(serialized.data(), serialized.size(), 0U);
 staging.file().sync_data();
 staging.publish(path, cancellation, BenchmarkStagedArtifact::Publication::RenameAndSync);
 return serialized.size();
}
nlohmann::json read_json_file(const std::filesystem::path& path, std::uint64_t* extent) {
 std::ifstream input(path, extent ? std::ios::in | std::ios::ate : std::ios::in);
 if (!input.is_open()) { throw std::runtime_error("cannot open benchmark cache metadata: " + path.string()); }
 if (extent) {
  const auto end = input.tellg();
  if (end < 0) throw std::runtime_error("cannot inspect opened benchmark cache metadata: " + path.string());
  *extent = static_cast<std::uint64_t>(end);
  input.seekg(0);
  if (!input) throw std::runtime_error("cannot rewind benchmark cache metadata: " + path.string());
 }
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
