#pragma once

#include "common_utils.h"
#include "compiled_format.h"

#include <cstddef>
#include <string>
#include <stdexcept>

namespace fastloader {

struct CompiledFileSections {
    size_t index_offset = 0;
    size_t label_offset = 0;
    size_t rle_offset = 0;
    size_t pixel_offset = 0;
    size_t expected_index_bytes = 0;
    size_t label_bytes = 0;
    size_t label_count = 0;
    size_t rle_region_bytes = 0;
    size_t pixel_blob_size = 0;
};

inline void validate_compiled_header(const FileHeader& header) {
    if (header.magic != MAGIC) {
        throw std::runtime_error("Bad magic in compiled file");
    }
    if (header.version != FORMAT_VERSION) {
        throw std::runtime_error("Version mismatch");
    }
}

inline FileHeader read_compiled_header(const FileHandle& file) {
    FileHeader header{};
    file.pread_all(&header, sizeof(header), 0);
    validate_compiled_header(header);
    return header;
}

inline FileHeader read_compiled_header(const std::string& path) {
    const FileHandle file = FileHandle::open_readonly(path);
    return read_compiled_header(file);
}

inline CompiledFileSections validate_compiled_file_sections(const FileHeader& header,
                                                            size_t file_size) {
    const size_t index_offset = checked_cast<size_t>(header.index_offset, "index offset overflow");
    const size_t label_offset = checked_cast<size_t>(header.label_offset, "label offset overflow");
    const size_t rle_offset = checked_cast<size_t>(header.mask_rle_offset, "RLE offset overflow");
    const size_t pixel_offset = checked_cast<size_t>(header.pixel_offset, "pixel offset overflow");
    const size_t total_file_size =
        checked_cast<size_t>(header.total_file_size, "file size overflow");
    if (total_file_size != file_size) {
        throw std::runtime_error("compiled file size does not match header");
    }
    if (pixel_offset % PAGE_SIZE != 0) {
        throw std::runtime_error("pixel blob offset must be page aligned");
    }
    // v4 layout: index < pixel < label < rle < total
    if (index_offset < sizeof(FileHeader) ||
        pixel_offset < index_offset ||
        label_offset < pixel_offset ||
        rle_offset < label_offset ||
        total_file_size < rle_offset) {
        throw std::runtime_error("compiled file layout is invalid");
    }

    const size_t expected_index_bytes =
        checked_cast<size_t>(static_cast<uint64_t>(header.num_images) * sizeof(ImageEntry),
                             "index size overflow");
    if (pixel_offset < index_offset + expected_index_bytes) {
        throw std::runtime_error("compiled file index block size mismatch");
    }

    const size_t label_bytes = rle_offset - label_offset;
    if (label_bytes % sizeof(PackedInstance) != 0) {
        throw std::runtime_error("label block is not aligned to PackedInstance");
    }

    const size_t pixel_blob_size = label_offset - pixel_offset;
    const size_t expected_pixel_blob_size =
        checked_cast<size_t>(static_cast<uint64_t>(header.num_images) * header.image_stride,
                             "pixel blob size overflow");
    if (pixel_blob_size != expected_pixel_blob_size) {
        throw std::runtime_error("compiled pixel blob size mismatch");
    }

    const size_t rle_region_bytes = total_file_size - rle_offset;

    return CompiledFileSections{
        index_offset,
        label_offset,
        rle_offset,
        pixel_offset,
        expected_index_bytes,
        label_bytes,
        label_bytes / sizeof(PackedInstance),
        rle_region_bytes,
        pixel_blob_size,
    };
}

} // namespace fastloader
