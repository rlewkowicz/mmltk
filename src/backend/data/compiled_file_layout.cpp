#include "src/backend/data/compiled_file_layout.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
using mmltk::common::math::checked_cast;
FileLayout compute_pixel_layout(uint32_t num_images, size_t image_stride) {
    FileLayout layout;
    layout.index_size = mmltk::common::math::checked_multiply(static_cast<size_t>(num_images), sizeof(ImageEntry), "compiled index size overflow");
    const auto index_end = mmltk::common::math::checked_add(layout.index_offset, layout.index_size, "compiled index end overflow");
    layout.pixel_offset = mmltk::common::math::checked_add(index_end, HUGE_PAGE_SIZE - 1U, "compiled alignment overflow") & ~(HUGE_PAGE_SIZE - 1U);
    layout.pixel_blob_size = mmltk::common::math::checked_multiply(static_cast<size_t>(num_images), image_stride, "compiled pixels overflow");
    (void)mmltk::common::math::checked_add(layout.pixel_offset, layout.pixel_blob_size, "compiled pixel end overflow");
    return layout;
}
void finalize_layout(FileLayout& layout, const LayoutFinalizeInputs& inputs) {
    layout.label_block_size = mmltk::common::math::checked_multiply(inputs.label_count, sizeof(PackedInstance), "compiled layout overflow");
    layout.rle_block_size = mmltk::common::math::checked_multiply(inputs.rle_count, sizeof(RLEPair), "compiled layout overflow");
    layout.label_offset = mmltk::common::math::checked_add(layout.pixel_offset, layout.pixel_blob_size, "compiled layout overflow");
    layout.rle_offset = mmltk::common::math::checked_add(layout.label_offset, layout.label_block_size, "compiled layout overflow");
    layout.total_size = mmltk::common::math::checked_add(layout.rle_offset, layout.rle_block_size, "compiled layout overflow");
}
FileHeader make_file_header(const FileHeaderInputs& inputs, const std::span<const std::string> class_names, const FileLayout& layout) {
    FileHeader header{};
    header.magic = MAGIC;
    header.version = FORMAT_VERSION;
    header.resize_mode = inputs.resize_mode;
    header.num_images = inputs.num_images;
    header.image_width = inputs.width;
    header.image_height = inputs.height;
    header.channels = inputs.channels;
    header.num_classes = checked_cast<uint32_t>(class_names.size(), "too many classes for file header");
    header.max_instances_per_image = inputs.max_instances_per_image;
    header.index_offset = layout.index_offset;
    header.label_offset = layout.label_offset;
    header.pixel_offset = layout.pixel_offset;
    header.mask_rle_offset = layout.rle_offset;
    header.total_file_size = layout.total_size;
    header.image_stride = inputs.image_stride;
    if (class_names.empty() || class_names.size() > MAX_CLASSES) throw std::runtime_error("invalid compiled class count");
    if (inputs.max_instances_per_image > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error("compiled maximum instance count exceeds the index representation");
    for (std::size_t id = 0; id < class_names.size(); ++id) {
        const auto& name = class_names[id];
        if (name.empty() || name.size() > COMPILED_CLASS_NAME_CAPACITY || name.find('\0') != std::string::npos)
            throw std::runtime_error("invalid compiled class name or index");
        std::memcpy(header.class_names[id].data(), name.data(), name.size());
    }
    return header;
}
}  // namespace mmltk::backend::data
