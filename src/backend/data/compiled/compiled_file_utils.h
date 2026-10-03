#pragma once
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <memory>
#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <span>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/common/types/cancellation_observation.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
struct CompiledFileSections {
 size_t index_offset = 0;
 size_t expected_index_bytes = 0;
 FileLayout layout;
};
struct CompiledDatasetInfo {
 std::filesystem::path path;
 std::uint32_t image_count = 0;
 std::uint32_t width = 0;
 std::uint32_t height = 0;
 std::uint32_t channels = 0;
 std::uint32_t max_instances_per_image = 0;
 std::shared_ptr<const catalog::ClassCatalog> class_catalog;
 [[nodiscard]] std::span<const std::string> class_names() const noexcept { return class_catalog ? class_catalog->names() : std::span<const std::string>{}; }
};
inline void validate_compiled_header(const FileHeader& header) {
 if (header.magic != MAGIC) { throw std::runtime_error("Bad magic in compiled file"); }
 if (header.version != FORMAT_VERSION) {
  throw std::runtime_error("compiled dataset format " + std::to_string(header.version) + " is unsupported; expected format " + std::to_string(FORMAT_VERSION) + "; recompile the dataset");
 }
 if (!std::ranges::all_of(header._reserved, [](std::uint8_t value) { return value == 0U; })) throw std::runtime_error("compiled header reserved fields are invalid");
 if (header.num_images == 0U || header.num_images > header.num_slots || header.image_width == 0U || header.image_height == 0U || header.channels == 0U) {
  throw std::runtime_error("compiled file has invalid image dimensions");
 }
 if (header.num_classes == 0U || header.num_classes > MAX_CLASSES) { throw std::runtime_error("compiled file has invalid class count"); }
 if (header.max_instances_per_image > std::numeric_limits<std::uint16_t>::max()) { throw std::runtime_error("compiled file maximum instance count exceeds the index representation"); }
 if (header.max_label_bytes > COMPILED_LABEL_CAPACITY || header.max_label_bytes % sizeof(RLEPair) || annotation_layout(header.max_instances_per_image, 0).used_bytes > header.max_label_bytes)
  throw std::runtime_error("compiled maximum label size is invalid");
 if (header.image_width > MAX_IMAGE_EXTENT || header.image_height > MAX_IMAGE_EXTENT || header.channels != 3U ||
     (header.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Stretch && header.resize_mode != mmltk::backend::imaging::resample::ImageResizeMode::Letterbox))
  throw std::runtime_error("compiled image extent, channels or resize mode is invalid");
 const uint64_t expected_image_stride = static_cast<uint64_t>(header.image_width) * header.image_height * header.channels * sizeof(float);
 if (header.image_stride != expected_image_stride) { throw std::runtime_error("compiled file has an invalid image stride"); }
}
inline FileHeader read_compiled_header(const mmltk::common::io::FileHandle& file) {
 FileHeader header{};
 file.pread_all(&header, sizeof(header), 0);
 validate_compiled_header(header);
 return header;
}
inline FileHeader read_compiled_header(const std::string& path) {
 const mmltk::common::io::FileHandle file = mmltk::common::io::FileHandle::open_readonly(path);
 return read_compiled_header(file);
}
// Aborts compiled-file validation loops when cancellation is requested; the flag is sampled every
// 4096 iterations so hot loops stay cheap.
inline void throw_if_compiled_validation_cancelled(const size_t index, mmltk::common::concurrency::CancellationObservation cancel_requested) {
 if ((index & 4095U) == 0U && cancel_requested.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
}
inline CompiledFileSections validate_compiled_file_sections(const FileHeader& header, size_t file_size) {
 validate_compiled_header(header);
 const auto layout = compute_file_layout(header.num_slots, mmltk::common::math::checked_cast<size_t>(header.image_stride, "image stride overflow"), header.max_label_bytes);
 if (header.total_file_size != file_size || header.total_file_size != layout.total_size || header.index_offset != layout.index_offset || header.data_offset != layout.data_offset)
  throw std::runtime_error("compiled fixed slot layout or file size is invalid");
 return {layout.index_offset, layout.index_size, layout};
}
// Field checks have one owner. Producers use these while copying/encoding;
// mapped readers use the fused admission below before exposing bulk views.
class CompiledRecordChecks final {
public:
 static void image(const ImageEntry& entry, std::size_t position = 0) {
  if (!entry.original_width || !entry.original_height || entry.has_source_image_id > 1U || entry.source > AnnotationSource::CoconutObjects365V2 ||
      (!entry.has_source_image_id && entry.source_image_id) || !std::ranges::all_of(entry._reserved, [](auto value) { return value == 0; }))
   throw std::runtime_error("compiled original image dimensions are invalid at image " + std::to_string(position));
  if (entry.source != AnnotationSource::Generic && entry.has_source_image_id != 1U) throw std::runtime_error("compiled benchmark image lacks source identity");
 }
 [[nodiscard]] static std::size_t label(const PackedInstance& instance, std::uint32_t classes, std::size_t rle_bytes, std::size_t expected, AnnotationSource source, std::size_t position = 0) {
  if (instance.class_id >= classes) throw std::runtime_error("compiled instance class id is out of bounds at label " + std::to_string(position));
  if (!std::isfinite(instance.bbox_x1) || !std::isfinite(instance.bbox_y1) || !std::isfinite(instance.bbox_x2) || !std::isfinite(instance.bbox_y2) || instance.bbox_x2 <= instance.bbox_x1 ||
      instance.bbox_y2 <= instance.bbox_y1 || !std::isfinite(instance.original_area) || instance.original_area < 0.0 || (instance.flags & ~kAnnotationFlags) ||
      (!instance.has_mask() && instance.mask_rle_pairs) || (!instance.has_annotation_id() && instance.annotation_id) || (!instance.has_source_category() && instance.source_category_id))
   throw std::runtime_error("compiled instance annotation metadata is invalid at label " + std::to_string(position));
  if (!instance.has_complete_source_ordinal() && source != AnnotationSource::CoconutCoco && source != AnnotationSource::CoconutObjects365V1 && source != AnnotationSource::CoconutObjects365V2)
   throw std::runtime_error("incomplete source ordinal outside COCONut at label " + std::to_string(position));
  if (source != AnnotationSource::Generic) {
   if (!instance.has_source_category()) throw std::runtime_error("compiled benchmark annotation lacks source category");
   if (source == AnnotationSource::OpenImages && !valid_open_images_category(instance.source_category_id)) throw std::runtime_error("invalid compiled Open Images source category");
  }
  if (instance.mask_rle_offset != expected || instance.mask_rle_offset % sizeof(RLEPair)) throw std::runtime_error("compiled instance RLE metadata is invalid at label " + std::to_string(position));
  const auto bytes = std::size_t{instance.mask_rle_pairs} * sizeof(RLEPair);
  if (expected > rle_bytes || bytes > rle_bytes - expected) throw std::runtime_error("compiled instance RLE span is out of bounds at label " + std::to_string(position));
  return expected + bytes;
 }
 [[nodiscard]] static std::size_t run(const RLEPair& pair, std::size_t previous_end, std::size_t pixels, std::size_t label = 0, std::size_t position = 0) {
  if (!pair.length || pair.start < previous_end || pair.start > pixels || pair.length > pixels - pair.start)
   throw std::runtime_error("compiled RLE run is invalid at label " + std::to_string(label) + ", pair " + std::to_string(position));
  return std::size_t{pair.start} + pair.length;
 }
};
// Producers and readers admit the same per-image payload, with local run offsets.
[[nodiscard]] inline bool admit_compiled_annotations(
 const ImageEntry& entry, ImageAnnotations annotations, const FileHeader& header, std::size_t slot = 0, mmltk::common::concurrency::CancellationObservation cancellation = {}) {
 CompiledRecordChecks::image(entry, slot);
 if (!entry.eligible() || entry.num_instances > header.max_instances_per_image || annotations.labels.size() != entry.num_instances || annotations.runs.size() != entry.num_runs ||
     annotation_layout(entry.num_instances, entry.num_runs).used_bytes != entry.label_bytes)
  throw std::runtime_error("compiled annotation counts differ at slot " + std::to_string(slot));
 const auto pixels = mmltk::common::math::checked_multiply<std::size_t>(header.image_width, header.image_height, "compiled mask size overflow");
 std::size_t consumed = 0;
 bool masks = false;
 for (std::size_t i = 0; i < annotations.labels.size(); ++i) {
  throw_if_compiled_validation_cancelled(i, cancellation);
  const auto& label = annotations.labels[i];
  const auto end = CompiledRecordChecks::label(label, header.num_classes, annotations.runs.size_bytes(), consumed, entry.source, i);
  masks = masks || label.has_mask();
  std::size_t previous_end = 0;
  for (; consumed < end; consumed += sizeof(RLEPair)) {
   throw_if_compiled_validation_cancelled(consumed / sizeof(RLEPair), cancellation);
   previous_end = CompiledRecordChecks::run(annotations.runs[consumed / sizeof(RLEPair)], previous_end, pixels, i, consumed / sizeof(RLEPair));
  }
 }
 if (consumed != annotations.runs.size_bytes()) throw std::runtime_error("compiled image has unreferenced mask runs");
 return masks;
}
inline void set_annotation_extent(ImageEntry& entry, std::size_t labels, std::size_t runs, std::string_view identity) {
 try {
  const auto payload = annotation_layout(labels, runs);
  entry.num_instances = mmltk::common::math::checked_cast<std::uint16_t>(labels, "compiled instance count overflow");
  entry.num_runs = mmltk::common::math::checked_cast<std::uint32_t>(runs, "compiled run count overflow");
  entry.label_bytes = mmltk::common::math::checked_cast<std::uint32_t>(payload.used_bytes, "compiled label size overflow");
 } catch (const std::exception& error) { throw std::runtime_error("image " + std::string(identity) + ": " + error.what()); }
}
// A bounded used-record transform serves new writes and provenance settlement.
// Empty source reads existing records; neither path touches mask runs or pixels.
inline void write_compiled_label_ordinals(
 const mmltk::common::io::FileHandle& file, std::size_t offset, std::size_t count, std::span<const PackedInstance> source, std::uint64_t base, bool incomplete = false) {
 if (!source.empty() && source.size() != count) throw std::invalid_argument("compiled ordinal source extent differs");
 std::array<PackedInstance, 256> records;
 for (std::size_t first = 0; first < count;) {
  const auto size = std::min(records.size(), count - first);
  const auto bytes = size * sizeof(PackedInstance);
  const auto position =
   mmltk::common::math::checked_add(offset, mmltk::common::math::checked_multiply(first, sizeof(PackedInstance), "compiled ordinal record extent overflow"), "compiled ordinal record offset overflow");
  if (source.empty())
   file.pread_all(records.data(), bytes, position);
  else
   std::memcpy(records.data(), source.data() + first, bytes);
  for (auto& record : std::span(records).first(size)) {
   record.source_ordinal = mmltk::common::math::checked_add(record.source_ordinal, base, "benchmark source ordinal overflow");
   if (incomplete) record.flags |= kAnnotationOrdinalIncomplete;
  }
  file.pwrite_all(records.data(), bytes, position);
  first += size;
 }
}
inline void write_compiled_annotations(
 const mmltk::common::io::FileHandle& file, const FileLayout& layout, std::uint32_t slot, const ImageEntry& entry, ImageAnnotations annotations, std::uint64_t ordinal_base = 0) {
 const auto payload = annotation_layout(entry.num_instances, entry.num_runs);
 if (annotations.labels.size() != entry.num_instances || annotations.runs.size() != entry.num_runs || entry.label_bytes != payload.used_bytes || entry.label_bytes > layout.label_capacity)
  throw std::runtime_error("compiled annotation write extent differs");
 const auto offset = layout.label_offset(slot);
 const std::uint64_t zero = 0;
 if (ordinal_base) write_compiled_label_ordinals(file, offset, annotations.labels.size(), annotations.labels, ordinal_base);
 const std::array spans{
  mmltk::common::io::FileHandle::WriteSpan{annotations.labels.data(), ordinal_base ? 0 : annotations.labels.size_bytes()},
  mmltk::common::io::FileHandle::WriteSpan{&zero, payload.run_offset - annotations.labels.size_bytes()},
  mmltk::common::io::FileHandle::WriteSpan{annotations.runs.data(), annotations.runs.size_bytes()}
 };
 file.pwritev_all(spans, offset + (ordinal_base ? annotations.labels.size_bytes() : 0));
}
inline constexpr std::size_t COMPILED_COMPACTION_SCRATCH_BYTES = 4U * 1024U * 1024U;
namespace detail { class StagedArtifact; }
struct CompiledStorageMutation {
 using Ranges = std::span<const common::io::FileHandle::Range>;
 [[nodiscard]] static CompiledStorageMutation for_artifact(detail::StagedArtifact&);
 std::function<void(Ranges, common::concurrency::CancellationObservation, std::uint64_t* syscalls)> allocate{};
 std::function<bool(Ranges, common::concurrency::CancellationObservation, std::uint64_t* syscalls)> reclaim{};
 std::function<void(std::size_t, common::concurrency::CancellationObservation, std::uint64_t* syscalls)> shrink{};
};
// Calls/ranges are wrapper requests; syscall counts are actual pread, pwritev,
// fallocate and ftruncate attempts (including retries), excluding ledger stat
// probes. Byte counters record completed copy requests, even on a failed run.
struct CompiledCompactionObservation {
 std::uint64_t live_bytes = 0, moved_bytes = 0, stationary_bytes = 0;
 std::uint64_t read_calls = 0, read_bytes = 0, read_syscalls = 0, read_ns = 0;
 std::uint64_t write_calls = 0, write_bytes = 0, write_syscalls = 0, write_ns = 0;
 std::uint64_t allocation_calls = 0, allocation_ranges = 0, allocation_syscalls = 0, allocation_ns = 0;
 std::uint64_t reclaim_calls = 0, reclaim_ranges = 0, reclaim_syscalls = 0, reclaim_ns = 0;
 std::uint64_t truncate_calls = 0, truncate_syscalls = 0, truncate_ns = 0;
 std::uint64_t scratch_high_water = 0, capacity_fallbacks = 0, duration_ns = 0;
 bool reclaim_supported = true, failed = false;
};
MMLTK_REFLECT_FIELDS(CompiledCompactionObservation)
using CompiledCompactionObserver = std::function<void(const CompiledCompactionObservation&)>;
using CompiledCompactionProgress = std::function<void(std::size_t completed_bytes, std::size_t total_bytes)>;
// All pixel/label borrowers must be drained. Compacts this private staged inode;
// a failed or cancelled compaction is discarded by its existing publication owner.
[[nodiscard]] FileLayout compact_compiled_file(const mmltk::common::io::FileHandle&, const FileLayout&, std::span<const ImageEntry>, mmltk::common::concurrency::CancellationObservation = {},
 CompiledCompactionProgress = {}, CompiledStorageMutation = {}, std::span<std::byte> scratch = {}, CompiledCompactionObserver = {});
inline catalog::ClassCatalog compiled_class_catalog(const FileHeader& header) {
 std::vector<std::string> names;
 names.reserve(header.num_classes);
 for (std::uint32_t index = 0U; index < header.num_classes; ++index) {
  const auto& stored_name = header.class_names[index];
  const std::size_t length = ::strnlen(stored_name.data(), stored_name.size());
  if (length == 0U || length == stored_name.size()) { throw std::runtime_error("compiled file contains an empty or unterminated class name at index " + std::to_string(index)); }
  std::string name(stored_name.data(), length);
  names.push_back(std::move(name));
 }
 return catalog::ClassCatalog(std::move(names), COMPILED_CLASS_NAME_CAPACITY);
}
inline CompiledDatasetInfo inspect_compiled_dataset(const std::filesystem::path& path) {
 const mmltk::common::io::FileHandle file = mmltk::common::io::FileHandle::open_readonly(path.string());
 const FileHeader header = read_compiled_header(file);
 (void)validate_compiled_file_sections(header, file.size());
 CompiledDatasetInfo info;
 info.path = path;
 info.image_count = header.num_images;
 info.width = header.image_width;
 info.height = header.image_height;
 info.channels = header.channels;
 info.max_instances_per_image = header.max_instances_per_image;
 info.class_catalog = std::make_shared<const catalog::ClassCatalog>(compiled_class_catalog(header));
 return info;
}
}  // namespace mmltk::backend::data
