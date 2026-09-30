#include "src/backend/data/compiled/compiled_file_utils.h"
#include <cerrno>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/types.h>
namespace mmltk::backend::data {
FileLayout compact_compiled_file(const mmltk::common::io::FileHandle& file, const FileLayout& staging, std::span<const ImageEntry> entries,
 mmltk::common::concurrency::CancellationObservation cancellation, CompiledCompactionProgress progress, const std::function<void(std::size_t, std::size_t)>& reserve) {
 using namespace mmltk::common::math;
 if (entries.size() != staging.num_slots || file.size() < staging.index_offset) throw std::runtime_error("compiled compaction staging extent differs");
 const auto check_cancelled = [&] {
  if (cancellation.requested()) throw std::runtime_error("dataset compilation cancelled");
 };
 std::size_t maximum = 0, total = 0;
 for (std::size_t slot = 0; slot < entries.size(); ++slot) {
  throw_if_compiled_validation_cancelled(slot, cancellation);
  const auto& entry = entries[slot];
  if (!entry.eligible()) {
   if (entry.status != ImageStatus::Inactive || entry.label_bytes || entry.num_instances || entry.num_runs) throw std::runtime_error("invalid inactive compaction slot");
   continue;
  }
  if (entry.label_bytes != annotation_layout(entry.num_instances, entry.num_runs).used_bytes || entry.label_bytes > staging.label_capacity)
   throw std::runtime_error("compiled compaction label extent differs");
  maximum = std::max(maximum, std::size_t{entry.label_bytes});
  if (progress) total = checked_add(total, checked_add(staging.image_stride, std::size_t{entry.label_bytes}, "compiled compaction sample overflow"), "compiled compaction total overflow");
 }
 const auto compact = compute_file_layout(staging.num_slots, staging.image_stride, maximum);
 if (compact.total_size > staging.total_size) throw std::logic_error("compiled compaction cannot grow its layout");
 // Forward traversal is memmove-safe even when source and destination overlap.
 // Only live prefixes move; page padding and inactive slots remain sparse.
 constexpr std::size_t maximum_scratch = 4U * 1024U * 1024U;
 const auto scratch_bytes = compact.label_pitch == staging.label_pitch ? 0U : std::min(maximum_scratch, std::max(staging.image_stride, maximum));
 auto scratch = mmltk::common::io::OwnedBuffer<std::byte>::allocate(scratch_bytes);
 bool reclaim_supported = true;
 const auto reclaim = [&](std::size_t begin, std::size_t end) {
  if (!reclaim_supported || begin >= end) return;
  check_cancelled();
  int result;
  do {
   result = ::fallocate(
    file.get(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, checked_cast<off_t>(begin, "compiled reclaim offset overflow"), checked_cast<off_t>(end - begin, "compiled reclaim size overflow"));
  } while (result < 0 && errno == EINTR);
  if (result == 0) return;
  if (errno == EOPNOTSUPP || errno == ENOSYS) {
   reclaim_supported = false;
   return;
  }
  throw mmltk::common::io::errno_error("compiled compaction extent reclaim failed");
 };
 std::size_t retained_end = compact.data_offset, completed = 0;
 if (progress) progress(0, total);
 const auto advance = [&](std::size_t bytes) {
  if (progress) {
   completed += bytes;
   progress(completed, total);
  }
 };
 const auto move = [&](std::size_t source, std::size_t destination, std::size_t bytes) {
  check_cancelled();
  if (!bytes) return;
  if (destination > source || destination < retained_end) throw std::logic_error("compiled compaction extent order is invalid");
  reclaim(retained_end, destination);
  if (source == destination) {
   advance(bytes);
  } else {
   if (scratch.empty()) throw std::logic_error("compiled compaction scratch was not prepared");
   for (std::size_t offset = 0; offset < bytes;) {
    check_cancelled();
    const auto size = std::min(scratch.bytes(), bytes - offset);
    file.pread_all(scratch.data(), size, source + offset);
    const auto occupied = compiled_align_up(size, PAGE_SIZE);
    if (reserve)
     reserve(destination + offset, occupied);
    else
     file.allocate_range(destination + offset, occupied);
    file.pwrite_all(scratch.data(), size, destination + offset);
    // Release only fully consumed source pages beyond the destination prefix.
    // Future source reads remain intact; later destinations can reuse the holes.
    reclaim(std::max(source + offset, compiled_align_up(destination + offset + size, PAGE_SIZE)), (source + offset + size) & ~(PAGE_SIZE - 1U));
    offset += size;
    advance(size);
   }
  }
  retained_end = compiled_align_up(destination + bytes, PAGE_SIZE);
 };
 for (std::size_t block = 0; block < staging.block_count(); ++block) {
  const auto first = block * COMPILED_BLOCK_SLOTS;
  const auto end = std::min(first + COMPILED_BLOCK_SLOTS, entries.size());
  for (std::size_t image = first; image < end; ++image) {
   if (!entries[image].eligible()) continue;
   const auto slot = checked_cast<std::uint32_t>(image, "compiled compaction slot overflow");
   move(staging.pixel_offset(slot), compact.pixel_offset(slot), staging.image_stride);
  }
  for (std::size_t image = first; image < end; ++image) {
   if (!entries[image].eligible() || !entries[image].label_bytes) continue;
   const auto slot = checked_cast<std::uint32_t>(image, "compiled compaction slot overflow");
   move(staging.label_offset(slot), compact.label_offset(slot), entries[image].label_bytes);
  }
 }
 reclaim(retained_end, compact.index_offset);
 check_cancelled();
 file.resize_sparse(compact.total_size);
 return compact;
}
}  // namespace mmltk::backend::data
