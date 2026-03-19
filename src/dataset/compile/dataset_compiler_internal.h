#pragma once

#include "compiled_format.h"
#include "common_utils.h"
#include "dataset_compiler.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastloader::compiler_internal {

struct DatasetScan {
    std::unordered_map<std::string, uint8_t> class_map;
    uint32_t num_images = 0;
};

struct LabelBlocks {
    std::vector<ImageEntry> index;
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
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

int resolve_num_workers(int configured_workers);

std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index);

DatasetScan scan_dataset(const CompilerConfig& config);
LabelBlocks build_label_blocks(const std::filesystem::path& split_dir,
                               uint32_t num_images,
                               uint32_t target_width,
                               uint32_t target_height,
                               const std::unordered_map<std::string, uint8_t>& class_map,
                               int num_workers,
                               const std::function<void(size_t, size_t)>& progress_cb = nullptr);

FileLayout compute_layout(uint32_t num_images,
                          size_t image_stride,
                          size_t label_count,
                          size_t rle_count);
void assign_pixel_offsets(std::vector<ImageEntry>& index,
                          size_t pixel_offset,
                          size_t image_stride);
FileHeader make_file_header(uint32_t num_images,
                            uint32_t width,
                            uint32_t height,
                            uint32_t channels,
                            size_t image_stride,
                            const std::unordered_map<std::string, uint8_t>& class_map,
                            const FileLayout& layout);
void write_metadata_blocks(const FileHandle& fd,
                           const FileLayout& layout,
                           const FileHeader& header,
                           const LabelBlocks& label_blocks);

void write_pixel_blob(const FileHandle& fd,
                      const std::filesystem::path& split_dir,
                      uint32_t num_images,
                      uint32_t width,
                      uint32_t height,
                      size_t image_stride,
                      int num_workers,
                      size_t pixel_offset,
                      const std::function<void(size_t, size_t)>& progress_cb);

} // namespace fastloader::compiler_internal
