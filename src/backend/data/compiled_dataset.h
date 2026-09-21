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
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/catalog/class_catalog.h"
#include <memory>
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/io/file_memory.h"
namespace mmltk::backend::data {
struct LabelIndexEntry {
 uint32_t label_begin;
 uint16_t num_instances;
 uint16_t _pad;
};
static_assert(sizeof(LabelIndexEntry) == 8);
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
 [[nodiscard]] std::span<const mmltk::backend::data::PackedInstance> labels() const noexcept;
 [[nodiscard]] std::span<const mmltk::backend::data::RLEPair> rle_pairs() const noexcept;
 [[nodiscard]] bool masks_available() const noexcept;
 [[nodiscard]] const mmltk::backend::data::ImageEntry& image_entry(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] std::span<const mmltk::backend::data::PackedInstance> image_labels(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] std::span<const mmltk::backend::data::RLEPair> instance_rle(const mmltk::backend::data::PackedInstance& instance) const noexcept;
 [[nodiscard]] const float* image_pixels(std::uint32_t compiled_index) const noexcept;
 [[nodiscard]] mmltk::backend::imaging::resample::ImageResizeGeometry geometry(std::uint32_t compiled_index) const;
 [[nodiscard]] std::span<const LabelIndexEntry> label_index() const noexcept;
 [[nodiscard]] const float* pixel_blob() const noexcept;
 // Called only by image-stream I/O workers. Each advised/populated range is
 // bounded; successful advice does not replace the actual gather copy.
 struct ImageDestination {
  void* context;
  std::size_t capacity;
  void (*write)(void*, std::size_t, std::span<const std::byte>);
 };
 [[nodiscard]] bool read_images_to(std::span<const CompiledImageRead>, ImageDestination, const std::atomic<bool>&, bool prefault = false) const;
 [[nodiscard]] bool read_images(std::span<const CompiledImageRead> reads, std::span<std::byte> destination, const std::atomic<bool>& cancelled,
                                bool prefault = false) const;

private:
 [[nodiscard]] static CompiledDataset open_mapped(const std::filesystem::path&, mmltk::common::io::MappedFile, std::size_t image_limit, AccessPattern);
 std::filesystem::path path_;
 mmltk::common::io::MappedFile mapping_;
 mmltk::backend::data::FileHeader header_{};
 std::span<const mmltk::backend::data::ImageEntry> image_entries_;
 std::span<const mmltk::backend::data::PackedInstance> labels_;
 std::span<const mmltk::backend::data::RLEPair> rle_pairs_;
 std::shared_ptr<const catalog::ClassCatalog> catalog_;
 std::vector<LabelIndexEntry> label_index_;
 bool masks_available_ = false;
};
}  // namespace mmltk::backend::data
