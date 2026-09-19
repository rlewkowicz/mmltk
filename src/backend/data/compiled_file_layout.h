#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/resample/image_resize.h"
namespace mmltk::backend::data {
struct LayoutFinalizeInputs {
    size_t label_count = 0;
    size_t rle_count = 0;
};
struct FileHeaderInputs {
    uint32_t num_images = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    uint32_t max_instances_per_image = 0;
    size_t image_stride = 0;
    mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
};
struct FileLayout {
    size_t index_size = 0;
    size_t label_block_size = 0;
    size_t rle_block_size = 0;
    size_t index_offset = sizeof(FileHeader);
    size_t label_offset = 0;
    size_t rle_offset = 0;
    size_t pixel_offset = 0;
    size_t pixel_blob_size = 0;
    size_t total_size = 0;
};
// Settle the pixel prefix before label work; finalization never moves that region.
[[nodiscard]] FileLayout compute_pixel_layout(uint32_t num_images, size_t image_stride);
void finalize_layout(FileLayout& layout, const LayoutFinalizeInputs& inputs);
[[nodiscard]] FileHeader make_file_header(const FileHeaderInputs& inputs, std::span<const std::string> class_names, const FileLayout& layout);
}  // namespace mmltk::backend::data
