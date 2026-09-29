#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "src/backend/data/compiled/compiled_file_layout.h"
#include <ranges>
#include "src/backend/data/catalog/class_catalog.h"
#include <memory>
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data {
struct CompiledImageRead {
 std::uint32_t index = 0;
 std::size_t destination_offset = 0;
};
class CompiledDataset {
public:
 enum class AccessPattern : std::uint8_t { Normal, Sequential, Random };
 CompiledDataset() = default;
 CompiledDataset(const CompiledDataset&) = delete;
 CompiledDataset& operator=(const CompiledDataset&) = delete;
 CompiledDataset(CompiledDataset&&) noexcept = default;
 CompiledDataset& operator=(CompiledDataset&&) noexcept = default;
 ~CompiledDataset() = default;
 [[nodiscard]] static CompiledDataset open(const std::filesystem::path& path, AccessPattern access = AccessPattern::Random);
 [[nodiscard]] static std::expected<CompiledDataset, std::error_code> open_source(const std::filesystem::path& path, std::size_t image_limit);
 [[nodiscard]] const std::filesystem::path& path() const noexcept;
 [[nodiscard]] const mmltk::backend::data::FileHeader& header() const noexcept;
 [[nodiscard]] std::span<const std::string> class_names() const noexcept;
 [[nodiscard]] const std::shared_ptr<const catalog::ClassCatalog>& class_catalog() const noexcept { return catalog_; }
 [[nodiscard]] std::span<const mmltk::backend::data::ImageEntry> image_entries() const noexcept;
 [[nodiscard]] auto eligible_slots() const {
  return std::views::iota(std::uint32_t{0}, header_.num_slots) | std::views::filter([this](std::uint32_t slot) { return image_entries_[slot].eligible(); });
 }
 [[nodiscard]] bool eligible(std::uint32_t slot) const noexcept { return slot < image_entries_.size() && image_entries_[slot].eligible(); }
 [[nodiscard]] std::size_t slot_count() const noexcept { return image_entries_.size(); }
 [[nodiscard]] std::size_t label_count() const noexcept { return label_count_; }
 [[nodiscard]] std::size_t run_count() const noexcept { return run_count_; }
 [[nodiscard]] ImageAnnotations annotations(std::uint32_t slot) const;
 [[nodiscard]] std::span<const std::byte> label_payload(std::uint32_t slot) const;
 [[nodiscard]] std::size_t pixel_offset(std::uint32_t slot) const { return layout_.pixel_offset(slot); }
 [[nodiscard]] bool masks_available() const noexcept;
 [[nodiscard]] const mmltk::backend::data::ImageEntry& image_entry(std::uint32_t compiled_index) const;
 [[nodiscard]] std::span<const mmltk::backend::data::PackedInstance> image_labels(std::uint32_t compiled_index) const;
 [[nodiscard]] std::span<const mmltk::backend::data::RLEPair> instance_rle(std::uint32_t slot, const mmltk::backend::data::PackedInstance& instance) const;
 [[nodiscard]] const float* image_pixels(std::uint32_t compiled_index) const;
 [[nodiscard]] mmltk::backend::imaging::resample::ImageResizeGeometry geometry(std::uint32_t compiled_index) const;
 // Called only by image-stream I/O workers. Each advised/populated range is
 // bounded; successful advice does not replace the actual gather copy. Source
 // runs across this admitted batch are advised before gathering, independently
 // of destination adjacency. Source-ordered inventories maximize coalescing.
 struct ImageDestination {
  void* context;
  std::size_t capacity;
  void (*write)(void*, std::size_t, std::span<const std::byte>);
 };
 [[nodiscard]] bool read_images_to(std::span<const CompiledImageRead>, ImageDestination, const std::atomic<bool>&, bool prefault = false) const;
 [[nodiscard]] bool read_images(std::span<const CompiledImageRead> reads, std::span<std::byte> destination, const std::atomic<bool>& cancelled, bool prefault = false) const;

private:
 [[nodiscard]] static CompiledDataset open_mapped(const std::filesystem::path&, mmltk::common::io::MappedFile, std::size_t image_limit, AccessPattern);
 std::filesystem::path path_;
 mmltk::common::io::MappedFile mapping_;
 mmltk::backend::data::FileHeader header_{};
 std::vector<ImageEntry> image_entries_;
 FileLayout layout_;
 std::size_t label_count_ = 0, run_count_ = 0;
 std::shared_ptr<const catalog::ClassCatalog> catalog_;
 bool masks_available_ = false;
};
}  // namespace mmltk::backend::data
