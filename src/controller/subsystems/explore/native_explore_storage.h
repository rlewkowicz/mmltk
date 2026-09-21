#pragma once
#include <array>
#include <cstddef>
#include "src/backend/imaging/explore/explore_render_storage.h"
#include "src/controller/subsystems/explore/detail/gallery_buffer_family.h"
namespace mmltk::controller::explore_detail {
class GalleryStream;
struct NativeExploreStorageTestAccess;
// Physical retained cache images. Descriptor and probe storage have independent
// typed owners in the same terminal-retirement aggregate.
class NativeExploreStorage final {
 friend class GalleryStream;
 friend struct NativeExploreStorageTestAccess;
 using Buffer = mmltk::backend::imaging::explore::ExploreHighWaterBuffer;
 struct Family final {
  std::array<Buffer, 2U> cached_clean_;
  std::array<Buffer, 2U> cached_semantic_;
 };
 GalleryBufferFamily<Family> storage_;

public:
 using Release = GalleryBufferFamily<Family>::Release;
 void Bind(mmltk::backend::imaging::explore::ExploreCudaAllocationApi) noexcept;
 [[nodiscard]] Release ResetChecked() noexcept;
 [[nodiscard]] bool OwnsAllocation() const noexcept;
};
}  // namespace mmltk::controller::explore_detail
