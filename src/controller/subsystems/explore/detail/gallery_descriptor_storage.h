#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <cuda_runtime_api.h>
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/controller/subsystems/explore/detail/gallery_buffer_family.h"
#include "src/controller/subsystems/explore/detail/gallery_host_allocations.h"
#include "src/controller/subsystems/explore/detail/gallery_payload.h"
#include "src/controller/subsystems/explore/explore_system.h"
namespace mmltk::controller::explore_detail {
class GalleryStream;
class GalleryStreamProbe;
class GalleryDescriptorStorage final {
 friend class GalleryStream;
 friend struct NativeExploreStorageTestAccess;
 friend class GalleryStreamProbe;

public:
 GalleryDescriptorStorage();
 ~GalleryDescriptorStorage();
 GalleryDescriptorStorage(const GalleryDescriptorStorage&) = delete;
 GalleryDescriptorStorage& operator=(const GalleryDescriptorStorage&) = delete;

private:
 using Buffer = mmltk::backend::imaging::explore::ExploreHighWaterBuffer;
 using Memory = mmltk::backend::imaging::explore::ExploreBufferMemory;
 struct Family final {
  Buffer cards_device_;
  Buffer annotations_device_;
  Buffer rle_device_;
  Buffer classes_device_;
  Buffer tiles_device_;
  Buffer descriptors_{Memory::PinnedHost};
  Buffer augmented_batch_;
  Buffer donor_boxes_device_;
  Buffer donor_masks_device_;
 };
 ExploreHostAllocations host_allocations_;
 GalleryBufferFamily<Family> storage_;
 struct DescriptorLayout final {
  StorageSpan cards{};
  StorageSpan annotations{};
  StorageSpan rle{};
  StorageSpan classes{};
  StorageSpan tiles{};
  std::size_t bytes = 0U;
 };
 std::vector<const float*> batch_input_slots_, batch_donor_slots_;
 std::vector<std::uint32_t> batch_indices_;
 std::vector<std::uint32_t> semantic_slots_;
 std::vector<std::uint64_t> batch_keys_;
 std::vector<mmltk::backend::models::rfdetr::GpuAugmentationDonor> batch_donors_;
 std::unique_ptr<mmltk::backend::models::rfdetr::GpuAugmentationExecutor> augmenter_;
 std::uint32_t augmentation_width_ = 0U;
 std::uint32_t augmentation_height_ = 0U;
 DescriptorLayout descriptor_layout_{};
 bool descriptors_pending_ = false;
 void PrepareDescriptors(std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, VisualDiagnosticSink, int, std::uint64_t);
 void UploadDescriptors(cudaStream_t, bool, const ExploreDemandCheck&, std::uint64_t);
 void UploadDescriptor(Buffer&, std::size_t, std::size_t, cudaStream_t, const ExploreDemandCheck&, std::uint64_t);
};
}  // namespace mmltk::controller::explore_detail
