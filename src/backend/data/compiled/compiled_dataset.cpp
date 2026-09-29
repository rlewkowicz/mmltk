#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/pch_linux.h"
#include <cassert>
#include "src/pch_std.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
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
 store.image_entries_.resize(store.header_.num_slots);
 std::memcpy(store.image_entries_.data(), store.mapping_.data() + sections.index_offset, sections.expected_index_bytes);
 std::size_t eligible = 0;
 for (std::uint32_t slot = 0; slot < store.header_.num_slots; ++slot) {
  const auto& entry = store.image_entries_[slot];
  if (entry.status != ImageStatus::Inactive && entry.status != ImageStatus::Eligible) throw std::runtime_error("invalid compiled slot status");
  if (!entry.eligible()) {
   if (entry.num_instances || entry.num_runs || entry.label_bytes) throw std::runtime_error("inactive compiled slot has published annotations");
   continue;
  }
  ++eligible;
  store.masks_available_ |= admit_compiled_annotations(entry, store.annotations(slot), store.header_, slot);
  store.label_count_ += entry.num_instances;
  store.run_count_ += entry.num_runs;
 }
 if (eligible != store.header_.num_images) throw std::runtime_error("compiled eligible population differs from header");
 store.catalog_ = std::make_shared<const catalog::ClassCatalog>(compiled_class_catalog(store.header_));
 const int advice = access == AccessPattern::Normal ? MADV_NORMAL : access == AccessPattern::Sequential ? MADV_SEQUENTIAL : MADV_RANDOM;
 store.mapping_.advise_aligned_range(store.layout_.data_offset, store.layout_.index_offset - store.layout_.data_offset, advice);
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
 if (entry.label_bytes > COMPILED_LABEL_CAPACITY) throw std::runtime_error("compiled label payload exceeds capacity");
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
bool CompiledDataset::read_images(const std::span<const CompiledImageRead> reads, const std::span<std::byte> destination, const std::atomic<bool>& cancelled, const bool prefault) const {
 return read_images_to(reads,
  ImageDestination{
   destination.data(), destination.size(), [](void* data, std::size_t offset, std::span<const std::byte> bytes) { std::memcpy(static_cast<std::byte*>(data) + offset, bytes.data(), bytes.size()); }
  },
  cancelled, prefault);
}
bool CompiledDataset::read_images_to(const std::span<const CompiledImageRead> reads, const ImageDestination destination, const std::atomic<bool>& cancelled, const bool prefault) const {
 if (!destination.write) throw std::invalid_argument("compiled image destination has no writer");
 constexpr std::size_t kReadExtent = 16U * 1024U * 1024U;
 const auto stride = static_cast<std::size_t>(header_.image_stride);
 for (const auto& read : reads) {
  if (!eligible(read.index) || read.destination_offset > destination.capacity || stride > destination.capacity - read.destination_offset)
   throw std::out_of_range("compiled image read exceeds source or destination");
 }
 // Request the admitted slot's source runs before copying any of them. Source
 // locality is independent of destination order; repeated images need advice
 // only once, but still have a separate destination for each logical draw.
 for (std::size_t first = 0; first < reads.size();) {
  std::size_t end = first + 1;
  auto last_index = reads[first].index;
  while (end < reads.size() && reads[end].index >= last_index && (reads[end].index == last_index || layout_.pixel_offset(reads[end].index) == layout_.pixel_offset(last_index) + stride) &&
         static_cast<std::size_t>(reads[end].index - reads[first].index) < std::max<std::size_t>(1U, kReadExtent / stride)) {
   last_index = reads[end++].index;
  }
  const auto source_offset = layout_.pixel_offset(reads[first].index);
  const auto bytes = (static_cast<std::size_t>(last_index - reads[first].index) + 1U) * stride;
  for (std::size_t offset = 0; offset < bytes;) {
   if (cancelled.load(std::memory_order_acquire)) return false;
   const auto extent = std::min(kReadExtent, bytes - offset);
   mapping_.advise_aligned_range(source_offset + offset, extent, MADV_WILLNEED);
   offset += extent;
  }
  first = end;
 }
 for (std::size_t first = 0; first < reads.size();) {
  std::size_t count = 1;
  while (first + count < reads.size() && layout_.pixel_offset(reads[first + count].index) == layout_.pixel_offset(reads[first].index) + count * stride && reads[first + count].destination_offset == reads[first].destination_offset + count * stride &&
         count < std::max<std::size_t>(1, kReadExtent / stride))
   ++count;
  const auto source_offset = layout_.pixel_offset(reads[first].index);
  const auto bytes = count * stride;
  for (std::size_t offset = 0; offset < bytes;) {
   if (cancelled.load(std::memory_order_acquire)) return false;
   const auto extent = std::min(kReadExtent, bytes - offset);
   if (prefault) mapping_.advise_aligned_range(source_offset + offset, extent, MADV_POPULATE_READ);
   destination.write(destination.context, reads[first].destination_offset + offset, {reinterpret_cast<const std::byte*>(mapping_.data() + source_offset + offset), extent});
   offset += extent;
  }
  first += count;
 }
 return !cancelled.load(std::memory_order_acquire);
}
}  // namespace mmltk::backend::data
