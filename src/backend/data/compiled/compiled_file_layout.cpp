#include "src/backend/data/compiled/compiled_file_layout.h"
#include <limits>
#include <sys/types.h>
#include <cstring>
#include <stdexcept>
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
using namespace mmltk::common::math;
size_t compiled_align_up(size_t value, size_t alignment) {
 if (!alignment || (alignment & (alignment - 1U))) throw std::invalid_argument("compiled alignment must be a power of two");
 return checked_add(value, alignment - 1U, "compiled alignment overflow") & ~(alignment - 1U);
}
AnnotationLayout annotation_layout(size_t labels, size_t runs) {
 const auto offset = compiled_align_up(checked_multiply(labels, sizeof(PackedInstance), "compiled labels overflow"), sizeof(RLEPair));
 const auto bytes = checked_add(offset, checked_multiply(runs, sizeof(RLEPair), "compiled runs overflow"), "compiled label payload overflow");
 if (bytes > COMPILED_LABEL_CAPACITY) throw std::runtime_error("compiled label capacity exceeded: required " + std::to_string(bytes) + " bytes, capacity " + std::to_string(COMPILED_LABEL_CAPACITY));
 return {offset, bytes};
}
std::span<const RLEPair> ImageAnnotations::instance_rle(const PackedInstance& instance) const {
 if (instance.mask_rle_offset % sizeof(RLEPair)) throw std::runtime_error("unaligned compiled run offset");
 const auto begin = checked_cast<size_t>(instance.mask_rle_offset / sizeof(RLEPair), "compiled run offset overflow");
 if (begin > runs.size() || instance.mask_rle_pairs > runs.size() - begin) throw std::out_of_range("compiled instance run span");
 return runs.subspan(begin, instance.mask_rle_pairs);
}
ImageAnnotations annotation_view(const ImageEntry& entry, std::span<const std::byte> payload) {
 const auto layout = annotation_layout(entry.num_instances, entry.num_runs);
 if (entry.label_bytes != layout.used_bytes || payload.size() < layout.used_bytes) throw std::runtime_error("compiled label payload extent differs");
 if (!layout.used_bytes) return {};
 return {{reinterpret_cast<const PackedInstance*>(payload.data()), entry.num_instances},
  {reinterpret_cast<const RLEPair*>(payload.data() + layout.run_offset), entry.num_runs}};
}
FileLayout compute_file_layout(uint32_t slots, size_t stride) {
 FileLayout layout;
 layout.num_slots = slots;
 layout.image_stride = stride;
 if (!stride || stride % sizeof(float)) throw std::invalid_argument("invalid compiled pixel stride");
 layout.pixel_pitch = compiled_align_up(stride, PAGE_SIZE);
 layout.label_pitch = compiled_align_up(COMPILED_LABEL_CAPACITY, PAGE_SIZE);
 layout.pixel_block_bytes = checked_multiply(COMPILED_BLOCK_SLOTS, layout.pixel_pitch, "compiled pixel block overflow");
 layout.block_bytes = compiled_align_up(checked_multiply(COMPILED_BLOCK_SLOTS, checked_add(layout.pixel_pitch, layout.label_pitch, "compiled slot extent overflow"), "compiled block overflow"), HUGE_PAGE_SIZE);
 layout.data_offset = compiled_align_up(sizeof(FileHeader), HUGE_PAGE_SIZE);
 layout.index_size = checked_multiply(size_t{slots}, sizeof(ImageEntry), "compiled index overflow");
 if (layout.index_size > COMPILED_METADATA_CAPACITY) throw std::runtime_error("compiled resident metadata exceeds 1000000000 bytes");
 const auto blocks = layout.block_count();
 layout.index_offset = layout.block_offset(blocks);
 layout.total_size = checked_add(layout.index_offset, layout.index_size, "compiled file extent overflow");
 (void)checked_cast<off_t>(layout.total_size, "compiled file exceeds native offset representation");
 return layout;
}
size_t FileLayout::block_offset(size_t block) const { return checked_add(data_offset, checked_multiply(block, block_bytes, "compiled block offset overflow"), "compiled data offset overflow"); }
size_t FileLayout::pixel_offset(uint32_t slot) const {
 if (slot >= num_slots) throw std::out_of_range("compiled pixel slot");
 return checked_add(block_offset(slot / COMPILED_BLOCK_SLOTS), checked_multiply(size_t{slot} % COMPILED_BLOCK_SLOTS, pixel_pitch, "compiled pixel offset overflow"), "compiled pixel extent overflow");
}
size_t FileLayout::label_offset(uint32_t slot) const {
 if (slot >= num_slots) throw std::out_of_range("compiled label slot");
 const auto prefix = pixel_block_bytes;
 return checked_add(checked_add(block_offset(slot / COMPILED_BLOCK_SLOTS), prefix, "compiled label base overflow"), checked_multiply(size_t{slot} % COMPILED_BLOCK_SLOTS, label_pitch, "compiled label offset overflow"), "compiled label extent overflow");
}
FileHeader make_file_header(const FileHeaderInputs& inputs, std::span<const std::string> classes, const FileLayout& layout) {
 FileHeader header{};
 header.magic = MAGIC;
 header.version = FORMAT_VERSION;
 header.resize_mode = inputs.resize_mode;
 header.num_images = inputs.num_images;
 header.num_slots = layout.num_slots;
 header.image_width = inputs.width;
 header.image_height = inputs.height;
 header.channels = inputs.channels;
 header.num_classes = checked_cast<uint32_t>(classes.size(), "too many classes for file header");
 header.max_instances_per_image = inputs.max_instances_per_image;
 header.index_offset = layout.index_offset;
 header.data_offset = layout.data_offset;
 header.total_file_size = layout.total_size;
 header.image_stride = inputs.image_stride;
 if (!inputs.num_images || inputs.num_images > layout.num_slots || inputs.image_stride != layout.image_stride) throw std::runtime_error("invalid compiled slot population");
 if (classes.empty() || classes.size() > MAX_CLASSES) throw std::runtime_error("invalid compiled class count");
 if (inputs.max_instances_per_image > std::numeric_limits<std::uint16_t>::max()) throw std::runtime_error("compiled maximum instance count exceeds the index representation");
 for (std::size_t id = 0; id < classes.size(); ++id) {
  const auto& name = classes[id];
  if (name.empty() || name.size() > COMPILED_CLASS_NAME_CAPACITY || name.find('\0') != std::string::npos) throw std::runtime_error("invalid compiled class name or index");
  std::memcpy(header.class_names[id].data(), name.data(), name.size());
 }
 return header;
}
}  // namespace mmltk::backend::data
