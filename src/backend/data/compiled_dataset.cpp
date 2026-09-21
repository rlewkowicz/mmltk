#include "src/backend/data/compiled_dataset.h"
#include <sys/mman.h>
#include <cassert>
#include <cstring>
#include "src/backend/data/compiled_file_utils.h"
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
CompiledDataset CompiledDataset::open_mapped(const std::filesystem::path& path, mmltk::common::io::MappedFile mapping, const std::size_t image_limit,
                                             const AccessPattern access) {
    CompiledDataset store;
    store.path_ = path;
    store.mapping_ = std::move(mapping);
    if (store.mapping_.size() < sizeof(mmltk::backend::data::FileHeader)) { throw std::runtime_error("compiled dataset is smaller than its header"); }
    std::memcpy(&store.header_, store.mapping_.data(), sizeof(store.header_));
    mmltk::backend::data::validate_compiled_header(store.header_);
    if (store.header_.num_images > image_limit) { throw std::runtime_error("compiled dataset exceeds the caller image limit"); }
    const mmltk::backend::data::CompiledFileSections sections = mmltk::backend::data::validate_compiled_file_sections(store.header_, store.mapping_.size());
    const auto* image_entries = reinterpret_cast<const mmltk::backend::data::ImageEntry*>(store.mapping_.data() + sections.index_offset);
    const auto* labels = reinterpret_cast<const mmltk::backend::data::PackedInstance*>(store.mapping_.data() + sections.label_offset);
    const auto* rle_pairs = reinterpret_cast<const mmltk::backend::data::RLEPair*>(store.mapping_.data() + sections.rle_offset);
    store.image_entries_ = {image_entries, store.header_.num_images};
    store.labels_ = {labels, sections.label_count};
    store.rle_pairs_ = {rle_pairs, sections.rle_region_bytes / sizeof(mmltk::backend::data::RLEPair)};
    mmltk::backend::data::validate_compiled_index_entries(store.image_entries_, store.header_, store.labels_.size());
    mmltk::backend::data::validate_compiled_original_image_dimensions(store.image_entries_);
    const std::size_t used_rle_bytes = mmltk::backend::data::validate_compiled_label_entries(store.labels_, store.header_, store.rle_pairs_.size_bytes());
    if (used_rle_bytes != store.rle_pairs_.size_bytes()) { throw std::runtime_error("compiled label metadata does not reference the complete RLE block"); }
    if (store.header_.image_height != 0U && store.header_.image_width > std::numeric_limits<std::size_t>::max() / store.header_.image_height) {
        throw std::overflow_error("compiled mask size overflow");
    }
    const std::size_t mask_pixels = static_cast<std::size_t>(store.header_.image_width) * store.header_.image_height;
    mmltk::backend::data::validate_compiled_rle_pairs(store.labels_, store.rle_pairs_, mask_pixels);
    validate_compiled_annotation_provenance(store.image_entries_, store.labels_);
    store.masks_available_ = std::ranges::any_of(store.labels_, [](const PackedInstance& label) { return label.has_mask(); });
    store.label_index_.reserve(store.image_entries_.size());
    for (const auto& entry : store.image_entries_)
        store.label_index_.push_back({static_cast<std::uint32_t>(entry.label_offset / sizeof(PackedInstance)), entry.num_instances, 0});
    store.catalog_ = std::make_shared<const catalog::ClassCatalog>(compiled_class_catalog(store.header_));
    store.mapping_.advise_aligned_range(sections.index_offset, sections.expected_index_bytes, MADV_SEQUENTIAL);
    store.mapping_.advise_aligned_range(sections.pixel_offset, sections.pixel_blob_size, MADV_HUGEPAGE);
    const int advice = access == AccessPattern::Normal ? MADV_NORMAL : access == AccessPattern::Sequential ? MADV_SEQUENTIAL : MADV_RANDOM;
    store.mapping_.advise_aligned_range(sections.pixel_offset, sections.pixel_blob_size, advice);
    store.mapping_.advise_aligned_range(sections.label_offset, sections.label_bytes + sections.rle_region_bytes, MADV_SEQUENTIAL);
    return store;
}
const std::filesystem::path& CompiledDataset::path() const noexcept { return path_; }
const mmltk::backend::data::FileHeader& CompiledDataset::header() const noexcept { return header_; }
std::span<const std::string> CompiledDataset::class_names() const noexcept { return catalog_ ? catalog_->names() : std::span<const std::string>{}; }
std::span<const mmltk::backend::data::ImageEntry> CompiledDataset::image_entries() const noexcept { return image_entries_; }
std::span<const mmltk::backend::data::PackedInstance> CompiledDataset::labels() const noexcept { return labels_; }
std::span<const mmltk::backend::data::RLEPair> CompiledDataset::rle_pairs() const noexcept { return rle_pairs_; }
bool CompiledDataset::masks_available() const noexcept { return masks_available_; }
const mmltk::backend::data::ImageEntry& CompiledDataset::image_entry(const std::uint32_t compiled_index) const noexcept {
    assert(compiled_index < image_entries_.size());
    return image_entries_[compiled_index];
}
std::span<const mmltk::backend::data::PackedInstance> CompiledDataset::image_labels(const std::uint32_t compiled_index) const noexcept {
    const mmltk::backend::data::ImageEntry& entry = image_entry(compiled_index);
    const std::size_t begin = entry.label_offset / sizeof(mmltk::backend::data::PackedInstance);
    return labels_.subspan(begin, entry.num_instances);
}
std::span<const mmltk::backend::data::RLEPair> CompiledDataset::instance_rle(const mmltk::backend::data::PackedInstance& instance) const noexcept {
    const std::size_t begin = instance.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair);
    return rle_pairs_.subspan(begin, instance.mask_rle_pairs);
}
const float* CompiledDataset::image_pixels(const std::uint32_t compiled_index) const noexcept {
    const mmltk::backend::data::ImageEntry& entry = image_entry(compiled_index);
    // The compiled layout validates float alignment and byte extent when the mapping is opened.
    // cppcheck-suppress invalidPointerCast
    return reinterpret_cast<const float*>(mapping_.data() + entry.pixel_offset);
}
mmltk::backend::imaging::resample::ImageResizeGeometry CompiledDataset::geometry(const std::uint32_t compiled_index) const {
    const mmltk::backend::data::ImageEntry& entry = image_entry(compiled_index);
    return mmltk::backend::imaging::resample::compute_image_resize_geometry(entry.original_width, entry.original_height, header_.image_width,
                                                                            header_.image_height, header_.resize_mode);
}
std::span<const LabelIndexEntry> CompiledDataset::label_index() const noexcept { return label_index_; }
const float* CompiledDataset::pixel_blob() const noexcept { return reinterpret_cast<const float*>(mapping_.data() + header_.pixel_offset); }
bool CompiledDataset::read_images(const std::span<const CompiledImageRead> reads, const std::span<std::byte> destination, const std::atomic<bool>& cancelled,
                                  const bool prefault) const {
    return read_images_to(reads,
                          ImageDestination{destination.data(), destination.size(),
                                           [](void* data, std::size_t offset, std::span<const std::byte> bytes) {
                                               std::memcpy(static_cast<std::byte*>(data) + offset, bytes.data(), bytes.size());
                                           }},
                          cancelled, prefault);
}
bool CompiledDataset::read_images_to(const std::span<const CompiledImageRead> reads, const ImageDestination destination, const std::atomic<bool>& cancelled,
                                     const bool prefault) const {
    if (!destination.write) throw std::invalid_argument("compiled image destination has no writer");
    constexpr std::size_t kReadExtent = 16U * 1024U * 1024U;
    const auto stride = static_cast<std::size_t>(header_.image_stride);
    for (const auto& read : reads) {
        if (read.index >= image_entries_.size() || read.destination_offset > destination.capacity || stride > destination.capacity - read.destination_offset)
            throw std::out_of_range("compiled image read exceeds source or destination");
    }
    for (std::size_t first = 0; first < reads.size();) {
        std::size_t count = 1;
        while (first + count < reads.size() && reads[first + count].index == reads[first].index + count &&
               reads[first + count].destination_offset == reads[first].destination_offset + count * stride &&
               count < std::max<std::size_t>(1, kReadExtent / stride))
            ++count;
        const auto source_offset = image_entries_[reads[first].index].pixel_offset;
        const auto bytes = count * stride;
        for (std::size_t offset = 0; offset < bytes;) {
            if (cancelled.load(std::memory_order_acquire)) return false;
            const auto extent = std::min(kReadExtent, bytes - offset);
            mapping_.advise_aligned_range(source_offset + offset, extent, MADV_WILLNEED);
            if (prefault) mapping_.advise_aligned_range(source_offset + offset, extent, MADV_POPULATE_READ);
            destination.write(destination.context, reads[first].destination_offset + offset,
                              {reinterpret_cast<const std::byte*>(mapping_.data() + source_offset + offset), extent});
            offset += extent;
        }
        first += count;
    }
    return !cancelled.load(std::memory_order_acquire);
}
}  // namespace mmltk::backend::data
