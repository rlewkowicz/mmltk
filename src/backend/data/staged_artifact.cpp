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
namespace {
// At most one split/insert node per normalized range. Allocate every node before
// admission or destructive mutation; map node transfer below cannot allocate.
std::map<std::size_t, std::size_t> prepare_range_nodes(std::size_t count) {
 std::map<std::size_t, std::size_t> nodes;
 for (std::size_t i = 0; i < count; ++i) nodes.emplace(i, 0);
 return nodes;
}
}
void StagedArtifact::forget_range(std::size_t begin, std::size_t end, std::map<std::size_t, std::size_t>& prepared) {
 if (begin >= end) return;
 auto& ranges = mutation_->occupied;
 auto it = ranges.lower_bound(begin);
 if (it != ranges.begin() && std::prev(it)->second > begin) --it;
 while (it != ranges.end() && it->first < end) {
  const auto first = it->first, last = it->second;
  if (first < begin) {
   it->second = begin;
   ++it;
   if (last > end) {
    auto node = prepared.extract(prepared.begin());
    node.key() = end;
    node.mapped() = last;
    ranges.insert(std::move(node));
    break;
   }
  } else if (last > end) {
   auto node = ranges.extract(it);
   node.key() = end;
   ranges.insert(std::move(node));
   break;
  } else it = ranges.erase(it);
 }
}
void StagedArtifact::occupy_range(std::size_t begin, std::size_t end, std::map<std::size_t, std::size_t>& prepared) {
 auto& ranges = mutation_->occupied;
 auto it = ranges.lower_bound(begin);
 if (it != ranges.begin() && std::prev(it)->second >= begin) --it;
 while (it != ranges.end() && it->first <= end) {
  begin = std::min(begin, it->first);
  end = std::max(end, it->second);
  it = ranges.erase(it);
 }
 auto node = prepared.extract(prepared.begin());
 node.key() = begin;
 node.mapped() = end;
 ranges.insert(std::move(node));
}
void StagedArtifact::resize_sparse(std::size_t bytes, std::uint64_t* syscalls, common::concurrency::CancellationObservation cancellation) {
 const auto occupied_end = common::math::checked_add(bytes, std::size_t{4095}, "staged page extent overflow") / 4096 * 4096;
 std::map<std::size_t, std::size_t> unused;
 const std::lock_guard lock(mutation_->mutex);
 // Withdraw credited backing before truncate can remove it. No map allocation
 // can fail after the physical operation, including when length only grows.
 allocation_.withdraw_allocation();
 file_.resize_sparse(bytes, syscalls, cancellation);
 forget_range(occupied_end, std::numeric_limits<std::size_t>::max(), unused);
 allocation_.resize(reconcile_locked(), "sparse staged allocation");
}
void StagedArtifact::allocate_range(std::size_t offset, std::size_t bytes) {
 const common_io::FileHandle::Range range{offset, bytes};
 allocate_ranges({&range, 1});
}
void StagedArtifact::allocate_ranges(std::span<const common_io::FileHandle::Range> input, common::concurrency::CancellationObservation cancellation, std::uint64_t* syscalls) {
 using namespace common::math;
 auto normalized = common_io::FileHandle::normalize_ranges(input, common_io::FileHandle::RangeCoverage::CoverPages);
 if (normalized.empty()) return;
 const std::lock_guard lock(mutation_->mutex);
 const auto& occupied = mutation_->occupied;
 std::size_t uncovered = 0, used = 0;
 for (const auto& range : normalized) {
  const auto begin = range.offset, end = range.end();
  auto missing = range.bytes;
  auto it = occupied.lower_bound(begin);
  if (it != occupied.begin() && std::prev(it)->second > begin) --it;
  for (; it != occupied.end() && it->first < end; ++it) missing -= std::min(end, it->second) - std::max(begin, it->first);
  if (!missing) continue;
  uncovered = checked_add(uncovered, missing, "staged uncovered extent overflow");
  normalized[used++] = range;
 }
 normalized.resize(used);
 if (normalized.empty()) return;
 auto prepared = prepare_range_nodes(normalized.size());
 if (cancellation.requested()) throw std::runtime_error("staged allocation cancelled");
 const auto allocated = StorageReservationPool::allocated_bytes(file_.get());
 // Exactly one growth admission covers the batch. Rejection has not changed
 // the file or its positive occupied-range knowledge. Unknown pages, including
 // raw writes and legacy full preallocation, are never assumed to be holes.
 allocation_.reserve_growth(allocated, static_cast<std::uint64_t>(uncovered), "occupied staged extent");
 // On partial failure retain the complete promise and prior known backing;
 // successful kernel work can only add backing. Retirement/reconcile settles it.
 file_.allocate_ranges(normalized, cancellation, syscalls);
 for (const auto& range : normalized) occupy_range(range.offset, range.end(), prepared);
 allocation_.resize(reconcile_locked(), "occupied staged allocation");
}
bool StagedArtifact::reclaim_range(std::size_t offset, std::size_t bytes) {
 const common_io::FileHandle::Range range{offset, bytes};
 return reclaim_ranges({&range, 1});
}
bool StagedArtifact::reclaim_ranges(std::span<const common_io::FileHandle::Range> input, common::concurrency::CancellationObservation cancellation, std::uint64_t* syscalls) {
 const auto normalized = common_io::FileHandle::normalize_ranges(input);
 if (normalized.empty()) return true;
 auto prepared = prepare_range_nodes(normalized.size());
 const std::lock_guard lock(mutation_->mutex);
 if (cancellation.requested()) throw std::runtime_error("staged reclamation cancelled");
 allocation_.withdraw_allocation();
 // Keep positive knowledge for an unsupported first punch. On an error,
 // invalidate the whole requested window conservatively (the failing syscall
 // may itself have partially changed backing) without allocating any nodes.
 const auto forget = [&](std::span<const common_io::FileHandle::Range> ranges) {
  for (const auto& range : ranges) {
   const auto first = common::math::checked_add(range.offset, std::size_t{4095}, "staged reclaim page overflow") / 4096 * 4096;
   forget_range(first, range.end() / 4096 * 4096, prepared);
  }
 };
 std::size_t completed = 0;
 bool reclaimed;
 try { reclaimed = file_.punch_holes(normalized, cancellation, syscalls, &completed); }
 catch (...) { forget(normalized); throw; }
 forget(std::span(normalized).first(completed));
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
