#include "dataset_compiler_internal.h"
#include "cpu_affinity.h"
#include "execution_policy.h"
#include "profile_utils.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace mmltk::compiler_internal {

int resolve_num_workers(int configured_workers) {
    std::vector<int> allowed;
    if (configured_workers > 0) {
        try {
            allowed = allowed_cpu_set();
            const int clamped = clamp_worker_count_to_cpus(configured_workers, allowed.size(), 0, 1);
            log_worker_budget_clamp("compile", configured_workers, clamped, allowed);
            return clamped;
        } catch (...) {
            return configured_workers;
        }
    }
    int available = 0;
    try {
        allowed = allowed_cpu_set();
        available = static_cast<int>(allowed.size());
    } catch (...) {
        available = static_cast<int>(std::thread::hardware_concurrency());
    }
    return std::max(1, available);
}

FileLayout compute_pixel_layout(uint32_t num_images, size_t image_stride) {
    FileLayout layout;
    layout.index_size = static_cast<size_t>(num_images) * sizeof(ImageEntry);
    layout.pixel_offset = align_up(layout.index_offset + layout.index_size, HUGE_PAGE_SIZE);
    layout.pixel_blob_size = static_cast<size_t>(num_images) * image_stride;
    return layout;
}

void finalize_layout(FileLayout& layout, const LayoutFinalizeInputs& inputs) {
    layout.label_block_size = inputs.label_count * sizeof(PackedInstance);
    layout.rle_block_size = inputs.rle_count * sizeof(RLEPair);
    layout.label_offset = layout.pixel_offset + layout.pixel_blob_size;
    layout.rle_offset = layout.label_offset + layout.label_block_size;
    layout.total_size = layout.rle_offset + layout.rle_block_size;
}

void assign_pixel_offsets(std::vector<ImageEntry>& index, size_t pixel_offset, size_t image_stride) {
    for (size_t i = 0; i < index.size(); ++i) {
        index[i].pixel_offset = pixel_offset + i * image_stride;
    }
}

FileHeader make_file_header(const FileHeaderInputs& inputs, const std::unordered_map<std::string, uint8_t>& class_map,
                            const FileLayout& layout) {
    FileHeader header{};
    header.magic = MAGIC;
    header.version = FORMAT_VERSION;
    header.num_images = inputs.num_images;
    header.image_width = inputs.width;
    header.image_height = inputs.height;
    header.channels = inputs.channels;
    header.num_classes = checked_cast<uint32_t>(class_map.size(), "too many classes for file header");
    header.index_offset = layout.index_offset;
    header.label_offset = layout.label_offset;
    header.pixel_offset = layout.pixel_offset;
    header.mask_rle_offset = layout.rle_offset;
    header.total_file_size = layout.total_size;
    header.image_stride = inputs.image_stride;
    for (const auto& [name, id] : class_map) {
        std::strncpy(header.class_names[id].data(), name.c_str(), header.class_names[id].size() - 1);
    }
    return header;
}

void write_metadata_blocks(const FileHandle& fd, const FileLayout& layout, const FileHeader& header,
                           const LabelBlocks& label_blocks) {
    {
        MMLTK_PROFILE_SCOPE("compiler.write_header");
        fd.pwrite_all(&header, sizeof(header), 0);
    }
    {
        MMLTK_PROFILE_SCOPE("compiler.write_index");
        fd.pwrite_all(label_blocks.index.data(), layout.index_size, layout.index_offset);
    }
    {
        MMLTK_PROFILE_SCOPE("compiler.write_labels");
        fd.pwrite_all(label_blocks.labels.data(), layout.label_block_size, layout.label_offset);
    }
    {
        MMLTK_PROFILE_SCOPE("compiler.write_rle");
        fd.pwrite_all(label_blocks.rle_pairs.data(), layout.rle_block_size, layout.rle_offset);
    }
}

}  // namespace mmltk::compiler_internal
