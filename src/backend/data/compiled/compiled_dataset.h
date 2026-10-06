#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include "src/backend/data/compiled/compiled_file_layout.h"
#include <ranges>
#include "src/backend/data/catalog/class_catalog.h"
#include <memory>
#include <optional>
#include "src/backend/imaging/resample/image_resize.h"
#include "src/common/io/file_memory.h"
#include "src/common/types/cancellation_observation.h"
namespace mmltk::common::concurrency {
class WorkerPool;
}
namespace mmltk::backend::data {
struct CompiledDatasetOpenOptions {
 // Compiler-published metadata is trusted by every reader. Opening copies the
 // index and totals its recorded counts without verifying slots or payloads.
 mmltk::common::concurrency::WorkerPool* workers = nullptr;
 mmltk::common::concurrency::CancellationObservation cancellation{};
 // Optional diagnostics; called from admission workers, possibly concurrently.
 // Neither callback delivery nor these counts govern admission or lifetime.
 std::function<void(std::size_t completed, std::size_t total)> progress{};
};
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
 [[nodiscard]] static CompiledDataset open(const std::filesystem::path& path, AccessPattern access = AccessPattern::Random, const CompiledDatasetOpenOptions& = {});
 [[nodiscard]] static std::expected<CompiledDataset, std::error_code> open_source(const std::filesystem::path& path, std::size_t image_limit, const CompiledDatasetOpenOptions& = {});
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
 [[nodiscard]] bool masks_available() const;
 [[nodiscard]] const mmltk::backend::data::ImageEntry& image_entry(std::uint32_t compiled_index) const;
 [[nodiscard]] std::span<const mmltk::backend::data::PackedInstance> image_labels(std::uint32_t compiled_index) const;
 [[nodiscard]] std::span<const mmltk::backend::data::RLEPair> instance_rle(std::uint32_t slot, const mmltk::backend::data::PackedInstance& instance) const;
 [[nodiscard]] const float* image_pixels(std::uint32_t compiled_index) const;
 [[nodiscard]] mmltk::backend::imaging::resample::ImageResizeGeometry geometry(std::uint32_t compiled_index) const;
 static constexpr std::size_t read_chunk_bytes = 16U * 1024U * 1024U;
 enum class ReadSection : std::uint8_t { Labels, Pixels };
 struct UsedRange {
  ReadSection section;
  std::size_t source_offset, destination_offset, bytes;
 };
 // Borrows the slot's existing descriptors and resident index. No source index
 // or payload is materialized. Advice coalesces source adjacency independently
 // of destinations; gathering coalesces only disjoint contiguous destinations.
 class UsedRangeCursor final {
 public:
  UsedRangeCursor() = default;
  [[nodiscard]] std::optional<UsedRange> next(std::size_t limit = read_chunk_bytes);
  [[nodiscard]] bool empty() const noexcept { return source_ == nullptr; }

 private:
  friend class CompiledDataset;
  const CompiledDataset* source_ = nullptr;
  std::span<const CompiledImageRead> reads_;
  std::size_t label_pitch_ = 0, image_ = 0, offset_ = 0;
  ReadSection section_ = ReadSection::Pixels;
  bool advice_ = false;
  void normalize();
 };
 [[nodiscard]] UsedRangeCursor used_ranges(std::span<const CompiledImageRead>, std::optional<std::size_t> label_pitch = {}, bool advice = false) const;
 void prepare_range(const UsedRange&, bool prefault = false) const;
 // Upper bound for useful gather records at the configured maximum batch.
 [[nodiscard]] static std::size_t gather_chunks(std::size_t images, std::size_t image_stride, std::size_t label_pitch = 0);
 struct ImageDestination {
  void* context;
  std::size_t capacity;
  void (*write)(void*, std::size_t, std::span<const std::byte>);
 };
 void gather_range(const UsedRange&, ImageDestination) const;
 [[nodiscard]] bool read_images_to(std::span<const CompiledImageRead>, ImageDestination, const std::atomic<bool>&, bool prefault = false) const;
 [[nodiscard]] bool read_images(std::span<const CompiledImageRead> reads, std::span<std::byte> destination, const std::atomic<bool>& cancelled, bool prefault = false) const;

private:
 [[nodiscard]] static std::expected<CompiledDataset, std::error_code> try_open(const std::filesystem::path&, std::size_t image_limit, AccessPattern, const CompiledDatasetOpenOptions&);
 std::filesystem::path path_;
 mmltk::common::io::MappedFile mapping_;
 mmltk::backend::data::FileHeader header_{};
 std::vector<ImageEntry> image_entries_;
 FileLayout layout_;
 std::size_t label_count_ = 0, run_count_ = 0;
 std::shared_ptr<const catalog::ClassCatalog> catalog_;
};
}  // namespace mmltk::backend::data
