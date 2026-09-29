#pragma once
#include <memory>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/compiled/compiled_file_layout.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
struct CompiledFileSections {
 size_t index_offset = 0;
 size_t label_offset = 0;
 size_t rle_offset = 0;
 size_t pixel_offset = 0;
 size_t expected_index_bytes = 0;
 size_t label_bytes = 0;
 size_t label_count = 0;
 size_t rle_region_bytes = 0;
 size_t pixel_blob_size = 0;
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
 if (header.num_images == 0U || header.image_width == 0U || header.image_height == 0U || header.channels == 0U) { throw std::runtime_error("compiled file has invalid image dimensions"); }
 if (header.num_classes == 0U || header.num_classes > MAX_CLASSES) { throw std::runtime_error("compiled file has invalid class count"); }
 if (header.max_instances_per_image > std::numeric_limits<std::uint16_t>::max()) { throw std::runtime_error("compiled file maximum instance count exceeds the index representation"); }
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
 const auto index_offset = mmltk::common::math::checked_cast<size_t>(header.index_offset, "index offset overflow");
 const auto label_offset = mmltk::common::math::checked_cast<size_t>(header.label_offset, "label offset overflow");
 const auto rle_offset = mmltk::common::math::checked_cast<size_t>(header.mask_rle_offset, "RLE offset overflow");
 const auto pixel_offset = mmltk::common::math::checked_cast<size_t>(header.pixel_offset, "pixel offset overflow");
 const auto total_file_size = mmltk::common::math::checked_cast<size_t>(header.total_file_size, "file size overflow");
 if (total_file_size != file_size) { throw std::runtime_error("compiled file size does not match header"); }
 if (index_offset != sizeof(FileHeader) || pixel_offset < index_offset || label_offset < pixel_offset || rle_offset < label_offset || total_file_size < rle_offset) {
  throw std::runtime_error("compiled file layout is invalid");
 }
 const auto layout = compute_pixel_layout(header.num_images, mmltk::common::math::checked_cast<size_t>(header.image_stride, "image stride overflow"));
 const auto expected_index_bytes = layout.index_size;
 if (pixel_offset != layout.pixel_offset) { throw std::runtime_error("compiled file index or pixel alignment is invalid"); }
 const size_t label_bytes = rle_offset - label_offset;
 if (label_bytes % sizeof(PackedInstance) != 0) { throw std::runtime_error("label block is not aligned to PackedInstance"); }
 const size_t pixel_blob_size = label_offset - pixel_offset;
 const auto expected_pixel_blob_size = layout.pixel_blob_size;
 if (pixel_blob_size != expected_pixel_blob_size) { throw std::runtime_error("compiled pixel blob size mismatch"); }
 const size_t rle_region_bytes = total_file_size - rle_offset;
 if (rle_region_bytes % sizeof(RLEPair) != 0U) { throw std::runtime_error("compiled RLE block is not aligned to RLEPair"); }
 return CompiledFileSections{
  index_offset,
  label_offset,
  rle_offset,
  pixel_offset,
  expected_index_bytes,
  label_bytes,
  label_bytes / sizeof(PackedInstance),
  rle_region_bytes,
  pixel_blob_size,
 };
}
// Field checks have one owner. Producers use these while copying/encoding;
// mapped readers use the fused admission below before exposing bulk views.
class CompiledRecordChecks final {
public:
 static void image(const ImageEntry& entry, std::size_t position = 0) {
  if (!entry.original_width || !entry.original_height || entry.has_source_image_id > 1U || entry.source > AnnotationSource::CoconutObjects365V2 ||
      (!entry.has_source_image_id && entry.source_image_id) || entry._reserved)
   throw std::runtime_error("compiled original image dimensions are invalid at image " + std::to_string(position));
  if (entry.source != AnnotationSource::Generic && entry.has_source_image_id != 1U) throw std::runtime_error("compiled benchmark image lacks source identity");
 }
 [[nodiscard]] static std::size_t label(const PackedInstance& instance, std::uint32_t classes, std::size_t rle_bytes, std::size_t expected, AnnotationSource source, std::size_t position = 0) {
  if (instance.class_id >= classes) throw std::runtime_error("compiled instance class id is out of bounds at label " + std::to_string(position));
  if (!std::isfinite(instance.bbox_x1) || !std::isfinite(instance.bbox_y1) || !std::isfinite(instance.bbox_x2) || !std::isfinite(instance.bbox_y2) || instance.bbox_x2 <= instance.bbox_x1 ||
      instance.bbox_y2 <= instance.bbox_y1 || !std::isfinite(instance.original_area) || instance.original_area < 0.0 || (instance.flags & ~kAnnotationFlags) ||
      (!instance.has_mask() && instance.mask_rle_pairs) || (!instance.has_annotation_id() && instance.annotation_id) || (!instance.has_source_category() && instance.source_category_id))
   throw std::runtime_error("compiled instance annotation metadata is invalid at label " + std::to_string(position));
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
// The consumer constructs its required metadata in this traversal. Each image,
// label and run is admitted exactly once, including present-empty masks.
template <std::ranges::random_access_range ImageRange, class ImageConsumer>
 requires std::ranges::sized_range<ImageRange>
[[nodiscard]] inline bool admit_compiled_records(const ImageRange& index, std::span<const PackedInstance> labels, std::span<const RLEPair> runs, const FileHeader& header, ImageConsumer&& consume,
 mmltk::common::concurrency::CancellationObservation cancellation = {}) {
 if (index.size() != header.num_images) throw std::runtime_error("compiled index entry count does not match header");
 const auto pixels = mmltk::common::math::checked_multiply<std::size_t>(header.image_width, header.image_height, "compiled mask size overflow");
 std::size_t label_offset = 0, run_offset = 0;
 bool masks = false;
 for (std::size_t i = 0; i < index.size(); ++i) {
  throw_if_compiled_validation_cancelled(i, cancellation);
  const auto& entry = index[i];
  CompiledRecordChecks::image(entry, i);
  if (entry.pixel_offset != header.pixel_offset + std::uint64_t{i} * header.image_stride) throw std::runtime_error("compiled pixel index is inconsistent at image " + std::to_string(i));
  if (entry._pad || entry.num_instances > header.max_instances_per_image || entry.label_offset != label_offset * sizeof(PackedInstance) ||
      entry.label_bytes != std::uint32_t{entry.num_instances} * sizeof(PackedInstance))
   throw std::runtime_error("compiled label index is inconsistent at image " + std::to_string(i));
  if (label_offset > labels.size() || entry.num_instances > labels.size() - label_offset) throw std::runtime_error("compiled label span is out of bounds at image " + std::to_string(i));
  consume(entry);
  for (const auto& label : labels.subspan(label_offset, entry.num_instances)) {
   throw_if_compiled_validation_cancelled(label_offset, cancellation);
   const auto begin = run_offset;
   const auto end = CompiledRecordChecks::label(label, header.num_classes, runs.size_bytes(), run_offset, entry.source, label_offset);
   masks = masks || label.has_mask();
   std::size_t previous_end = 0;
   for (; run_offset < end; run_offset += sizeof(RLEPair)) {
    throw_if_compiled_validation_cancelled(run_offset / sizeof(RLEPair), cancellation);
    previous_end = CompiledRecordChecks::run(runs[run_offset / sizeof(RLEPair)], previous_end, pixels, label_offset, (run_offset - begin) / sizeof(RLEPair));
   }
   ++label_offset;
  }
 }
 if (label_offset != labels.size()) throw std::runtime_error("compiled label index does not reference the complete label block");
 if (run_offset != runs.size_bytes()) throw std::runtime_error("compiled label metadata does not reference the complete RLE block");
 return masks;
}
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
