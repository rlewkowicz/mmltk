#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "src/backend/data/compiled_format.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"

// CLEANUP-IGNORE: This layout implementation imports the concrete data and system owners used by its independent unit.

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

#include "detail/dataset_compiler_internal.h"

namespace mmltk::backend::data::compiler_internal {

using mmltk::common::io::FileHandle;
using mmltk::common::math::checked_cast;
using mmltk::common::system::clamp_worker_count_to_cpus;

int resolve_num_workers(int configured_workers, const std::span<const int> worker_cpus) {
    if (worker_cpus.empty()) { throw std::runtime_error("dataset compilation requires a non-empty CPU execution domain"); }
    const std::vector<int> allowed(worker_cpus.begin(), worker_cpus.end());
    if (configured_workers > 0) {
        const int clamped = clamp_worker_count_to_cpus(configured_workers, allowed.size(), 0, 1);
        if (configured_workers != clamped) {
            mmltk::common::logging::warn([&](spdlog::logger& log) {
                log.warn("compile workers clamped {}->{} for cpuset={} (reserved=0 minimum=1)", configured_workers, clamped,
                         mmltk::common::system::format_cpu_list(allowed));
            });
        }
        return clamped;
    }
    return static_cast<int>(allowed.size());
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
    header.max_instances_per_image = inputs.max_instances_per_image;
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

void write_metadata_blocks(const FileHandle& fd, const FileLayout& layout, const FileHeader& header, const LabelBlocks& label_blocks,
                           mmltk::common::concurrency::CancellationObservation cancel_requested) {
    constexpr size_t kWriteChunkBytes = size_t{16U} * 1024U * 1024U;
    const auto write_block = [&](const void* source, const size_t bytes, const size_t offset) {
        const auto* source_bytes = static_cast<const std::uint8_t*>(source);
        size_t written = 0U;
        while (written < bytes) {
            if (cancel_requested.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
            const size_t chunk = std::min(kWriteChunkBytes, bytes - written);
            fd.pwrite_all(source_bytes + written, chunk, offset + written);
            written += chunk;
        }
    };
    {
        mmltk::common::logging::ScopedProfile profile{"compiler.write_header"};
        write_block(&header, sizeof(header), 0);
    }
    {
        mmltk::common::logging::ScopedProfile profile{"compiler.write_index"};
        write_block(label_blocks.index.data(), layout.index_size, layout.index_offset);
    }
    {
        mmltk::common::logging::ScopedProfile profile{"compiler.write_labels"};
        write_block(label_blocks.labels.data(), layout.label_block_size, layout.label_offset);
    }
    {
        mmltk::common::logging::ScopedProfile profile{"compiler.write_rle"};
        write_block(label_blocks.rle_pairs.data(), layout.rle_block_size, layout.rle_offset);
    }
}

}  // namespace mmltk::backend::data::compiler_internal
