#pragma once

#include "common_utils.h"
#include "compiled_format.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace mmltk {

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

struct CompiledDatasetInfo {
    std::filesystem::path path;
    std::uint32_t image_count = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::vector<std::string> class_names;
};

inline void validate_compiled_header(const FileHeader& header) {
    if (header.magic != MAGIC) {
        throw std::runtime_error("Bad magic in compiled file");
    }
    if (header.version != FORMAT_VERSION) {
        throw std::runtime_error("Version mismatch");
    }
    if (header.num_images == 0U || header.image_width == 0U || header.image_height == 0U || header.channels == 0U) {
        throw std::runtime_error("compiled file has invalid image dimensions");
    }
    if (header.num_classes == 0U || header.num_classes > MAX_CLASSES) {
        throw std::runtime_error("compiled file has invalid class count");
    }
    const uint64_t expected_image_stride = static_cast<uint64_t>(header.image_width) * header.image_height *
                                           header.channels * sizeof(float);
    if (header.image_stride != expected_image_stride) {
        throw std::runtime_error("compiled file has an invalid image stride");
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

inline CompiledFileSections validate_compiled_file_sections(const FileHeader& header, size_t file_size) {
    const auto index_offset = checked_cast<size_t>(header.index_offset, "index offset overflow");
    const auto label_offset = checked_cast<size_t>(header.label_offset, "label offset overflow");
    const auto rle_offset = checked_cast<size_t>(header.mask_rle_offset, "RLE offset overflow");
    const auto pixel_offset = checked_cast<size_t>(header.pixel_offset, "pixel offset overflow");
    const auto total_file_size = checked_cast<size_t>(header.total_file_size, "file size overflow");
    if (total_file_size != file_size) {
        throw std::runtime_error("compiled file size does not match header");
    }
    if (index_offset != sizeof(FileHeader) || pixel_offset < index_offset || label_offset < pixel_offset ||
        rle_offset < label_offset || total_file_size < rle_offset) {
        throw std::runtime_error("compiled file layout is invalid");
    }

    const auto expected_index_bytes =
        checked_cast<size_t>(static_cast<uint64_t>(header.num_images) * sizeof(ImageEntry), "index size overflow");
    if (pixel_offset != align_up(index_offset + expected_index_bytes, HUGE_PAGE_SIZE)) {
        throw std::runtime_error("compiled file index or pixel alignment is invalid");
    }

    const size_t label_bytes = rle_offset - label_offset;
    if (label_bytes % sizeof(PackedInstance) != 0) {
        throw std::runtime_error("label block is not aligned to PackedInstance");
    }

    const size_t pixel_blob_size = label_offset - pixel_offset;
    const auto expected_pixel_blob_size = checked_cast<size_t>(
        static_cast<uint64_t>(header.num_images) * header.image_stride, "pixel blob size overflow");
    if (pixel_blob_size != expected_pixel_blob_size) {
        throw std::runtime_error("compiled pixel blob size mismatch");
    }

    const size_t rle_region_bytes = total_file_size - rle_offset;
    if (rle_region_bytes % sizeof(RLEPair) != 0U) {
        throw std::runtime_error("compiled RLE block is not aligned to RLEPair");
    }

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

inline void validate_compiled_index_entries(const std::span<const ImageEntry> index, const FileHeader& header,
                                            const size_t label_count,
                                            const std::atomic<bool>* cancel_requested = nullptr) {
    if (index.size() != header.num_images) {
        throw std::runtime_error("compiled index entry count does not match header");
    }
    size_t expected_label_offset = 0U;
    for (size_t image_index = 0U; image_index < index.size(); ++image_index) {
        if ((image_index & 4095U) == 0U && cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            throw std::runtime_error("dataset compilation cancelled");
        }
        const ImageEntry& entry = index[image_index];
        const uint64_t expected_pixel_offset =
            header.pixel_offset + static_cast<uint64_t>(image_index) * header.image_stride;
        if (entry.pixel_offset != expected_pixel_offset) {
            throw std::runtime_error("compiled pixel index is inconsistent at image " +
                                     std::to_string(image_index));
        }
        if (entry.label_offset != expected_label_offset || entry.label_offset % sizeof(PackedInstance) != 0U ||
            entry.label_bytes != static_cast<uint32_t>(entry.num_instances) * sizeof(PackedInstance)) {
            throw std::runtime_error("compiled label index is inconsistent at image " +
                                     std::to_string(image_index));
        }
        const size_t label_begin = entry.label_offset / sizeof(PackedInstance);
        if (label_begin > label_count || static_cast<size_t>(entry.num_instances) > label_count - label_begin) {
            throw std::runtime_error("compiled label span is out of bounds at image " +
                                     std::to_string(image_index));
        }
        expected_label_offset += entry.label_bytes;
    }
    if (expected_label_offset != label_count * sizeof(PackedInstance)) {
        throw std::runtime_error("compiled label index does not reference the complete label block");
    }
}

[[nodiscard]] inline size_t validate_compiled_label_entries(const std::span<const PackedInstance> labels,
                                                            const FileHeader& header,
                                                            const size_t rle_region_bytes,
                                                            const std::atomic<bool>* cancel_requested = nullptr) {
    size_t used_rle_bytes = 0U;
    for (size_t label_index = 0U; label_index < labels.size(); ++label_index) {
        if ((label_index & 4095U) == 0U && cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            throw std::runtime_error("dataset compilation cancelled");
        }
        const PackedInstance& instance = labels[label_index];
        if (instance.class_id >= header.num_classes) {
            throw std::runtime_error("compiled instance class id is out of bounds at label " +
                                     std::to_string(label_index));
        }
        if (instance.bbox_x1 < 0 || instance.bbox_y1 < 0 || instance.bbox_x2 <= instance.bbox_x1 ||
            instance.bbox_y2 <= instance.bbox_y1 ||
            static_cast<std::uint32_t>(instance.bbox_x2) > header.image_width ||
            static_cast<std::uint32_t>(instance.bbox_y2) > header.image_height) {
            throw std::runtime_error("compiled instance bounding box is invalid at label " +
                                     std::to_string(label_index));
        }
        if (instance.mask_rle_offset != used_rle_bytes || instance.mask_rle_offset % sizeof(RLEPair) != 0U ||
            instance.mask_rle_pairs == 0U) {
            throw std::runtime_error("compiled instance RLE metadata is invalid at label " +
                                     std::to_string(label_index));
        }
        const size_t rle_bytes = static_cast<size_t>(instance.mask_rle_pairs) * sizeof(RLEPair);
        if (instance.mask_rle_offset > rle_region_bytes || rle_bytes > rle_region_bytes - instance.mask_rle_offset) {
            throw std::runtime_error("compiled instance RLE span is out of bounds at label " +
                                     std::to_string(label_index));
        }
        used_rle_bytes += rle_bytes;
    }
    return used_rle_bytes;
}

inline void validate_compiled_rle_pairs(const std::span<const PackedInstance> labels,
                                        const std::span<const RLEPair> pairs, const size_t mask_pixels,
                                        const std::atomic<bool>* cancel_requested = nullptr) {
    for (size_t label_index = 0U; label_index < labels.size(); ++label_index) {
        if ((label_index & 4095U) == 0U && cancel_requested != nullptr &&
            cancel_requested->load(std::memory_order_relaxed)) {
            throw std::runtime_error("dataset compilation cancelled");
        }
        const PackedInstance& instance = labels[label_index];
        const size_t first_pair = instance.mask_rle_offset / sizeof(RLEPair);
        const size_t pair_count = instance.mask_rle_pairs;
        if (first_pair > pairs.size() || pair_count > pairs.size() - first_pair) {
            throw std::runtime_error("compiled RLE pair span is invalid at label " + std::to_string(label_index));
        }
        size_t previous_end = 0U;
        for (size_t local_pair_index = 0U; local_pair_index < pair_count; ++local_pair_index) {
            if ((local_pair_index & 4095U) == 0U && cancel_requested != nullptr &&
                cancel_requested->load(std::memory_order_relaxed)) {
                throw std::runtime_error("dataset compilation cancelled");
            }
            const RLEPair& pair = pairs[first_pair + local_pair_index];
            const size_t start = pair.start;
            const size_t length = pair.length;
            if (length == 0U || start < previous_end || start > mask_pixels || length > mask_pixels - start) {
                throw std::runtime_error("compiled RLE run is invalid at label " + std::to_string(label_index) +
                                         ", pair " + std::to_string(local_pair_index));
            }
            previous_end = start + length;
        }
    }
}

inline CompiledDatasetInfo inspect_compiled_dataset(const std::filesystem::path& path) {
    const FileHandle file = FileHandle::open_readonly(path.string());
    const FileHeader header = read_compiled_header(file);
    (void)validate_compiled_file_sections(header, file.size());

    CompiledDatasetInfo info;
    info.path = path;
    info.image_count = header.num_images;
    info.width = header.image_width;
    info.height = header.image_height;
    info.channels = header.channels;
    info.class_names.reserve(header.num_classes);
    for (std::uint32_t index = 0U; index < header.num_classes; ++index) {
        const auto& stored_name = header.class_names[index];
        const std::size_t length = ::strnlen(stored_name.data(), stored_name.size());
        if (length == 0U || length == stored_name.size()) {
            throw std::runtime_error("compiled file contains an empty or unterminated class name at index " +
                                     std::to_string(index));
        }
        std::string name(stored_name.data(), length);
        if (std::ranges::find(info.class_names, name) != info.class_names.end()) {
            throw std::runtime_error("compiled file contains duplicate class name '" + name + "'");
        }
        info.class_names.push_back(std::move(name));
    }
    return info;
}

}  
