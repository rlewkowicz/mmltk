#include "src/backend/data/tests/profile_support.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/data/detail/compiled_slot_storage.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_workspace.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/common/io/file_digest.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include <cstring>
#include <utility>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
namespace mmltk::backend::data::testsupport::profile {
namespace {
using Clock = std::chrono::steady_clock;
namespace io = mmltk::common::io;
// Hash only semantic live bytes, in stable slot order. Sparse padding is never
// read into a payload buffer, either during fixture setup or verification.
void hash_extent(io::Sha256Hasher& hash, const io::FileHandle& file, std::size_t offset, std::size_t bytes, std::span<std::byte> scratch) {
 while (bytes) {
  const auto count = std::min(bytes, scratch.size());
  file.pread_all(scratch.data(), count, offset);
  hash.Update({reinterpret_cast<const std::uint8_t*>(scratch.data()), count});
  offset += count;
  bytes -= count;
 }
}
}
void run_compaction_profile(const Options& opts, const benchmark_internal::BenchmarkTraceSink& trace) {
 using namespace detail;
 using namespace benchmark_internal;
 const auto setup_start = Clock::now();
 const auto staging = profile_compaction_layout(opts); // Cap before creating or acquiring anything.
 std::filesystem::create_directories(opts.test_dir);
 StorageReservationPool pool(opts.test_dir);
 // One private iteration is retired before another starts. keep_artifacts does
 // not accumulate multi-GiB compaction fixtures or retain another output inode.
 auto stage = StagedArtifact::create(pool, std::filesystem::path(opts.test_dir) / "compaction", 0, "bounded compaction fixture");
 const auto initial = io::FileSnapshot::Read(stage.file().get());
 std::array<CompileWorkspaceProfile, static_cast<std::size_t>(CompileWorkClass::Count)> profiles{};
 auto plan = std::make_shared<CompileWorkspacePlan>(CompileWorkspacePlan::prepare(1, profiles, 0, COMPILED_COMPACTION_SCRATCH_BYTES, 16ULL << 20));
 CompileExecution execution(1, {}, {.workspace_plan = plan}, {}, observe_compile_execution(trace));
 auto output = execution.open_class(CompileWorkClass::Output);
 FileWorkQueue::Observer file_observer;
 if (trace) file_observer = {trace.selection(), [&](std::string_view event, const FileWorkQueue::Observation& value) {
  trace_benchmark_event(trace, event, [&] { return benchmark_observation_fields(value); });
 }};
 FileWorkQueue files(execution, 2, 1, std::move(file_observer));
 auto allowance = execution.reserve({.bytes = COMPILED_COMPACTION_SCRATCH_BYTES, .supplied_workspace = true, .completion_workspace = true,
  .workspace_class = CompileWorkClass::Output, .workspace_window = CompileWorkspaceWindow::Compaction});
 const auto scratch = allowance.workspace();
 std::vector<ImageEntry> entries(staging.num_slots);
 CompiledSlotStorage slots(stage, staging);
 CompiledLabelWorkspace labels;
 std::uint32_t eligible = 0, max_instances = 0;
 io::Sha256Hasher before_hash;
 for (std::uint32_t slot = 0; slot < staging.num_slots; ++slot) {
  const bool active = opts.compaction_case == CompactionCase::Dense || (slot % 7 != 1 && slot % 7 != 2);
  if (!active) continue;
  auto& entry = entries[slot];
  entry.status = ImageStatus::Eligible;
  entry.original_width = static_cast<std::uint32_t>(opts.width);
  entry.original_height = static_cast<std::uint32_t>(opts.height);
  entry.has_source_image_id = 1;
  entry.source_image_id = slot + 1;
  ++eligible;
  slots.prepare_pixels(slot);
  {
   auto pixels = slots.pixels(slot);
   for (std::size_t i = 0; i < staging.image_stride / sizeof(float); ++i) pixels.data()[i] = static_cast<float>((slot * 17 + i % 257) % 1024) / 1024.0F;
   before_hash.Update({reinterpret_cast<const std::uint8_t*>(pixels.data()), staging.image_stride});
  }
  const std::size_t instances = opts.compaction_case == CompactionCase::Sparse || slot % 5 == 0 ? 0 : opts.compaction_case == CompactionCase::MixedLabels && slot % 97 == 0 ? 8191 : 1;
  if (instances) {
   labels.prepare(0);
   for (std::size_t i = 0; i < instances; ++i) {
    PackedInstance label{};
    label.bbox_x2 = static_cast<float>(opts.width);
    label.bbox_y2 = static_cast<float>(opts.height);
    label.source_ordinal = std::uint64_t{slot} * 8192 + i;
    labels.labels.push_back(label);
   }
   // Small labels deliberately have a 72-byte prefix, including run alignment.
   if (instances == 1) {
    labels.labels[0].flags = kAnnotationMask;
    labels.labels[0].mask_rle_pairs = 1;
    labels.runs.push_back({0, 1});
   }
   set_annotation_extent(entry, instances, labels.runs.size(), "compaction profile");
   slots.labels(slot).write(entry, labels.release_output());
   hash_extent(before_hash, stage.file(), staging.label_offset(slot), entry.label_bytes, scratch);
   max_instances = std::max(max_instances, static_cast<std::uint32_t>(instances));
  }
 }
 slots.quiesce();
 stage.resize_sparse(staging.total_size);
 const auto expected_hash = before_hash.Finish();
 const auto allocated_before = StorageReservationPool::allocated_bytes(stage.file().get());
 if (opts.compaction_cold) {
  stage.file().sync_data();
  stage.file().advise(0, staging.total_size, POSIX_FADV_DONTNEED);
 }
 CompiledCompactionObserver observation;
 if (trace.accepts("profile.compaction.summary")) observation = [&](const CompiledCompactionObservation& value) {
  trace_benchmark_event(trace, "profile.compaction.summary", [&] { return benchmark_observation_fields(value); });
 };
 FileLayout final;
 Clock::time_point start, compacted, metadata, synced;
 files.submit({.work = [&] {
  start = Clock::now();
  final = compact_compiled_file(stage.file(), staging, entries, {}, {},
   CompiledStorageMutation::for_artifact(stage), scratch, observation);
  compacted = Clock::now();
  const std::array<std::string, 1> classes{"profile"};
  const auto header = make_file_header({eligible, static_cast<std::uint32_t>(opts.width), static_cast<std::uint32_t>(opts.height), 3, max_instances, staging.image_stride}, classes, final);
  const std::array ranges{io::FileHandle::Range{0, sizeof(header)}, io::FileHandle::Range{final.index_offset, final.index_size}};
  stage.allocate_ranges(ranges);
  stage.file().pwrite_all(&header, sizeof(header), 0);
  stage.file().pwrite_all(entries.data(), final.index_size, final.index_offset);
  stage.resize_sparse(final.total_size);
  metadata = Clock::now();
  stage.file().sync_data();
  synced = Clock::now();
 }}).wait();
 const auto allocated_after = StorageReservationPool::allocated_bytes(stage.file().get());
 const auto after = io::FileSnapshot::Read(stage.file().get());
 if (initial.device != after.device || initial.inode != after.inode || after.bytes != final.total_size) throw std::runtime_error("compaction profile changed inode or final extent");
 (void)validate_compiled_file_sections(read_compiled_header(stage.file()), stage.file().size());
 std::vector<ImageEntry> stored(entries.size());
 stage.file().pread_all(stored.data(), final.index_size, final.index_offset);
 if (std::memcmp(stored.data(), entries.data(), final.index_size)) throw std::runtime_error("compaction profile changed slot identity");
 io::Sha256Hasher after_hash;
 for (std::uint32_t slot = 0; slot < final.num_slots; ++slot) if (entries[slot].eligible()) {
  hash_extent(after_hash, stage.file(), final.pixel_offset(slot), final.image_stride, scratch);
  hash_extent(after_hash, stage.file(), final.label_offset(slot), entries[slot].label_bytes, scratch);
 }
 if (after_hash.Finish() != expected_hash) throw std::runtime_error("compaction profile live bytes differ");
 const auto verified = Clock::now();
 std::printf("compaction_profile slots=%u eligible=%u case=%s cold=%d staging_bytes=%zu final_bytes=%zu allocated_before=%llu allocated_after=%llu live_sha256=%s setup_sec=%.6f compaction_sec=%.6f metadata_sec=%.6f sync_sec=%.6f compaction_sync_sec=%.6f total_sec=%.6f verification_sec=%.6f\n",
  staging.num_slots, eligible, std::string(mmltk::frameworks::reflection::enum_name(opts.compaction_case)).c_str(), opts.compaction_cold, staging.total_size, final.total_size,
  static_cast<unsigned long long>(allocated_before), static_cast<unsigned long long>(allocated_after), io::sha256_hex(expected_hash).c_str(), seconds_since(setup_start, start),
  seconds_since(start, compacted), seconds_since(compacted, metadata), seconds_since(metadata, synced), seconds_since(start, synced), seconds_since(setup_start, synced), seconds_since(synced, verified));
 record_duration_metric("compaction", "setup", setup_start, start);
 record_duration_metric("compaction", "move", start, compacted);
 record_duration_metric("compaction", "sync", metadata, synced);
 record_duration_metric("compaction", "total", start, synced);
 allowance = {};
 stage = {};
 if (pool.outstanding()) throw std::runtime_error("compaction profile retained storage promises");
}
}  // namespace mmltk::backend::data::testsupport::profile
