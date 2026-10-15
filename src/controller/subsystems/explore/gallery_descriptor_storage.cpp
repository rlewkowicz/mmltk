#include "src/controller/subsystems/explore/detail/gallery_descriptor_storage.h"
#include "src/backend/data/compiled/compiled_format.h"
#include <algorithm>
#include <stdexcept>
import mmltk.backend.imaging.explore.explore_render_core;
namespace mmltk::controller::explore_detail {
namespace explore = mmltk::backend::imaging::explore;
namespace data = mmltk::backend::data;
GalleryDescriptorStorage::GalleryDescriptorStorage() { storage_.Bind(host_allocations_.api()); }
GalleryDescriptorStorage::~GalleryDescriptorStorage() = default;
void GalleryDescriptorStorage::PrepareDescriptors(const std::size_t cards, const std::size_t annotations, const std::size_t rle, const std::size_t classes, const std::size_t tiles,
 VisualDiagnosticSink diagnostics, int device, std::uint64_t generation, const std::size_t joints, const data::catalog::ClassCatalog& catalog) {
 if (descriptors_pending_) throw std::logic_error("Explore descriptor staging was not settled before output publication");
 DescriptorLayout layout;
 layout.cards.count = cards;
 layout.annotations.count = annotations;
 layout.rle.count = rle;
 layout.joints.count = joints;
 if (joints > explore::kExploreRenderJointCapacity || classes != catalog.size()) throw std::length_error("Explore joint or catalog capacity exceeded");
 for (std::size_t category = 0; category < classes; ++category) {
  const auto count = catalog.keypoints(category).skeleton.size();
  class_edges_[category] = {layout.edges.count, count};
  layout.edges.count += count;
 }
 layout.classes.count = classes;
 layout.tiles.count = tiles;
 layout.annotations.offset = align_up(cards * sizeof(explore::ExploreRenderCardDescriptor), alignof(explore::ExploreRenderAnnotationDescriptor));
 layout.rle.offset = align_up(layout.annotations.offset + annotations * sizeof(explore::ExploreRenderAnnotationDescriptor), alignof(data::RLEPair));
 layout.joints.offset = align_up(layout.rle.offset + rle * sizeof(data::RLEPair), alignof(data::PackedKeypoint));
 layout.edges.offset = align_up(layout.joints.offset + joints * sizeof(data::PackedKeypoint), alignof(explore::detail::ExploreRenderEdgeAbi));
 layout.classes.offset = align_up(layout.edges.offset + layout.edges.count * sizeof(explore::detail::ExploreRenderEdgeAbi), alignof(explore::ExploreRenderClassDescriptor));
 layout.tiles.offset = align_up(layout.classes.offset + classes * sizeof(explore::ExploreRenderClassDescriptor), alignof(explore::ExploreRenderTileDescriptor));
 layout.bytes = layout.tiles.offset + tiles * sizeof(explore::ExploreRenderTileDescriptor);
 ensure_gallery_buffer(storage_.buffers_.descriptors_, std::max<std::size_t>(layout.bytes, 1U), "Explore pinned descriptor staging allocation failed", diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.cards_device_, std::max<std::size_t>(cards * sizeof(explore::ExploreRenderCardDescriptor), 1U), "Explore card descriptor high-water allocation failed",
  diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.annotations_device_, std::max<std::size_t>(annotations * sizeof(explore::ExploreRenderAnnotationDescriptor), 1U),
  "Explore annotation descriptor high-water allocation failed", diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.rle_device_, std::max<std::size_t>(rle * sizeof(data::RLEPair), 1U), "Explore RLE descriptor high-water allocation failed", diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.classes_device_, std::max<std::size_t>(classes * sizeof(explore::ExploreRenderClassDescriptor), 1U), "Explore class descriptor high-water allocation failed",
  diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.tiles_device_, std::max<std::size_t>(tiles * sizeof(explore::ExploreRenderTileDescriptor), 1U), "Explore tile descriptor high-water allocation failed",
  diagnostics, device, generation);
 ensure_gallery_buffer(
  storage_.buffers_.joints_device_, std::max<std::size_t>(joints * sizeof(data::PackedKeypoint), 1U), "Explore joint descriptor allocation failed", diagnostics, device, generation);
 ensure_gallery_buffer(storage_.buffers_.edges_device_, std::max<std::size_t>(layout.edges.count * sizeof(explore::detail::ExploreRenderEdgeAbi), 1U), "Explore skeleton edge allocation failed",
  diagnostics, device, generation);
 for (std::size_t category = 0; category < classes; ++category) {
  auto offset = class_edges_[category].offset;
  for (const auto& edge : catalog.keypoints(category).skeleton)
   store_payload(storage_.buffers_.descriptors_.data(), layout.edges.offset + offset++ * sizeof(explore::detail::ExploreRenderEdgeAbi), explore::detail::ExploreRenderEdgeAbi{edge.first, edge.second});
 }
 descriptor_layout_ = layout;
}
void GalleryDescriptorStorage::UploadDescriptors(const cudaStream_t stream, const bool upload_classes, const ExploreDemandCheck& demand, std::uint64_t generation) {
 UploadDescriptor(storage_.buffers_.cards_device_, descriptor_layout_.cards.offset, descriptor_layout_.cards.count * sizeof(explore::ExploreRenderCardDescriptor), stream, demand, generation);
 UploadDescriptor(
  storage_.buffers_.annotations_device_, descriptor_layout_.annotations.offset, descriptor_layout_.annotations.count * sizeof(explore::ExploreRenderAnnotationDescriptor), stream, demand, generation);
 UploadDescriptor(storage_.buffers_.rle_device_, descriptor_layout_.rle.offset, descriptor_layout_.rle.count * sizeof(data::RLEPair), stream, demand, generation);
 UploadDescriptor(storage_.buffers_.joints_device_, descriptor_layout_.joints.offset, descriptor_layout_.joints.count * sizeof(data::PackedKeypoint), stream, demand, generation);
 UploadDescriptor(storage_.buffers_.edges_device_, descriptor_layout_.edges.offset, descriptor_layout_.edges.count * sizeof(explore::detail::ExploreRenderEdgeAbi), stream, demand, generation);
 if (upload_classes) {
  for (std::size_t category = 0; category < descriptor_layout_.classes.count; ++category) {
   const auto offset = descriptor_layout_.classes.offset + category * sizeof(explore::ExploreRenderClassDescriptor);
   auto value = load_payload<explore::ExploreRenderClassDescriptor>(storage_.buffers_.descriptors_.data(), offset);
   value.edge_offset = static_cast<std::uint32_t>(class_edges_[category].offset);
   value.edge_count = static_cast<std::uint32_t>(class_edges_[category].count);
   store_payload(storage_.buffers_.descriptors_.data(), offset, value);
  }
  UploadDescriptor(storage_.buffers_.classes_device_, descriptor_layout_.classes.offset, descriptor_layout_.classes.count * sizeof(explore::ExploreRenderClassDescriptor), stream, demand, generation);
 }
 UploadDescriptor(storage_.buffers_.tiles_device_, descriptor_layout_.tiles.offset, descriptor_layout_.tiles.count * sizeof(explore::ExploreRenderTileDescriptor), stream, demand, generation);
}
void GalleryDescriptorStorage::UploadDescriptor(
 explore::ExploreHighWaterBuffer& destination, const std::size_t offset, const std::size_t bytes, const cudaStream_t stream, const ExploreDemandCheck& demand, std::uint64_t generation) {
 if (!demand(generation)) return;
 if (bytes != 0U)
  ensure_gallery_cuda(
   cudaMemcpyAsync(destination.data(), static_cast<std::byte*>(storage_.buffers_.descriptors_.data()) + offset, bytes, cudaMemcpyHostToDevice, stream), "Explore pinned descriptor upload failed");
}
}  // namespace mmltk::controller::explore_detail
