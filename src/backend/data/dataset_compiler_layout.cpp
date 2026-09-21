#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/common/io/file_memory.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
// CLEANUP-IGNORE: This layout implementation imports the concrete data and system owners used by its independent unit.
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
#include "detail/dataset_compiler_internal.h"
namespace mmltk::backend::data::compiler_internal {
using mmltk::common::io::FileHandle;
using mmltk::common::system::clamp_worker_count_to_cpus;
int resolve_num_workers(int configured_workers, const std::span<const int> worker_cpus) {
 if (worker_cpus.empty()) { throw std::runtime_error("dataset compilation requires a non-empty CPU execution domain"); }
 const std::vector<int> allowed(worker_cpus.begin(), worker_cpus.end());
 if (configured_workers > 0) {
  const int clamped = clamp_worker_count_to_cpus(configured_workers, allowed.size(), 0, 1);
  if (configured_workers != clamped) {
   mmltk::common::logging::warn([&](spdlog::logger& log) {
    log.warn("compile workers clamped {}->{} for cpuset={} (reserved=0 minimum=1)", configured_workers, clamped,
             mmltk::common::system::format_cpu_list(allowed));
   });
  }
  return clamped;
 }
 return static_cast<int>(allowed.size());
}
void assign_pixel_offsets(std::vector<ImageEntry>& index, size_t pixel_offset, size_t image_stride) {
 for (size_t i = 0; i < index.size(); ++i) { index[i].pixel_offset = pixel_offset + i * image_stride; }
}
void write_metadata_blocks(const FileHandle& fd, const FileLayout& layout, const FileHeader& header, const LabelBlocks& label_blocks,
                           mmltk::common::concurrency::CancellationObservation cancel_requested) {
 constexpr size_t kWriteChunkBytes = size_t{16U} * 1024U * 1024U;
 const auto write_block = [&](const void* source, const size_t bytes, const size_t offset) {
  const auto* source_bytes = static_cast<const std::uint8_t*>(source);
  size_t written = 0U;
  while (written < bytes) {
   if (cancel_requested.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
   const size_t chunk = std::min(kWriteChunkBytes, bytes - written);
   fd.pwrite_all(source_bytes + written, chunk, offset + written);
   written += chunk;
  }
 };
 {
  mmltk::common::logging::ScopedProfile profile{"compiler.write_header"};
  write_block(&header, sizeof(header), 0);
 }
 {
  mmltk::common::logging::ScopedProfile profile{"compiler.write_index"};
  write_block(label_blocks.index.data(), layout.index_size, layout.index_offset);
 }
 {
  mmltk::common::logging::ScopedProfile profile{"compiler.write_labels"};
  write_block(label_blocks.labels.data(), layout.label_block_size, layout.label_offset);
 }
 {
  mmltk::common::logging::ScopedProfile profile{"compiler.write_rle"};
  write_block(label_blocks.rle_pairs.data(), layout.rle_block_size, layout.rle_offset);
 }
}
}  // namespace mmltk::backend::data::compiler_internal
