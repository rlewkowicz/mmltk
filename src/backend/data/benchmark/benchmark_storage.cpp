#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
std::filesystem::path existing_parent(const std::filesystem::path& path) {
 auto probe = std::filesystem::absolute(path).lexically_normal();
 // Anchor repeated capacity observations to a directory that survives target
 // replacement; a retained final file may be removed before its retry grows.
 while (!std::filesystem::is_directory(probe)) {
  const auto parent = probe.parent_path();
  if (parent == probe || parent.empty()) throw std::runtime_error("cannot locate benchmark storage filesystem");
  probe = parent;
 }
 return probe;
}
std::uint64_t available_bytes(const std::filesystem::path& probe) {
 struct statvfs status{};
 if (::statvfs(probe.c_str(), &status) != 0) throw common_io::errno_error("cannot inspect benchmark storage", probe.string());
 return common_math::checked_multiply(status.f_bavail, status.f_frsize, "available storage byte count overflow");
}
std::uint64_t allocated_bytes(const struct stat& status) {
 if (!S_ISREG(status.st_mode)) throw std::runtime_error("benchmark staging allocation is not a regular file");
 return common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks, "benchmark allocation overflow"), 512U, "benchmark allocation overflow");
}
std::uint64_t download_allocation(const std::filesystem::path& destination) {
 std::uint64_t allocated = 0;
 for (const auto& path : {destination, std::filesystem::path(destination.string() + ".part")}) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
   if (errno == ENOENT) continue;
   throw common_io::errno_error("cannot inspect retained download storage", path.string());
  }
  if (S_ISREG(status.st_mode)) allocated = std::max(allocated, allocated_bytes(status));
 }
 return allocated;
}
void require_capacity(std::uint64_t available, std::uint64_t reserved, std::uint64_t required, std::string_view description) {
 if (reserved > available || required > available - reserved)
  throw InsufficientBenchmarkStorage("insufficient storage for " + std::string(description) + ": requires " + std::to_string(required) + " bytes with " + std::to_string(reserved) +
                                     " bytes outstanding, available " + std::to_string(available));
}
}  // namespace
std::uint64_t additional_download_bytes(const std::filesystem::path& destination, std::uint64_t expected) {
 const auto allocated = download_allocation(destination);
 return expected > allocated ? expected - allocated : 0;
}
void require_storage(const std::filesystem::path& path, std::uint64_t required, const char* description, const BenchmarkTraceSink& trace) {
 const auto available = available_bytes(existing_parent(path));
 trace_benchmark_event(
  trace, "benchmark.storage.preflight", [&] { return nlohmann::json{{"target", description}, {"path", path.string()}, {"required_bytes", required}, {"available_bytes", available}}; });
 require_capacity(available, 0, required, description);
}
struct StorageReservationPool::Ledger {
 std::mutex mutex;
 std::unordered_map<std::uint64_t, std::uint64_t> outstanding;
};
struct StorageReservationPool::Destination {
 std::filesystem::path path, probe;
 BenchmarkTraceSink trace;
 std::shared_ptr<Ledger> ledger;
 std::uint64_t* total = nullptr;  // unordered_map references survive rehash; ledger outlives this view.
};
StorageReservationPool::Reservation::Reservation(std::shared_ptr<Destination> destination, std::uint64_t promised, std::uint64_t allocated)
    : destination_(std::move(destination)), promised_(promised), allocated_(allocated), fully_allocated_(allocated >= promised) {}
StorageReservationPool::Reservation::Reservation(Reservation&& other) noexcept
    : destination_(std::move(other.destination_)), promised_(other.promised_), allocated_(other.allocated_), fully_allocated_(other.fully_allocated_.load(std::memory_order_relaxed)) {}
