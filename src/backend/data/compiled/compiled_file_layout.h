#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include "src/backend/data/compiled/compiled_format.h"
namespace mmltk::backend::data {
struct FileHeaderInputs {
 uint32_t num_images = 0;
 uint32_t width = 0;
 uint32_t height = 0;
 uint32_t channels = 0;
 uint32_t max_instances_per_image = 0;
 size_t image_stride = 0;
 mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
};
// All persisted arithmetic belongs here; slots remain stable as capacity grows.
struct FileLayout {
 std::uint32_t num_slots = 0;
 size_t image_stride = 0;
 size_t pixel_pitch = 0;
 size_t label_pitch = 0;
 size_t pixel_block_bytes = 0;
 size_t block_bytes = 0;
 size_t data_offset = 0;
 size_t index_size = 0;
 size_t index_offset = 0;
 size_t total_size = 0;
 [[nodiscard]] size_t block_count() const noexcept { return (size_t{num_slots} + COMPILED_BLOCK_SLOTS - 1U) / COMPILED_BLOCK_SLOTS; }
 [[nodiscard]] size_t block_index(std::uint32_t slot) const noexcept { return slot / COMPILED_BLOCK_SLOTS; }
 [[nodiscard]] size_t block_offset(std::size_t block) const;
 [[nodiscard]] size_t pixel_offset(std::uint32_t slot) const;
 [[nodiscard]] size_t label_offset(std::uint32_t slot) const;
};
struct AnnotationLayout {
 size_t run_offset = 0;
 size_t used_bytes = 0;
};
struct ImageAnnotations {
 std::span<const PackedInstance> labels;
 std::span<const RLEPair> runs;
 [[nodiscard]] std::span<const RLEPair> instance_rle(const PackedInstance& instance) const;
};
[[nodiscard]] size_t compiled_align_up(size_t value, size_t alignment);
[[nodiscard]] AnnotationLayout annotation_layout(size_t labels, size_t runs);
[[nodiscard]] ImageAnnotations annotation_view(const ImageEntry&, std::span<const std::byte> payload);
[[nodiscard]] FileLayout compute_file_layout(uint32_t num_slots, size_t image_stride);
[[nodiscard]] FileHeader make_file_header(const FileHeaderInputs&, std::span<const std::string> class_names, const FileLayout&);
}  // namespace mmltk::backend::data
