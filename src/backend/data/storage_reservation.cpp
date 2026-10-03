#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_observation.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <exception>
namespace mmltk::backend::data::detail {
namespace {
class CapacityObservation final {
 const StorageReservationPool::Observer& observer_;
 StorageReservationPool::Observation value_;
 CompileObservationClock clock_;
 int exceptions_;
public:
 CapacityObservation(const StorageReservationPool::Observer& observer, const std::filesystem::path& path, std::string_view description)
     : observer_(observer), value_{path, description, 0, 0, 0}, clock_(bool(observer)), exceptions_(observer ? std::uncaught_exceptions() : 0) {}
 ~CapacityObservation() {
  if (!observer_) return;
  value_.duration_ns = clock_.elapsed();
  value_.failed = std::uncaught_exceptions() > exceptions_;
  try { observer_(value_); } catch (...) {}
 }
 void sample() noexcept { if (observer_) ++value_.capacity_samples; }
 void capacity(std::uint64_t required, std::uint64_t outstanding, std::uint64_t available) noexcept {
  if (observer_) { value_.required = required; value_.outstanding = outstanding; value_.available = available; }
 }
};
std::filesystem::path existing_parent(const std::filesystem::path& path) {
 auto probe = std::filesystem::absolute(path).lexically_normal();
 while (!std::filesystem::is_directory(probe)) {
  const auto parent = probe.parent_path();
  if (parent == probe || parent.empty()) throw std::runtime_error("cannot locate storage filesystem");
  probe = parent;
 }
 return probe;
}
std::uint64_t available_bytes(const std::filesystem::path& probe) {
 struct statvfs status{};
 if (::statvfs(probe.c_str(), &status) != 0) throw common::io::errno_error("cannot inspect storage", probe.string());
 return common::math::checked_multiply(status.f_bavail, status.f_frsize, "available storage overflow");
}
void check_capacity(std::uint64_t available, std::uint64_t reserved, std::uint64_t required, std::string_view description) {
 if (reserved > available || required > available - reserved)
  throw InsufficientStorage("insufficient storage for " + std::string(description) + ": requires " + std::to_string(required) + " bytes with " + std::to_string(reserved) +
                            " bytes outstanding, available " + std::to_string(available));
}
}  // namespace
struct StorageReservationPool::Ledger {
 struct Filesystem {
  std::mutex mutex;
  std::uint64_t outstanding = 0;
  void replace(std::uint64_t previous, std::uint64_t required, bool initial, const std::filesystem::path& probe, std::string_view description, CapacityObservation& observation) {
   const std::lock_guard lock(mutex);
   const auto others = outstanding - previous;
   std::uint64_t available = 0;
   observation.capacity(required, others, 0);
   // New reservations observe the filesystem even when all bytes are credited.
   if (initial || required > previous) {
    // Keep every sibling promise charged across this single sample and commit.
    // Physical writes may continue, but their settlement takes this same lock.
    observation.sample();
    available = available_bytes(probe);
    observation.capacity(required, others, available);
    if (required) check_capacity(available, others, required, description);
   }
   outstanding = common::math::checked_add(others, required, "storage reservation overflow");
   if (initial || required <= previous) observation.capacity(required, outstanding, available);
  }
 };
 // Registration alone uses this mutex. Filesystem entries remain stable and
 // live until the last destination/reservation releases the shared ledger.
 std::mutex mutex;
 std::unordered_map<std::uint64_t, Filesystem> filesystems;
};
struct StorageReservationPool::Destination {
 std::filesystem::path path, probe;
 Observer observer;
 std::shared_ptr<Ledger> ledger;
 Ledger::Filesystem* filesystem = nullptr;
};
void StorageReservationPool::require_capacity(const std::filesystem::path& path, std::uint64_t required, std::string_view description, Observer observer) {
 CapacityObservation observation(observer, path, description);
 observation.capacity(required, 0, 0);
 observation.sample();
 const auto available = available_bytes(existing_parent(path));
 observation.capacity(required, 0, available);
 check_capacity(available, 0, required, description);
}
StorageReservationPool::StorageReservationPool(std::filesystem::path path, Observer observer, StorageReservationPool* shared) : destination_(std::make_shared<Destination>()) {
 destination_->probe = existing_parent(path);
 destination_->path = std::move(path);
 destination_->observer = std::move(observer);
 destination_->ledger = shared ? shared->destination_->ledger : std::make_shared<Ledger>();
 struct stat status{};
 if (::stat(destination_->probe.c_str(), &status) != 0) throw common::io::errno_error("cannot identify storage filesystem", destination_->probe.string());
 const std::lock_guard lock(destination_->ledger->mutex);
 destination_->filesystem = &destination_->ledger->filesystems.try_emplace(static_cast<std::uint64_t>(status.st_dev)).first->second;
}
StorageReservationPool::Reservation::Reservation(std::shared_ptr<Destination> destination, std::uint64_t promised, std::uint64_t allocated)
    : destination_(std::move(destination)), promised_(promised), allocated_(allocated) {}
StorageReservationPool::Reservation::Reservation(Reservation&& other) noexcept : destination_(std::move(other.destination_)), promised_(other.promised_), allocated_(other.allocated_) {}
StorageReservationPool::Reservation& StorageReservationPool::Reservation::operator=(Reservation&& other) noexcept {
 if (this != &other) {
  release();
  destination_ = std::move(other.destination_);
  promised_ = other.promised_;
  allocated_ = other.allocated_;
 }
 return *this;
}
StorageReservationPool::Reservation::~Reservation() { release(); }
std::uint64_t StorageReservationPool::Reservation::outstanding() const noexcept { return promised_ > allocated_ ? promised_ - allocated_ : 0; }
void StorageReservationPool::Reservation::release() noexcept {
 if (!destination_) return;
 {
  const std::lock_guard lock(destination_->filesystem->mutex);
  destination_->filesystem->outstanding -= outstanding();
 }
 destination_.reset();
}
std::uint64_t StorageReservationPool::allocated_bytes(int descriptor) {
 struct stat status{};
 if (::fstat(descriptor, &status) != 0) throw common::io::errno_error("cannot reconcile staging allocation");
 if (!S_ISREG(status.st_mode)) throw std::runtime_error("staging allocation is not a regular file");
 return common::math::checked_multiply(common::math::checked_cast<std::uint64_t>(status.st_blocks, "allocation overflow"), std::uint64_t{512}, "allocation overflow");
}
void StorageReservationPool::Reservation::reconcile(int descriptor) {
 if (destination_) reconcile_allocated(StorageReservationPool::allocated_bytes(descriptor));
}
void StorageReservationPool::Reservation::reconcile_allocated(std::uint64_t allocated) {
 if (!destination_) return;
 auto& fs = *destination_->filesystem;
 const std::lock_guard lock(fs.mutex);
 const auto remaining = promised_ > allocated ? promised_ - allocated : 0;
 fs.outstanding = common::math::checked_add(fs.outstanding - outstanding(), remaining, "storage reservation overflow");
 allocated_ = allocated;
}
void StorageReservationPool::Reservation::withdraw_allocation() { reconcile_allocated(0); }
void StorageReservationPool::Reservation::grow(std::uint64_t promised, std::string_view description) {
 if (promised > promised_) resize(promised, description);
}
void StorageReservationPool::Reservation::resize(std::uint64_t promised, std::string_view description) {
 if (!destination_) throw std::logic_error("cannot resize an empty storage reservation");
 CapacityObservation observation(destination_->observer, destination_->path, description);
 const auto remaining = promised > allocated_ ? promised - allocated_ : 0;
 destination_->filesystem->replace(outstanding(), remaining, false, destination_->probe, description, observation);
 promised_ = promised;
}
StorageReservationPool::Reservation StorageReservationPool::reserve(std::uint64_t promised, std::string_view description, std::uint64_t allocated) {
 CapacityObservation observation(destination_->observer, destination_->path, description);
 const auto required = promised > allocated ? promised - allocated : 0;
 destination_->filesystem->replace(0, required, true, destination_->probe, description, observation);
 return Reservation(destination_, promised, allocated);
}
std::uint64_t StorageReservationPool::outstanding() const {
 const std::lock_guard lock(destination_->filesystem->mutex);
 return destination_->filesystem->outstanding;
}
}  // namespace mmltk::backend::data::detail
