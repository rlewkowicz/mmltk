#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include <sys/statvfs.h>
#include "src/pch_linux.h"
#include "src/pch_std.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::uint64_t available_bytes(const std::filesystem::path& path) {
 std::filesystem::path probe = std::filesystem::absolute(path).lexically_normal();
 while (!std::filesystem::exists(probe)) {
  const std::filesystem::path parent = probe.parent_path();
  if (parent == probe || parent.empty()) { throw std::runtime_error("cannot locate an existing parent for storage preflight"); }
  probe = parent;
 }
 struct statvfs status{};
 if (::statvfs(probe.c_str(), &status) != 0) { throw common_io::errno_error("cannot inspect benchmark storage", probe.string()); }
 return common_math::checked_multiply(status.f_bavail, status.f_frsize, "available storage byte count overflow");
}
}  // namespace
std::uint64_t additional_download_bytes(const std::filesystem::path& destination, const std::uint64_t expected) {
 std::uint64_t allocated = 0;
 for (const auto& path : {destination, std::filesystem::path(destination.string() + ".part")}) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
   if (errno == ENOENT) continue;
   throw common_io::errno_error("cannot inspect retained download storage", path.string());
  }
  if (S_ISREG(status.st_mode))
   allocated = std::max(allocated, common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks, "download allocation overflow"), 512U, "download allocation overflow"));
 }
 return expected > allocated ? expected - allocated : 0U;
}
void require_storage(const std::filesystem::path& path, const std::uint64_t required, const char* description, const BenchmarkTraceSink& trace) {
 const std::uint64_t available = available_bytes(path);
 trace_benchmark_event(
  trace, "benchmark.storage.preflight", [&] { return nlohmann::json{{"target", description}, {"path", path.string()}, {"required_bytes", required}, {"available_bytes", available}}; });
 if (available < required) {
  throw InsufficientBenchmarkStorage(std::string("insufficient storage for ") + description + ": requires " + std::to_string(required) + " bytes, available " + std::to_string(available));
 }
}
struct StorageReservationPool::Entry {
 std::uint64_t device = 0, promised = 0, allocated = 0;
 struct Watched { std::filesystem::path path; std::uint64_t baseline; };
 std::vector<Watched> watched;
};
struct StorageReservationPool::Ledger {
 mutable std::mutex mutex;
 std::vector<std::weak_ptr<Entry>> entries;
 std::uint64_t outstanding(std::uint64_t device) {
  std::uint64_t result = 0;
  std::erase_if(entries, [](const auto& entry) { return entry.expired(); });
  for (const auto& weak : entries) {
   const auto entry = weak.lock();
   if (!entry || entry->device != device) continue;
   std::uint64_t grown = 0;
   for (const auto& watched : entry->watched) {
    struct stat status{};
    const auto result = ::stat(watched.path.c_str(), &status);
    if (result == 0) {
     if (!S_ISREG(status.st_mode)) throw std::runtime_error("benchmark staging allocation is not a regular file");
     const auto bytes = common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks, "benchmark allocation overflow"), 512U, "benchmark allocation overflow");
     // Alternate names of one download generation (.part/final) count once.
     grown = std::max(grown, bytes > watched.baseline ? bytes - watched.baseline : 0U);
    } else if (errno != ENOENT) throw common_io::errno_error("cannot reconcile benchmark staging allocation", watched.path.string());
   }
   const auto allocated = std::max(entry->allocated, grown);
   result = common_math::checked_add(result, entry->promised > allocated ? entry->promised - allocated : 0, "benchmark storage reservation overflow");
  }
  return result;
 }
};
StorageReservationPool::Reservation::Reservation(std::shared_ptr<Ledger> ledger, std::shared_ptr<Entry> entry) : ledger_(std::move(ledger)), entry_(std::move(entry)) {}
StorageReservationPool::Reservation::Reservation(Reservation&&) noexcept = default;
StorageReservationPool::Reservation& StorageReservationPool::Reservation::operator=(Reservation&&) noexcept = default;
StorageReservationPool::Reservation::~Reservation() = default;
void StorageReservationPool::Reservation::release() noexcept { entry_.reset(); ledger_.reset(); }
void StorageReservationPool::Reservation::allocated(std::uint64_t bytes) {
 if (!entry_) return;
 const std::lock_guard lock(ledger_->mutex);
 entry_->allocated = std::max(entry_->allocated, bytes);
}
void StorageReservationPool::Reservation::watch(const std::filesystem::path& path, std::uint64_t baseline) {
 if (!entry_) return;
 const std::lock_guard lock(ledger_->mutex);
 entry_->watched.push_back({path, baseline});
}
StorageReservationPool::StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace, StorageReservationPool* compile)
 : path_(std::move(path)), trace_(std::move(trace)), ledger_(compile ? compile->ledger_ : std::make_shared<Ledger>()) {
 auto probe = std::filesystem::absolute(path_);
 while (!std::filesystem::exists(probe)) {
  const auto parent = probe.parent_path();
  if (parent == probe || parent.empty()) throw std::runtime_error("cannot locate benchmark storage filesystem");
  probe = parent;
 }
 struct stat status{};
 if (::stat(probe.c_str(), &status) != 0) throw common_io::errno_error("cannot identify benchmark storage filesystem", probe.string());
 device_ = static_cast<std::uint64_t>(status.st_dev);
}
StorageReservationPool::Reservation StorageReservationPool::reserve(std::uint64_t required, std::string_view description) {
 auto entry = std::make_shared<Entry>();
 entry->device = device_;
 entry->promised = required;
 std::uint64_t reserved, available;
 {
  const std::lock_guard lock(ledger_->mutex);
  // Reconcile first, then observe free bytes, so already allocated extents are
  // never deliberately subtracted twice. Concurrent unrelated writers can
  // still consume free space; actual writes retain their fatal I/O outcomes.
  reserved = ledger_->outstanding(device_);
  available = available_bytes(path_);
  if (reserved > available || required > available - reserved)
   throw InsufficientBenchmarkStorage("insufficient storage for " + std::string(description) + ": requires " + std::to_string(required) + " bytes with " + std::to_string(reserved) +
    " bytes outstanding, available " + std::to_string(available));
  ledger_->entries.push_back(entry);
 }
 trace_benchmark_event(trace_, "benchmark.storage.reserved", [&] {
  return nlohmann::json{{"target", description}, {"path", path_.string()}, {"required_bytes", required}, {"reserved_bytes", reserved + required}, {"available_bytes", available}};
 });
 return Reservation(ledger_, std::move(entry));
}
StorageReservationPool::Reservation StorageReservationPool::reserve_download(const std::filesystem::path& path, std::uint64_t expected, std::string_view description) {
 const auto additional = additional_download_bytes(path, expected);
 auto result = reserve(additional, description);
 {
  const std::lock_guard lock(ledger_->mutex);
  // Promise the complete resulting allocation, crediting the currently
  // retained inode. If validation removes/truncates that inode, its promise
  // becomes outstanding again at the same time free space returns. A fixed
  // initial baseline would lose the replacement promise in that interval.
  result.entry_->watched.push_back({path, 0});
  result.entry_->watched.push_back({path.string() + ".part", 0});
  result.entry_->promised = expected;
 }
 return result;
}
std::uint64_t StorageReservationPool::outstanding() const {
 const std::lock_guard lock(ledger_->mutex);
 return ledger_->outstanding(device_);
}
}  // namespace mmltk::backend::data::benchmark_internal
