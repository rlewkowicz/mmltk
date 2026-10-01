#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include <optional>
#include <cerrno>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/types.h>
namespace mmltk::backend::data {
FileLayout compact_compiled_file(const mmltk::common::io::FileHandle& file, const FileLayout& staging, std::span<const ImageEntry> entries,
 mmltk::common::concurrency::CancellationObservation cancellation, CompiledCompactionProgress progress, CompiledStorageMutation mutation, std::span<std::byte> supplied_scratch) {
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
 const auto scratch_bytes = compact.label_pitch == staging.label_pitch ? 0U : std::min(COMPILED_COMPACTION_SCRATCH_BYTES, compiled_align_up(std::max(staging.image_stride, maximum), PAGE_SIZE));
 std::optional<common::system::NumaMemory> owned;
 if (scratch_bytes && supplied_scratch.empty()) {
  owned.emplace(common::system::NumaTopology::Capture().permitted_nodes.front(), common::system::NumaMemory::Lock::Pages);
  owned->ensure_bytes(scratch_bytes);
  supplied_scratch = {static_cast<std::byte*>(owned->data()), owned->capacity_bytes()};
 }
 if (supplied_scratch.size() < scratch_bytes) throw std::length_error("compiled compaction scratch is too small");
 auto scratch = supplied_scratch.first(scratch_bytes);
 bool reclaim_supported = true;
 const auto reclaim = [&](std::size_t begin, std::size_t end) {
  if (!reclaim_supported || begin >= end) return;
  check_cancelled();
  reclaim_supported = mutation.reclaim ? mutation.reclaim(begin, end - begin) : file.punch_hole(begin, end - begin);
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
    const auto size = std::min(scratch.size(), bytes - offset);
    file.pread_all(scratch.data(), size, source + offset);
    const auto occupied_begin = (destination + offset) / PAGE_SIZE * PAGE_SIZE;
    const auto occupied_end = compiled_align_up(destination + offset + size, PAGE_SIZE);
    if (mutation.allocate)
     mutation.allocate(occupied_begin, occupied_end - occupied_begin);
    else
     file.allocate_range(occupied_begin, occupied_end - occupied_begin);
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
 struct LiveExtent {
  std::size_t source = 0, destination = 0, bytes = 0;
 } pending;
 const auto append = [&](std::size_t source, std::size_t destination, std::size_t bytes) {
  if (pending.bytes && pending.source + pending.bytes == source && pending.destination + pending.bytes == destination) {
   pending.bytes = checked_add(pending.bytes, bytes, "compiled live extent overflow");
   return;
  }
  if (pending.bytes) move(pending.source, pending.destination, pending.bytes);
  pending = {source, destination, bytes};
 };
 for (std::size_t block = 0; block < staging.block_count(); ++block) {
  const auto first = block * COMPILED_BLOCK_SLOTS;
  const auto end = std::min(first + COMPILED_BLOCK_SLOTS, entries.size());
  for (std::size_t image = first; image < end; ++image) {
   if (!entries[image].eligible()) continue;
   const auto slot = checked_cast<std::uint32_t>(image, "compiled compaction slot overflow");
   append(staging.pixel_offset(slot), compact.pixel_offset(slot), staging.image_stride);
  }
  for (std::size_t image = first; image < end; ++image) {
   if (!entries[image].eligible() || !entries[image].label_bytes) continue;
   const auto slot = checked_cast<std::uint32_t>(image, "compiled compaction slot overflow");
   append(staging.label_offset(slot), compact.label_offset(slot), entries[image].label_bytes);
  }
 }
 if (pending.bytes) move(pending.source, pending.destination, pending.bytes);
 reclaim(retained_end, compact.index_offset);
 check_cancelled();
 if (mutation.shrink)
  mutation.shrink(compact.total_size);
 else
  file.resize_sparse(compact.total_size);
 return compact;
}
}  // namespace mmltk::backend::data
