#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/data/detail/compile_observation.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/staged_artifact.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include <array>
#include <exception>
#include <optional>
namespace mmltk::backend::data {
CompiledStorageMutation CompiledStorageMutation::for_artifact(detail::StagedArtifact& artifact) {
 return {[&artifact](auto ranges, auto cancel, auto* calls) { artifact.allocate_ranges(ranges, cancel, calls); }, [&artifact](auto ranges, auto cancel, auto* calls) {
  return artifact.reclaim_ranges(ranges, cancel, calls);
 }, [&artifact](std::size_t bytes, auto cancel, auto* calls) { artifact.resize_sparse(bytes, calls, cancel); }};
}
namespace {
class CompactionObservation final {
 const CompiledCompactionObserver& observer_;
 detail::CompileObservationClock clock_{true};
 int exceptions_ = std::uncaught_exceptions();

public:
 CompiledCompactionObservation value;
 explicit CompactionObservation(const CompiledCompactionObserver& observer) : observer_(observer) {}
 ~CompactionObservation() {
  value.duration_ns = clock_.elapsed();
  value.failed = std::uncaught_exceptions() > exceptions_;
  try {
   observer_(value);
  } catch (...) {}
 }
};
class MutationClock final {
 std::uint64_t* elapsed_;
 detail::CompileObservationClock clock_;

public:
 explicit MutationClock(std::uint64_t* elapsed) : elapsed_(elapsed), clock_(elapsed != nullptr) {}
 ~MutationClock() {
  if (elapsed_) *elapsed_ += clock_.elapsed();
 }
};
}  // namespace
FileLayout compact_compiled_file(const common::io::FileHandle& file, const FileLayout& staging, std::span<const ImageEntry> entries, common::concurrency::CancellationObservation cancellation,
 CompiledCompactionProgress progress, CompiledStorageMutation mutation, std::span<std::byte> supplied_scratch, CompiledCompactionObserver observer) {
 using namespace common::math;
 using Range = common::io::FileHandle::Range;
 std::optional<CompactionObservation> observation;
 if (observer) observation.emplace(observer);
 auto* const facts = observation ? &observation->value : nullptr;
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
  if (entry.label_bytes < annotation_layout(entry.num_instances, entry.num_runs).used_bytes || entry.label_bytes % sizeof(RLEPair) || entry.label_bytes > staging.label_capacity)
   throw std::runtime_error("compiled compaction label extent differs");
  maximum = std::max(maximum, std::size_t{entry.label_bytes});
  if (progress || facts) total = checked_add(total, checked_add(staging.image_stride, std::size_t{entry.label_bytes}, "compiled compaction sample overflow"), "compiled compaction total overflow");
 }
 if (facts) facts->live_bytes = total;
 const auto compact = compute_file_layout(staging.num_slots, staging.image_stride, maximum, staging.catalog_bytes);
 if (compact.total_size > staging.total_size) throw std::logic_error("compiled compaction cannot grow its layout");
 const auto prior_window = compact.label_pitch == staging.label_pitch ? 0U : std::min(COMPILED_COMPACTION_SCRATCH_BYTES, compiled_align_up(std::max(staging.image_stride, maximum), PAGE_SIZE));
 std::optional<common::system::NumaMemory> owned;
 if (prior_window && supplied_scratch.empty()) {
  owned.emplace(common::system::NumaTopology::Capture().permitted_nodes.front(), common::system::NumaMemory::Lock::Pages);
  supplied_scratch = owned->ensure_storage(prior_window);
 }
 // Existing callers' smaller legal scratch remains sufficient. An admitted
 // larger lease is a maximum capture window, never a new resource minimum.
 if (supplied_scratch.size() < prior_window) throw std::length_error("compiled compaction scratch is too small");
 auto scratch = supplied_scratch.first(prior_window ? std::min(supplied_scratch.size(), COMPILED_COMPACTION_SCRATCH_BYTES) : 0);
 bool reclaim_supported = true;
 const auto reclaim = [&](std::span<const Range> ranges) {
  if (!reclaim_supported || ranges.empty()) return;
  check_cancelled();
  if (facts) {
   ++facts->reclaim_calls;
   facts->reclaim_ranges += ranges.size();
  }
  MutationClock clock(facts ? &facts->reclaim_ns : nullptr);
  auto* calls = facts ? &facts->reclaim_syscalls : nullptr;
  reclaim_supported = mutation.reclaim ? mutation.reclaim(ranges, cancellation, calls) : file.punch_holes(ranges, cancellation, calls);
  if (facts) facts->reclaim_supported = reclaim_supported;
 };
 const auto reclaim_between = [&](std::size_t begin, std::size_t end) {
  begin = compiled_align_up(begin, PAGE_SIZE);
  end = end / PAGE_SIZE * PAGE_SIZE;
  if (begin < end) {
   const Range range{begin, end - begin};
   reclaim({&range, 1});
  }
 };
 const auto allocate = [&](std::span<const Range> ranges) {
  if (ranges.empty()) return;
  check_cancelled();
  if (facts) {
   ++facts->allocation_calls;
   facts->allocation_ranges += ranges.size();
  }
  MutationClock clock(facts ? &facts->allocation_ns : nullptr);
  auto* calls = facts ? &facts->allocation_syscalls : nullptr;
  if (mutation.allocate)
   mutation.allocate(ranges, cancellation, calls);
  else
   file.allocate_ranges(ranges, cancellation, calls);
 };
 const auto read = [&](std::size_t source, std::size_t offset, std::size_t bytes) {
  check_cancelled();
  if (facts) {
   ++facts->read_calls;
   facts->scratch_high_water = std::max<std::uint64_t>(facts->scratch_high_water, offset + bytes);
  }
  MutationClock clock(facts ? &facts->read_ns : nullptr);
  file.pread_all(scratch.data() + offset, bytes, source, facts ? &facts->read_syscalls : nullptr, cancellation);
  if (facts) facts->read_bytes += bytes;
 };
 const auto write = [&](std::size_t destination, std::size_t offset, std::size_t bytes) {
  check_cancelled();
  if (facts) ++facts->write_calls;
  MutationClock clock(facts ? &facts->write_ns : nullptr);
  file.pwrite_all(scratch.data() + offset, bytes, destination, facts ? &facts->write_syscalls : nullptr, cancellation);
  if (facts) {
   facts->write_bytes += bytes;
   facts->moved_bytes += bytes;
  }
 };
 const auto destination_range = [](std::size_t destination, std::size_t bytes) {
  const auto begin = destination / PAGE_SIZE * PAGE_SIZE;
  return Range{begin, compiled_align_up(destination + bytes, PAGE_SIZE) - begin};
 };
 std::size_t retained_end = compact.data_offset, retained_live_end = compact.data_offset, completed = 0;
 if (progress) progress(0, total);
 const auto advance = [&](std::size_t bytes) {
  if (progress) {
   completed += bytes;
   progress(completed, total);
  }
 };
 // Capture a complete contiguous source window before any overlapping write.
 // On pressure, the already-captured window can be committed at the old safe
 // operation size, reclaiming each consumed prefix before admitting the next.
 const auto move_pixels = [&](std::size_t source, std::size_t destination, std::size_t bytes) {
  check_cancelled();
  if (destination > source || destination < retained_live_end) throw std::logic_error("compiled compaction extent order is invalid");
  reclaim_between(retained_end, destination);
  if (source == destination) {
   if (facts) facts->stationary_bytes += bytes;
   advance(bytes);
  } else {
   if (scratch.empty()) throw std::logic_error("compiled compaction scratch was not prepared");
   for (std::size_t offset = 0; offset < bytes;) {
    const auto size = std::min(scratch.size(), bytes - offset);
    read(source + offset, 0, size);
    const auto full = destination_range(destination + offset, size);
    bool admitted = false;
    try {
     allocate({&full, 1});
     admitted = true;
    } catch (const detail::InsufficientStorage&) {
     if (size <= prior_window) throw;
     if (facts) ++facts->capacity_fallbacks;
    }
    for (std::size_t committed = 0; committed < size;) {
     const auto count = admitted ? size : std::min(prior_window, size - committed);
     if (!admitted) {
      const auto part = destination_range(destination + offset + committed, count);
      allocate({&part, 1});
     }
     write(destination + offset + committed, committed, count);
     const auto end = offset + committed + count;
     reclaim_between(std::max(source + offset + committed, compiled_align_up(destination + end, PAGE_SIZE)), source + end);
     committed += count;
     advance(count);
    }
    offset += size;
   }
  }
  retained_live_end = destination + bytes;
  retained_end = compiled_align_up(retained_live_end, PAGE_SIZE);
 };
 struct CapturedLabel {
  std::size_t source, destination, bytes, scratch_offset;
 };
 std::array<CapturedLabel, COMPILED_BLOCK_SLOTS> labels{};
 std::size_t label_count = 0, captured_bytes = 0;
 const auto flush_labels = [&](std::size_t unread_source) {
  if (!label_count) return;
  // All prefixes in this source window are captured before destination gap
  // punches. Even early-block gaps can overlap a preceding captured source.
  std::array<Range, COMPILED_BLOCK_SLOTS> destinations{}, gaps{};
  std::size_t destination_count = 0, gap_count = 0, committed_end = retained_end, window_bytes = 0;
  for (const auto& label : std::span(labels).first(label_count)) {
   check_cancelled();
   if (label.source + label.bytes > unread_source || label.destination > label.source || label.destination < committed_end) throw std::logic_error("compiled label extent order is invalid");
   if (label.source != label.destination) {
    read(label.source, label.scratch_offset, label.bytes);
    destinations[destination_count++] = destination_range(label.destination, label.bytes);
   }
   if (committed_end < label.destination) gaps[gap_count++] = {committed_end, label.destination - committed_end};
   committed_end = compiled_align_up(label.destination + label.bytes, PAGE_SIZE);
   window_bytes += label.bytes;
  }
  bool admitted = false;
  try {
   allocate(std::span(destinations).first(destination_count));
   admitted = true;
  } catch (const detail::InsufficientStorage&) {
   if (facts) ++facts->capacity_fallbacks;
  }
  // Admission rejection itself has not touched a source. Captured bytes stay
  // available while safe destination gaps are reclaimed, so even a single
  // rejected prefix can retry with the same headroom as the previous mover.
  reclaim(std::span(gaps).first(gap_count));
  const auto source_begin = labels[0].source;
  for (const auto& label : std::span(labels).first(label_count)) {
   if (label.source == label.destination) {
    if (facts) facts->stationary_bytes += label.bytes;
   } else {
    if (!admitted) {
     const auto part = destination_range(label.destination, label.bytes);
     allocate({&part, 1});
    }
    write(label.destination, label.scratch_offset, label.bytes);
   }
   retained_live_end = label.destination + label.bytes;
   retained_end = compiled_align_up(retained_live_end, PAGE_SIZE);
   if (!admitted) reclaim_between(std::max(source_begin, retained_end), label.source + label.bytes);
   if (!admitted) advance(label.bytes);
  }
  if (admitted) advance(window_bytes);
  if (admitted) reclaim_between(std::max(source_begin, retained_end), labels[label_count - 1].source + labels[label_count - 1].bytes);
  label_count = captured_bytes = 0;
 };
 for (std::size_t block = 0; block < staging.block_count(); ++block) {
  const auto first = block * COMPILED_BLOCK_SLOTS;
  const auto end = std::min(first + COMPILED_BLOCK_SLOTS, entries.size());
  for (std::size_t image = first; image < end;) {
   if (!entries[image].eligible()) {
    ++image;
    continue;
   }
   const auto start = image++;
   while (staging.pixel_pitch == staging.image_stride && image < end && entries[image].eligible()) ++image;
   const auto slot = checked_cast<std::uint32_t>(start, "compiled compaction slot overflow");
   move_pixels(staging.pixel_offset(slot), compact.pixel_offset(slot), checked_multiply(image - start, staging.image_stride, "compiled pixel extent overflow"));
  }
  for (std::size_t image = first; image < end; ++image) {
   if (!entries[image].eligible() || !entries[image].label_bytes) continue;
   const auto slot = checked_cast<std::uint32_t>(image, "compiled compaction slot overflow");
   const auto source = staging.label_offset(slot), destination = compact.label_offset(slot), bytes = std::size_t{entries[image].label_bytes};
   const auto capture = source == destination ? 0 : bytes;
   if (capture > scratch.size() - captured_bytes) flush_labels(source);
   if (capture > scratch.size()) throw std::logic_error("compiled label capture exceeds scratch");
   labels[label_count++] = {source, destination, bytes, captured_bytes};
   captured_bytes += capture;
  }
  flush_labels(staging.block_offset(block + 1));
 }
 reclaim_between(retained_end, compact.index_offset);
 check_cancelled();
 if (facts) ++facts->truncate_calls;
 {
  MutationClock clock(facts ? &facts->truncate_ns : nullptr);
  auto* calls = facts ? &facts->truncate_syscalls : nullptr;
  if (mutation.shrink)
   mutation.shrink(compact.total_size, cancellation, calls);
  else
   file.resize_sparse(compact.total_size, calls, cancellation);
 }
 return compact;
}
}  // namespace mmltk::backend::data
