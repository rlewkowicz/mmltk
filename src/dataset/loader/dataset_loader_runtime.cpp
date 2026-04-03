#include "compiled_file_utils.h"
#include "dataset_loader_internal.h"
#include "profile_utils.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace mmltk {

namespace {

void populate_label_index(OwnedBuffer<LabelIndexEntry>& label_index,
                          const std::vector<ImageEntry>& disk_index,
                          const FileHeader& header,
                          size_t label_count) {
    MMLTK_PROFILE_SCOPE("loader.load_runtime.populate_label_index");
    for (size_t i = 0; i < disk_index.size(); ++i) {
        const ImageEntry& entry = disk_index[i];
        const uint64_t expected_pixel_entry =
            header.pixel_offset + static_cast<uint64_t>(i) * header.image_stride;
        if (entry.pixel_offset != expected_pixel_entry) {
            throw std::runtime_error("compiled pixel index is inconsistent");
        }
        if (entry.label_offset % sizeof(PackedInstance) != 0) {
            throw std::runtime_error("image label offset is not aligned to PackedInstance");
        }
        if (entry.label_bytes != static_cast<uint32_t>(entry.num_instances) * sizeof(PackedInstance)) {
            throw std::runtime_error("image label byte count is inconsistent");
        }

        const uint32_t label_begin = entry.label_offset / sizeof(PackedInstance);
        if (label_begin > label_count ||
            static_cast<size_t>(entry.num_instances) > label_count - label_begin) {
            throw std::runtime_error("image label span is out of bounds");
        }

        label_index.data()[i] = LabelIndexEntry{
            label_begin,
            entry.num_instances,
            0,
        };
    }
}

size_t compute_used_rle_bytes(const OwnedBuffer<PackedInstance>& labels, size_t rle_region_bytes) {
    MMLTK_PROFILE_SCOPE("loader.load_runtime.compute_used_rle_bytes");
    size_t used_rle_bytes = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        const PackedInstance& inst = labels.data()[i];
        const size_t rle_bytes = static_cast<size_t>(inst.mask_rle_pairs) * sizeof(RLEPair);
        if (inst.mask_rle_offset > rle_region_bytes ||
            rle_bytes > rle_region_bytes - inst.mask_rle_offset) {
            throw std::runtime_error("instance RLE span is out of bounds");
        }
        used_rle_bytes =
            std::max(used_rle_bytes, static_cast<size_t>(inst.mask_rle_offset) + rle_bytes);
    }
    return used_rle_bytes;
}

} // namespace

void DatasetLoader::Impl::load_runtime_data() {
    MMLTK_PROFILE_SCOPE("loader.load_runtime_data");
    const FileHandle file = FileHandle::open_readonly(config.compiled_path);
    const size_t file_size = file.size();
    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.read_header");
        header = read_compiled_header(file);
    }

    CompiledFileSections sections;
    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.validate_sections");
        sections = validate_compiled_file_sections(header, file_size);
    }

    std::vector<ImageEntry> disk_index(header.num_images);
    if (!disk_index.empty()) {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.read_disk_index");
        file.pread_all(disk_index.data(), sections.expected_index_bytes, sections.index_offset);
    }

    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.allocate_label_index");
        label_index = allocate_hugepage_buffer<LabelIndexEntry>(header.num_images);
    }
    MMLTK_PROFILE_SET("loader.label_index_bytes", label_index.bytes());
    populate_label_index(label_index, disk_index, header, sections.label_count);

    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.read_labels");
        labels = load_hugepage_block<PackedInstance>(file, sections.label_count, sections.label_offset);
    }
    MMLTK_PROFILE_SET("loader.label_bytes", labels.bytes());

    const size_t used_rle_bytes = compute_used_rle_bytes(labels, sections.rle_region_bytes);
    if (used_rle_bytes % sizeof(RLEPair) != 0) {
        throw std::runtime_error("RLE block is not aligned to RLEPair");
    }

    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.read_rle");
        rle_pairs =
            load_hugepage_block<RLEPair>(file, used_rle_bytes / sizeof(RLEPair), sections.rle_offset);
    }
    MMLTK_PROFILE_SET("loader.rle_bytes", rle_pairs.bytes());

    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.drop_metadata_pages");
        file.advise(0, sections.pixel_offset, POSIX_FADV_DONTNEED);
    }

    {
        MMLTK_PROFILE_SCOPE("loader.load_runtime.map_pixel_blob");
        pixel_file = MappedFile::open_readonly_range(config.compiled_path,
                                                     sections.pixel_offset,
                                                     sections.pixel_blob_size);
    }
    // cppcheck-suppress invalidPointerCast
    pixels = reinterpret_cast<const float*>(pixel_file.data());

    MMLTK_DEBUG_LOG(
        "[loader] labels in RAM: index=%zu bytes labels=%zu bytes rle=%zu bytes pixel_mmap=%zu bytes\n",
        label_index.bytes(),
        labels.bytes(),
        rle_pairs.bytes(),
        sections.pixel_blob_size);
}

void DatasetLoader::Impl::prescan() {
    MMLTK_PROFILE_SCOPE("loader.prescan.total");
    const size_t pixel_blob_size = static_cast<size_t>(header.num_images) * header.image_stride;
    MMLTK_PROFILE_SET("loader.pixel_blob_bytes", pixel_blob_size);
    {
        MMLTK_PROFILE_SCOPE("loader.prescan.advise_hugepage");
        pixel_file.advise_range(0, pixel_blob_size, MADV_HUGEPAGE);
    }
    {
        MMLTK_PROFILE_SCOPE("loader.prescan.advise_access_pattern");
        if (config.shuffle) {
            pixel_file.advise_range(0, pixel_blob_size, MADV_NORMAL);
        } else {
            pixel_file.advise_range(0, pixel_blob_size, MADV_SEQUENTIAL);
        }
    }
}

} // namespace mmltk