StorageReservationPool::Reservation& StorageReservationPool::Reservation::operator=(Reservation&& other) noexcept {
 if (this != &other) {
  release();
  destination_ = std::move(other.destination_);
  promised_ = other.promised_;
  allocated_ = other.allocated_;
  fully_allocated_.store(other.fully_allocated_.load(std::memory_order_relaxed), std::memory_order_relaxed);
 }
 return *this;
}
StorageReservationPool::Reservation::~Reservation() { release(); }
std::uint64_t StorageReservationPool::Reservation::outstanding() const noexcept { return promised_ > allocated_ ? promised_ - allocated_ : 0; }
void StorageReservationPool::Reservation::release() noexcept {
 if (!destination_) return;
 {
  const std::lock_guard lock(destination_->ledger->mutex);
  *destination_->total -= outstanding();
 }
 destination_.reset();
}
void StorageReservationPool::Reservation::settle_locked(std::uint64_t allocated) {
 const auto remaining = promised_ > allocated ? promised_ - allocated : 0;
 *destination_->total = common_math::checked_add(*destination_->total - outstanding(), remaining, "benchmark storage reservation overflow");
 allocated_ = allocated;
 fully_allocated_.store(remaining == 0, std::memory_order_relaxed);
}
void StorageReservationPool::Reservation::reconcile(int descriptor) {
 // Known full allocation needs no repeated fstat on every mapped pixel write.
 // All operations that can shrink backing withdraw this fact before mutation.
 if (!destination_ || fully_allocated_.load(std::memory_order_relaxed)) return;
 const std::lock_guard lock(destination_->ledger->mutex);
 struct stat status{};
 if (::fstat(descriptor, &status) != 0) throw common_io::errno_error("cannot reconcile benchmark staging allocation");
 settle_locked(allocated_bytes(status));
}
void StorageReservationPool::Reservation::reconcile_download(const std::filesystem::path& path) {
 if (!destination_) return;
 const std::lock_guard lock(destination_->ledger->mutex);
 settle_locked(download_allocation(path));
}
void StorageReservationPool::Reservation::withdraw_allocation() {
 if (!destination_) return;
 const std::lock_guard lock(destination_->ledger->mutex);
 settle_locked(0);
}
void StorageReservationPool::Reservation::grow(std::uint64_t promised, std::string_view description) {
 if (promised > promised_) resize(promised, description);
}
void StorageReservationPool::Reservation::resize(std::uint64_t promised, std::string_view description) {
 if (!destination_) throw std::logic_error("cannot resize an empty benchmark storage reservation");
 const std::lock_guard lock(destination_->ledger->mutex);
 const auto remaining = promised > allocated_ ? promised - allocated_ : 0;
 const auto others = *destination_->total - outstanding();
 require_capacity(available_bytes(destination_->probe), others, remaining, description);
 *destination_->total = common_math::checked_add(others, remaining, "benchmark storage reservation overflow");
 promised_ = promised;
 fully_allocated_.store(remaining == 0, std::memory_order_relaxed);
}
StorageReservationPool::StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace, StorageReservationPool* compile) : destination_(std::make_shared<Destination>()) {
 destination_->probe = existing_parent(path);
 destination_->path = std::move(path);
 destination_->trace = std::move(trace);
 destination_->ledger = compile ? compile->destination_->ledger : std::make_shared<Ledger>();
 struct stat status{};
 if (::stat(destination_->probe.c_str(), &status) != 0) throw common_io::errno_error("cannot identify benchmark storage filesystem", destination_->probe.string());
 const std::lock_guard lock(destination_->ledger->mutex);
 destination_->total = &destination_->ledger->outstanding.try_emplace(static_cast<std::uint64_t>(status.st_dev), 0).first->second;
}
StorageReservationPool::Reservation StorageReservationPool::reserve_backing(std::uint64_t promised, std::string_view description, const std::filesystem::path* download) {
 Reservation result;
 std::uint64_t available, total, required;
 {
  const std::lock_guard lock(destination_->ledger->mutex);
  const auto allocated = download ? download_allocation(*download) : 0;
  required = promised > allocated ? promised - allocated : 0;
  available = available_bytes(destination_->probe);
  require_capacity(available, *destination_->total, required, description);
  total = common_math::checked_add(*destination_->total, required, "benchmark storage reservation overflow");
  result = Reservation(destination_, promised, allocated);
  *destination_->total = total;
 }
 trace_benchmark_event(destination_->trace, "benchmark.storage.reserved",
  [&] { return nlohmann::json{{"target", description}, {"path", destination_->path.string()}, {"required_bytes", required}, {"reserved_bytes", total}, {"available_bytes", available}}; });
 return result;
}
StorageReservationPool::Reservation StorageReservationPool::reserve(std::uint64_t required, std::string_view description) { return reserve_backing(required, description, nullptr); }
StorageReservationPool::Reservation StorageReservationPool::reserve_download(const std::filesystem::path& path, std::uint64_t expected, std::string_view description) {
 return reserve_backing(expected, description, &path);
}
std::uint64_t StorageReservationPool::outstanding() const {
 const std::lock_guard lock(destination_->ledger->mutex);
 return *destination_->total;
}
}  // namespace mmltk::backend::data::benchmark_internal
