#include "src/backend/data/compiled/compiled_file_layout.h"
#include <limits>
#include <cmath>
#include <algorithm>
#include <sys/types.h>
#include <cstring>
#include <stdexcept>
#include "src/frameworks/serialization/serialization.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data {
using namespace mmltk::common::math;
size_t compiled_align_up(size_t value, size_t alignment) {
 if (!alignment || (alignment & (alignment - 1U))) throw std::invalid_argument("compiled alignment must be a power of two");
 return checked_add(value, alignment - 1U, "compiled alignment overflow") & ~(alignment - 1U);
}
AnnotationLayout annotation_layout(size_t labels, size_t runs, size_t capacity, size_t joints) {
 if (capacity > COMPILED_LABEL_LIMIT || capacity % sizeof(RLEPair)) throw std::invalid_argument("invalid compiled label capacity");
 AnnotationLayout layout;
 layout.run_offset = compiled_align_up(checked_multiply(labels, sizeof(PackedInstance), "compiled labels overflow"), sizeof(RLEPair));
 auto bytes = checked_add(layout.run_offset, checked_multiply(runs, sizeof(RLEPair), "compiled runs overflow"), "compiled label payload overflow");
 if (joints) {
  layout.joint_span_offset = checked_add(bytes, sizeof(PackedJointHeader), "compiled joint header overflow");
  layout.joint_offset = checked_add(layout.joint_span_offset, checked_multiply(labels, sizeof(PackedJointSpan), "compiled joint spans overflow"), "compiled joint offset overflow");
  bytes = compiled_align_up(checked_add(layout.joint_offset, checked_multiply(joints, sizeof(PackedKeypoint), "compiled joints overflow"), "compiled joint extent overflow"), sizeof(RLEPair));
 }
 if (bytes > capacity) throw std::runtime_error("compiled label capacity exceeded: required " + std::to_string(bytes) + " bytes, capacity " + std::to_string(capacity));
 layout.used_bytes = bytes;
 return layout;
}
void ImageAnnotations::validate_joints(const catalog::ClassCatalog* schema) const {
 if (joints.empty() && joint_spans.empty() && (!schema || !schema->has_keypoints())) return;
 if ((!joints.empty() && joint_spans.size() != labels.size()) || (joints.empty() && !joint_spans.empty())) throw std::runtime_error("compiled joint span count differs");
 std::size_t consumed = 0;
 for (std::size_t index = 0; index < labels.size(); ++index) {
  const auto points = instance_joints(index);
  if ((!joint_spans.empty() && joint_spans[index].offset != consumed) || points.size() > catalog::kKeypointsPerClassCapacity) throw std::runtime_error("invalid compiled joint span");
  if (schema && points.size() != schema->keypoints(labels[index].class_id).names.size()) throw std::runtime_error("compiled joints disagree with class schema");
  consumed += points.size();
 }
 if (consumed != joints.size()) throw std::runtime_error("unreferenced compiled joints");
 for (const auto& joint : joints)
  if (!std::isfinite(joint.x) || !std::isfinite(joint.y) || !mmltk::frameworks::reflection::enum_contains(joint.visibility) ||
      (joint.visibility == catalog::KeypointVisibility::Unannotated && (joint.x != 0 || joint.y != 0)) || !std::ranges::all_of(joint.reserved, [](auto value) { return value == 0; }))
   throw std::runtime_error("invalid compiled keypoint");
}
std::span<const PackedKeypoint> ImageAnnotations::instance_joints(std::size_t instance) const {
 if (instance >= labels.size()) throw std::out_of_range("compiled joint instance");
 if (joint_spans.empty()) return {};
 if (instance >= joint_spans.size()) throw std::out_of_range("compiled joint span index");
 const auto& span = joint_spans[instance];
 if (span.reserved || span.offset > joints.size() || span.count > joints.size() - span.offset) throw std::runtime_error("compiled joint span exceeds payload");
 return joints.subspan(span.offset, span.count);
}
std::span<const RLEPair> ImageAnnotations::instance_rle(const PackedInstance& instance) const {
 if (instance.mask_rle_offset % sizeof(RLEPair)) throw std::runtime_error("unaligned compiled run offset");
 const auto begin = checked_cast<size_t>(instance.mask_rle_offset / sizeof(RLEPair), "compiled run offset overflow");
 if (begin > runs.size() || instance.mask_rle_pairs > runs.size() - begin) throw std::out_of_range("compiled instance run span");
 return runs.subspan(begin, instance.mask_rle_pairs);
}
ImageAnnotations annotation_view(const ImageEntry& entry, std::span<const std::byte> payload, const catalog::ClassCatalog* schema) {
 auto layout = annotation_layout(entry.num_instances, entry.num_runs);
 if (payload.size() < entry.label_bytes || entry.label_bytes < layout.used_bytes) throw std::runtime_error("compiled label payload extent differs");
 if (!entry.label_bytes) return {};
 ImageAnnotations result{{reinterpret_cast<const PackedInstance*>(payload.data()), entry.num_instances}, {reinterpret_cast<const RLEPair*>(payload.data() + layout.run_offset), entry.num_runs}};
 if (entry.label_bytes > layout.used_bytes) {
  if (entry.label_bytes - layout.used_bytes < sizeof(PackedJointHeader)) throw std::runtime_error("truncated compiled joint header");
  PackedJointHeader header;
  std::memcpy(&header, payload.data() + layout.used_bytes, sizeof(header));
  if (header.version != 1 || !header.count) throw std::runtime_error("invalid compiled joint header");
  layout = annotation_layout(entry.num_instances, entry.num_runs, COMPILED_LABEL_LIMIT, header.count);
  if (entry.label_bytes != layout.used_bytes) throw std::runtime_error("compiled joint payload extent differs");
  result.joint_spans = {reinterpret_cast<const PackedJointSpan*>(payload.data() + layout.joint_span_offset), entry.num_instances};
  result.joints = {reinterpret_cast<const PackedKeypoint*>(payload.data() + layout.joint_offset), header.count};
 }
 result.validate_joints(schema);
 return result;
}
FileLayout compute_file_layout(uint32_t slots, size_t stride, size_t label_capacity, size_t catalog_bytes) {
 FileLayout layout;
 layout.num_slots = slots;
 layout.image_stride = stride;
 if (!stride || stride % sizeof(float)) throw std::invalid_argument("invalid compiled pixel stride");
 if (label_capacity > COMPILED_LABEL_LIMIT || label_capacity % sizeof(RLEPair)) throw std::invalid_argument("invalid compiled label capacity");
 layout.pixel_pitch = compiled_align_up(stride, PAGE_SIZE);
 layout.label_capacity = label_capacity;
 layout.label_pitch = compiled_align_up(label_capacity, PAGE_SIZE);
 layout.pixel_block_bytes = checked_multiply(COMPILED_BLOCK_SLOTS, layout.pixel_pitch, "compiled pixel block overflow");
 layout.block_bytes =
  compiled_align_up(checked_multiply(COMPILED_BLOCK_SLOTS, checked_add(layout.pixel_pitch, layout.label_pitch, "compiled slot extent overflow"), "compiled block overflow"), HUGE_PAGE_SIZE);
 if (catalog_bytes > COMPILED_CATALOG_LIMIT) throw std::runtime_error("compiled catalog exceeds byte limit");
 layout.catalog_bytes = catalog_bytes;
 layout.data_offset = compiled_align_up(checked_add(sizeof(FileHeader), catalog_bytes, "compiled metadata overflow"), HUGE_PAGE_SIZE);
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
 return checked_add(checked_add(block_offset(slot / COMPILED_BLOCK_SLOTS), prefix, "compiled label base overflow"),
  checked_multiply(size_t{slot} % COMPILED_BLOCK_SLOTS, label_pitch, "compiled label offset overflow"), "compiled label extent overflow");
}
FileHeader make_file_header(const FileHeaderInputs& inputs, std::span<const std::string> classes, const FileLayout& layout) {
 FileHeader header{};
 header.magic = MAGIC;
 header.version = FORMAT_VERSION;
 header.catalog_version = layout.catalog_bytes ? 1 : 0;
 header.catalog_bytes = checked_cast<std::uint32_t>(layout.catalog_bytes, "compiled catalog overflow");
 header.resize_mode = inputs.resize_mode;
 header.num_images = inputs.num_images;
 header.num_slots = layout.num_slots;
 header.image_width = inputs.width;
 header.image_height = inputs.height;
 header.channels = inputs.channels;
 header.num_classes = checked_cast<uint32_t>(classes.size(), "too many classes for file header");
 header.max_instances_per_image = inputs.max_instances_per_image;
 header.max_label_bytes = checked_cast<uint32_t>(layout.label_capacity, "compiled maximum label size overflow");
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
std::vector<std::byte> encode_compiled_catalog(const catalog::ClassCatalog& catalog) {
 frameworks::serialization::wire::ByteBuffer bytes;
 if (!frameworks::serialization::encode(catalog.record(), bytes, {.max_bytes = COMPILED_CATALOG_LIMIT, .max_items = COMPILED_CATALOG_LIMIT}))
  throw std::runtime_error("compiled catalog exceeds encoding limits");
 return bytes;
}
catalog::ClassCatalog decode_compiled_catalog(std::span<const std::byte> bytes) {
 auto record = frameworks::serialization::decode<catalog::OrderedClassCatalog>({bytes, {}}, {.max_bytes = COMPILED_CATALOG_LIMIT, .max_items = COMPILED_CATALOG_LIMIT});
 if (!record) throw std::runtime_error("invalid compiled catalog schema");
 return catalog::ClassCatalog(*record);
}
}  // namespace mmltk::backend::data
