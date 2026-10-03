#include "src/backend/data/detail/staged_artifact.h"
#include "src/common/io/path_publication.h"
#include "src/common/math/checked_arithmetic.h"
#include <unistd.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/falloc.h>
#include <algorithm>
#include <limits>
#include <sys/random.h>
#include <sys/stat.h>
#include <utility>
#include <stdexcept>
namespace mmltk::backend::data::detail {
namespace common_io = mmltk::common::io;
StagedArtifact StagedArtifact::create(
 StorageReservationPool& destination, const std::filesystem::path& path, std::uint64_t promised, std::string_view description, std::string_view suffix, mode_t mode, int directory) {
 StagedArtifact result;
 result.mutation_ = std::make_unique<Mutation>();
 result.temporary_ = path.string() + std::string(suffix);
 result.allocation_ = destination.reserve(promised, description);
 result.directory_ = directory;
 if (directory < 0)
  result.file_ = common_io::FileHandle::create_unique_output(result.temporary_, 0, mode);
 else {
  if (!result.temporary_.ends_with("XXXXXX")) throw std::invalid_argument("directory-relative staging requires a unique suffix");
  constexpr std::string_view alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (;;) {
   std::array<unsigned char, 6> random{};
   std::size_t used = 0;
   while (used < random.size()) {
    const auto count = ::getrandom(random.data() + used, random.size() - used, 0);
    if (count < 0) {
     if (errno == EINTR) continue;
     throw common_io::errno_error("cannot obtain staged filename randomness");
    }
    if (!count) throw std::runtime_error("empty staged filename randomness");
    used += static_cast<std::size_t>(count);
   }
   for (std::size_t i = 0; i < random.size(); ++i) result.temporary_[result.temporary_.size() - random.size() + i] = alphabet[random[i] % alphabet.size()];
   const int descriptor = ::openat(directory, result.temporary_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (descriptor >= 0) {
    result.file_ = common_io::FileHandle(descriptor);
    break;
   }
   if (errno != EEXIST) throw common_io::errno_error("cannot create directory-relative staged artifact", result.temporary_);
  }
 }
 // No allocating operation may intervene before cleanup owns the native name.
 result.cleanup_ = true;
 if (directory >= 0 && mode != 0600 && ::fchmod(result.file_.get(), mode) != 0) throw common_io::errno_error("cannot set staged artifact mode");
 result.path_ = result.temporary_;
 return result;
}
StagedArtifact::StagedArtifact(StagedArtifact&& other) noexcept
    : mutation_(std::move(other.mutation_)),
      allocation_(std::move(other.allocation_)),
      path_(std::move(other.path_)),
      temporary_(std::move(other.temporary_)),
      cleanup_(std::exchange(other.cleanup_, false)),
      directory_(other.directory_),
      file_(std::move(other.file_)) {}
StagedArtifact& StagedArtifact::operator=(StagedArtifact&& other) noexcept {
 if (this != &other) {
  discard();
  mutation_ = std::move(other.mutation_);
  allocation_ = std::move(other.allocation_);
  path_ = std::move(other.path_);
  temporary_ = std::move(other.temporary_);
  cleanup_ = std::exchange(other.cleanup_, false);
  directory_ = other.directory_;
  file_ = std::move(other.file_);
 }
 return *this;
}
StagedArtifact::~StagedArtifact() { discard(); }
void StagedArtifact::discard() noexcept {
 file_ = {};
 if (cleanup_) {
  (void)::unlinkat(directory_ < 0 ? AT_FDCWD : directory_, temporary_.c_str(), 0);
  cleanup_ = false;
 }
 allocation_.release();
}
std::uint64_t StagedArtifact::reconcile_locked() {
 // The existing promise remains charged while fstat runs. Its settlement joins
 // the filesystem transaction only after the physical observation completes.
 const auto allocated = StorageReservationPool::allocated_bytes(file_.get());
 allocation_.reconcile_allocated(allocated);
 return allocated;
}
void StagedArtifact::preallocate(std::size_t bytes) {
 const std::lock_guard lock(mutation_->mutex);
 allocation_.grow(bytes, "staged allocation");
 allocation_.withdraw_allocation();
 mutation_->occupied.clear();
 file_.preallocate(bytes);
 (void)reconcile_locked();
}
void StagedArtifact::resize(std::size_t bytes, std::string_view description) {
 const std::lock_guard lock(mutation_->mutex);
 (void)reconcile_locked();
 allocation_.resize(bytes, description);
 allocation_.withdraw_allocation();
 mutation_->occupied.clear();
 file_.resize_sparse(bytes);
 if (bytes) file_.preallocate(bytes);
 (void)reconcile_locked();
}
void StagedArtifact::forget_range(std::size_t begin, std::size_t end) {
 auto& ranges = mutation_->occupied;
 auto it = ranges.lower_bound(begin);
 if (it != ranges.begin() && std::prev(it)->second > begin) --it;
 while (it != ranges.end() && it->first < end) {
  const auto first = it->first, last = it->second;
  it = ranges.erase(it);
  if (first < begin) ranges.emplace(first, begin);
  if (last > end) {
   ranges.emplace(end, last);
   break;
  }
 }
}
void StagedArtifact::resize_sparse(std::size_t bytes) {
 const std::lock_guard lock(mutation_->mutex);
 // Withdraw credited backing before truncate can remove it. Keep that promise
 // charged until reconciliation, including when sparse length only grows.
 allocation_.withdraw_allocation();
 file_.resize_sparse(bytes);
 const auto occupied_end = common::math::checked_add(bytes, std::size_t{4095}, "staged page extent overflow") / 4096 * 4096;
 forget_range(occupied_end, std::numeric_limits<std::size_t>::max());
 allocation_.resize(reconcile_locked(), "sparse staged allocation");
}
void StagedArtifact::allocate_range(std::size_t offset, std::size_t bytes) {
 if (!bytes) return;
 using namespace common::math;
 constexpr std::size_t page = 4096;
 const auto begin = offset / page * page;
 const auto end = checked_add(checked_add(offset, bytes, "staged extent overflow"), page - 1, "staged page extent overflow") / page * page;
 const std::lock_guard lock(mutation_->mutex);
 auto& ranges = mutation_->occupied;
 auto it = ranges.upper_bound(begin);
 if (it != ranges.begin()) {
  auto previous = std::prev(it);
  if (previous->second >= end) return;
 }
 // Charge only uncovered pages. All callers share this inode mutation owner,
 // including compaction destinations reusing already-backed source pages.
 std::size_t uncovered = end - begin;
 it = ranges.lower_bound(begin);
 if (it != ranges.begin() && std::prev(it)->second > begin) --it;
 for (auto scan = it; scan != ranges.end() && scan->first < end; ++scan) uncovered -= std::min(end, scan->second) - std::max(begin, scan->first);
 const auto allocated = StorageReservationPool::allocated_bytes(file_.get());
 allocation_.reconcile_allocated(allocated);
 allocation_.resize(checked_add(allocated, static_cast<std::uint64_t>(uncovered), "staged growth overflow"), "occupied staged extent");
 // Allocate the bookkeeping node before physical mutation; on a syscall failure
 // invalidate it and retain the outstanding promise until cleanup/reconciliation.
 auto first = begin, last = end;
 it = ranges.lower_bound(begin);
 if (it != ranges.begin() && std::prev(it)->second >= begin) --it;
 while (it != ranges.end() && it->first <= last) {
  first = std::min(first, it->first);
  last = std::max(last, it->second);
  it = ranges.erase(it);
 }
 ranges.emplace(first, last);
 try {
  file_.allocate_range(begin, end - begin);
 } catch (...) {
  forget_range(begin, end);
  throw;
 }
 allocation_.resize(reconcile_locked(), "occupied staged allocation");
}
bool StagedArtifact::reclaim_range(std::size_t offset, std::size_t bytes) {
 if (!bytes) return true;
 const std::lock_guard lock(mutation_->mutex);
 const auto end = common::math::checked_add(offset, bytes, "staged reclaim overflow");
 allocation_.withdraw_allocation();
 const auto reclaimed = file_.punch_hole(offset, bytes);
 if (reclaimed) {
  const auto first = common::math::checked_add(offset, std::size_t{4095}, "staged reclaim page overflow") / 4096 * 4096;
  const auto last = end / 4096 * 4096;
  if (first < last) forget_range(first, last);
 }
 allocation_.resize(reconcile_locked(), "reclaimed staged allocation");
 return reclaimed;
}
void StagedArtifact::reconcile() {
 const std::lock_guard lock(mutation_->mutex);
 (void)reconcile_locked();
}
void StagedArtifact::close() {
 if (file_.get() >= 0) {
  reconcile();
  file_ = {};
 }
}
void StagedArtifact::publish(const std::filesystem::path& destination, mmltk::common::concurrency::CancellationObservation cancellation, Publication publication, bool overwrite) {
 // Publication retires this backing immediately. Keep its remaining promise
 // until rename succeeds instead of inspecting an allocation about to retire.
 file_ = {};
 if (cancellation.requested()) throw std::runtime_error("dataset compilation cancelled");
 if (directory_ >= 0) {
  if (publication != Publication::Rename || !overwrite) throw std::logic_error("directory-relative staging requires rename publication");
  if (::renameat(directory_, temporary_.c_str(), directory_, destination.c_str()) != 0) throw common_io::errno_error("cannot publish directory-relative staged artifact", destination.string());
 } else if (publication == Publication::DurableReplace)
  common_io::publish_staged_path_atomically(path_, destination, overwrite);
 else {
  std::filesystem::rename(path_, destination);
  if (publication == Publication::RenameAndSync) common_io::sync_parent_directory(destination);
 }
 cleanup_ = false;
 allocation_.release();
}
}  // namespace mmltk::backend::data::detail
