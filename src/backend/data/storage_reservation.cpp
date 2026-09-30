#include "src/backend/data/detail/storage_reservation.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
namespace mmltk::backend::data::detail {
namespace {
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
}
struct StorageReservationPool::Ledger {
 struct Filesystem { std::uint64_t outstanding = 0, revision = 0; };
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
 const auto available=available_bytes(existing_parent(path));
 if (observer) observer({path,description,required,0,available});
 check_capacity(available,0,required,description);
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
StorageReservationPool::Reservation::Reservation(Reservation&& other) noexcept
 : destination_(std::move(other.destination_)), promised_(other.promised_), allocated_(other.allocated_) {}
StorageReservationPool::Reservation& StorageReservationPool::Reservation::operator=(Reservation&& other) noexcept {
 if (this != &other) { release(); destination_ = std::move(other.destination_); promised_ = other.promised_; allocated_ = other.allocated_; }
 return *this;
}
StorageReservationPool::Reservation::~Reservation() { release(); }
std::uint64_t StorageReservationPool::Reservation::outstanding() const noexcept { return promised_ > allocated_ ? promised_ - allocated_ : 0; }
void StorageReservationPool::Reservation::release() noexcept {
 if (!destination_) return;
 { const std::lock_guard lock(destination_->ledger->mutex); destination_->filesystem->outstanding -= outstanding(); ++destination_->filesystem->revision; }
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
 const std::lock_guard lock(destination_->ledger->mutex);
 auto& fs = *destination_->filesystem;
 const auto remaining = promised_ > allocated ? promised_ - allocated : 0;
 fs.outstanding = common::math::checked_add(fs.outstanding - outstanding(), remaining, "storage reservation overflow");
 allocated_ = allocated; ++fs.revision;
}
void StorageReservationPool::Reservation::withdraw_allocation() { reconcile_allocated(0); }
void StorageReservationPool::Reservation::grow(std::uint64_t promised, std::string_view description) { if (promised > promised_) resize(promised, description); }
void StorageReservationPool::Reservation::resize(std::uint64_t promised, std::string_view description) {
 if (!destination_) throw std::logic_error("cannot resize an empty storage reservation");
 auto& fs = *destination_->filesystem;
 {
  const std::lock_guard lock(destination_->ledger->mutex);
  const auto remaining=promised > allocated_ ? promised-allocated_ : 0;
  if (remaining<=outstanding()) {
   fs.outstanding-=outstanding()-remaining;
   promised_=promised; ++fs.revision; return;
  }
 }
 for (;;) {
  std::uint64_t revision;
  { const std::lock_guard lock(destination_->ledger->mutex); revision = fs.revision; }
  const auto available = available_bytes(destination_->probe);
  const std::lock_guard lock(destination_->ledger->mutex);
  // Allocation promises remain visible throughout the observation. Any ledger
  // settlement invalidates this sample, including zero-growth punch/truncate.
  if (revision != fs.revision) continue;
  const auto remaining = promised > allocated_ ? promised - allocated_ : 0;
  const auto others = fs.outstanding - outstanding();
  if (remaining > outstanding()) check_capacity(available, others, remaining, description);
  fs.outstanding = common::math::checked_add(others, remaining, "storage reservation overflow");
  promised_ = promised; ++fs.revision; return;
 }
}
StorageReservationPool::Reservation StorageReservationPool::reserve(std::uint64_t promised, std::string_view description, std::uint64_t allocated) {
 auto& fs = *destination_->filesystem;
 for (;;) {
  std::uint64_t revision;
  { const std::lock_guard lock(destination_->ledger->mutex); revision = fs.revision; }
  const auto available = available_bytes(destination_->probe);
  const auto required = promised > allocated ? promised - allocated : 0;
  std::uint64_t total;
  {
   const std::lock_guard lock(destination_->ledger->mutex);
   if (revision != fs.revision) continue;
   if (required) check_capacity(available, fs.outstanding, required, description);
   total = common::math::checked_add(fs.outstanding, required, "storage reservation overflow");
   fs.outstanding = total; ++fs.revision;
  }
  Reservation result(destination_, promised, allocated);
  if (destination_->observer) destination_->observer({destination_->path, description, required, total, available});
  return result;
 }
}
std::uint64_t StorageReservationPool::outstanding() const { const std::lock_guard lock(destination_->ledger->mutex); return destination_->filesystem->outstanding; }
}
