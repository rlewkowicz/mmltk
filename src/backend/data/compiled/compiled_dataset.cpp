#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/pch_linux.h"
#include <cassert>
#include "src/pch_std.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
CompiledDataset CompiledDataset::open(const std::filesystem::path& path, const AccessPattern access) {
 auto mapping = mmltk::common::io::MappedFile::try_open_readonly(path.string());
 if (!mapping) { throw std::system_error{mapping.error(), "compiled dataset source open failed: " + path.string()}; }
 return open_mapped(path, std::move(*mapping), std::numeric_limits<std::size_t>::max(), access);
}
std::expected<CompiledDataset, std::error_code> CompiledDataset::open_source(const std::filesystem::path& path, const std::size_t image_limit) {
 auto mapping = mmltk::common::io::MappedFile::try_open_readonly(path.string());
 if (!mapping) { return std::unexpected{mapping.error()}; }
 return open_mapped(path, std::move(*mapping), image_limit, AccessPattern::Random);
}
CompiledDataset CompiledDataset::open_mapped(const std::filesystem::path& path, mmltk::common::io::MappedFile mapping, const std::size_t image_limit, const AccessPattern access) {
 CompiledDataset store;
 store.path_ = path;
 store.mapping_ = std::move(mapping);
 if (store.mapping_.size() < sizeof(mmltk::backend::data::FileHeader)) { throw std::runtime_error("compiled dataset is smaller than its header"); }
 std::memcpy(&store.header_, store.mapping_.data(), sizeof(store.header_));
 mmltk::backend::data::validate_compiled_header(store.header_);
 if (store.header_.num_images > image_limit) { throw std::runtime_error("compiled dataset exceeds the caller image limit"); }
 const mmltk::backend::data::CompiledFileSections sections = mmltk::backend::data::validate_compiled_file_sections(store.header_, store.mapping_.size());
 store.layout_ = sections.layout;
 const int advice = access == AccessPattern::Normal ? MADV_NORMAL : access == AccessPattern::Sequential ? MADV_SEQUENTIAL : MADV_RANDOM;
 store.mapping_.advise_aligned_range(store.layout_.data_offset, store.layout_.index_offset - store.layout_.data_offset, advice);
 store.image_entries_.resize(store.header_.num_slots);
 std::memcpy(store.image_entries_.data(), store.mapping_.data() + sections.index_offset, sections.expected_index_bytes);
 std::size_t eligible = 0;
 std::uint32_t maximum_label_bytes = 0;
 for (std::uint32_t slot = 0; slot < store.header_.num_slots; ++slot) {
  const auto& entry = store.image_entries_[slot];
  if (entry.status != ImageStatus::Inactive && entry.status != ImageStatus::Eligible) throw std::runtime_error("invalid compiled slot status");
  if (!entry.eligible()) {
   if (entry.num_instances || entry.num_runs || entry.label_bytes) throw std::runtime_error("inactive compiled slot has published annotations");
   continue;
  }
  ++eligible;
  maximum_label_bytes = std::max(maximum_label_bytes, entry.label_bytes);
  store.masks_available_ |= admit_compiled_annotations(entry, store.annotations(slot), store.header_, slot);
  store.label_count_ += entry.num_instances;
  store.run_count_ += entry.num_runs;
 }
 if (eligible != store.header_.num_images) throw std::runtime_error("compiled eligible population differs from header");
 if (maximum_label_bytes != store.header_.max_label_bytes) throw std::runtime_error("compiled maximum label size differs from eligible payloads");
 store.catalog_ = std::make_shared<const catalog::ClassCatalog>(compiled_class_catalog(store.header_));
 return store;
}
const std::filesystem::path& CompiledDataset::path() const noexcept { return path_; }
const mmltk::backend::data::FileHeader& CompiledDataset::header() const noexcept { return header_; }
std::span<const std::string> CompiledDataset::class_names() const noexcept { return catalog_ ? catalog_->names() : std::span<const std::string>{}; }
std::span<const mmltk::backend::data::ImageEntry> CompiledDataset::image_entries() const noexcept { return image_entries_; }
bool CompiledDataset::masks_available() const noexcept { return masks_available_; }
const ImageEntry& CompiledDataset::image_entry(std::uint32_t slot) const {
 if (!eligible(slot)) throw std::out_of_range("compiled image slot is not eligible");
 return image_entries_[slot];
}
std::span<const std::byte> CompiledDataset::label_payload(std::uint32_t slot) const {
 const auto& entry = image_entry(slot);
 if (entry.label_bytes > header_.max_label_bytes) throw std::runtime_error("compiled label payload exceeds recorded maximum");
 return {reinterpret_cast<const std::byte*>(mapping_.data() + layout_.label_offset(slot)), entry.label_bytes};
}
ImageAnnotations CompiledDataset::annotations(std::uint32_t slot) const { return annotation_view(image_entry(slot), label_payload(slot)); }
std::span<const PackedInstance> CompiledDataset::image_labels(std::uint32_t slot) const { return annotations(slot).labels; }
std::span<const RLEPair> CompiledDataset::instance_rle(std::uint32_t slot, const PackedInstance& instance) const { return annotations(slot).instance_rle(instance); }
const float* CompiledDataset::image_pixels(std::uint32_t slot) const {
 (void)image_entry(slot);
 return reinterpret_cast<const float*>(mapping_.data() + layout_.pixel_offset(slot));
}
mmltk::backend::imaging::resample::ImageResizeGeometry CompiledDataset::geometry(const std::uint32_t compiled_index) const {
 const mmltk::backend::data::ImageEntry& entry = image_entry(compiled_index);
 return mmltk::backend::imaging::resample::compute_image_resize_geometry(entry.original_width, entry.original_height, header_.image_width, header_.image_height, header_.resize_mode);
}
std::size_t CompiledDataset::gather_chunks(std::size_t images, std::size_t stride, std::size_t pitch) {
 const auto bytes = mmltk::common::math::checked_multiply(images, mmltk::common::math::checked_add(stride, pitch, "compiled read extent overflow"), "compiled batch extent overflow");
 return bytes / read_chunk_bytes + (bytes % read_chunk_bytes != 0);
}
CompiledDataset::UsedRangeCursor CompiledDataset::used_ranges(std::span<const CompiledImageRead> reads, std::optional<std::size_t> label_pitch, bool advice) const {
 UsedRangeCursor cursor;
 cursor.source_ = this;
 cursor.reads_ = reads;
 cursor.label_pitch_ = label_pitch.value_or(0);
 cursor.section_ = label_pitch ? ReadSection::Labels : ReadSection::Pixels;
 cursor.advice_ = advice;
 cursor.normalize();
 return cursor;
}
void CompiledDataset::UsedRangeCursor::normalize() {
 while (source_) {
  if (image_ == reads_.size()) {
   if (section_ == ReadSection::Pixels) {
    source_ = nullptr;
    return;
   }
   section_ = ReadSection::Pixels;
   image_ = 0;
   offset_ = 0;
   continue;
  }
  const auto& entry = source_->image_entry(reads_[image_].index);
  if ((section_ == ReadSection::Labels && entry.label_bytes == 0) || (advice_ && offset_ == 0 && image_ != 0 && reads_[image_ - 1].index == reads_[image_].index)) {
   ++image_;
   continue;
  }
  return;
 }
}
std::optional<CompiledDataset::UsedRange> CompiledDataset::UsedRangeCursor::next(std::size_t limit) {
 if (!limit || limit > read_chunk_bytes) throw std::invalid_argument("compiled read chunk limit is invalid");
 std::optional<UsedRange> result;
 while (source_) {
  const auto& read = reads_[image_];
  const bool labels = section_ == ReadSection::Labels;
  const auto size = labels ? source_->image_entry(read.index).label_bytes : source_->layout_.image_stride;
  const auto source_offset = (labels ? source_->layout_.label_offset(read.index) : source_->layout_.pixel_offset(read.index)) + offset_;
  const auto destination_offset =
   (labels ? mmltk::common::math::checked_multiply(read.destination_offset / source_->layout_.image_stride, label_pitch_, "compiled label destination overflow") : read.destination_offset) + offset_;
  if (result && (result->section != section_ || result->source_offset + result->bytes != source_offset || (!advice_ && result->destination_offset + result->bytes != destination_offset))) break;
  const auto bytes = std::min(size - offset_, limit - (result ? result->bytes : 0U));
  if (result)
   result->bytes += bytes;
  else
   result = UsedRange{section_, source_offset, destination_offset, bytes};
  offset_ += bytes;
  if (offset_ == size) {
   ++image_;
   offset_ = 0;
  }
  normalize();
  if (result->bytes == limit) break;
 }
 return result;
}
void CompiledDataset::prepare_range(const UsedRange& range, bool prefault) const { mapping_.advise_aligned_range(range.source_offset, range.bytes, prefault ? MADV_POPULATE_READ : MADV_WILLNEED); }
void CompiledDataset::gather_range(const UsedRange& range, const ImageDestination destination) const {
 if (!destination.write) throw std::invalid_argument("compiled image destination has no writer");
 if (range.source_offset > mapping_.size() || range.bytes > mapping_.size() - range.source_offset || range.destination_offset > destination.capacity ||
     range.bytes > destination.capacity - range.destination_offset)
  throw std::out_of_range("compiled read exceeds source or destination");
 destination.write(destination.context, range.destination_offset, {reinterpret_cast<const std::byte*>(mapping_.data() + range.source_offset), range.bytes});
}
bool CompiledDataset::read_images(const std::span<const CompiledImageRead> reads, const std::span<std::byte> destination, const std::atomic<bool>& cancelled, const bool prefault) const {
 return read_images_to(reads,
  ImageDestination{
   destination.data(), destination.size(), [](void* data, std::size_t offset, std::span<const std::byte> bytes) { std::memcpy(static_cast<std::byte*>(data) + offset, bytes.data(), bytes.size()); }
  },
  cancelled, prefault);
}
bool CompiledDataset::read_images_to(const std::span<const CompiledImageRead> reads, const ImageDestination destination, const std::atomic<bool>& cancelled, const bool prefault) const {
 if (!destination.write) throw std::invalid_argument("compiled image destination has no writer");
 const auto stride = static_cast<std::size_t>(header_.image_stride);
 for (const auto& read : reads) {
  if (!eligible(read.index) || read.destination_offset > destination.capacity || stride > destination.capacity - read.destination_offset)
   throw std::out_of_range("compiled image read exceeds source or destination");
 }
 auto advice = used_ranges(reads, {}, true);
 while (const auto range = advice.next()) {
  if (cancelled.load(std::memory_order_acquire)) return false;
  prepare_range(*range, false);
 }
 auto gather = used_ranges(reads);
 while (const auto range = gather.next()) {
  if (cancelled.load(std::memory_order_acquire)) return false;
  if (prefault) prepare_range(*range, true);
  gather_range(*range, destination);
 }
 return !cancelled.load(std::memory_order_acquire);
}
}  // namespace mmltk::backend::data
