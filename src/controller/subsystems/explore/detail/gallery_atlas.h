#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/frameworks/gpu/image_types.h"
namespace mmltk::controller::explore_detail {
// Metadata only. ImageProductPool owns admission, storage, and read custody.
// Each directory describes its own allocation, never the selected baseline.
class GalleryAtlas final {
public:
 [[nodiscard]] ExploreAtlasLayout Begin(mmltk::frameworks::gpu::ImagePlaneView clean, mmltk::frameworks::gpu::ImagePlaneView semantic, const ExploreViewport&, const GalleryThumbnailCache::Identity&);
 [[nodiscard]] std::size_t Physical(std::size_t logical) const noexcept;
 [[nodiscard]] bool Contains(std::size_t physical, const std::shared_ptr<const GalleryTileMeaning>&, std::uint64_t semantic_identity) const;
 [[nodiscard]] bool ContainsClean(std::size_t physical, const std::shared_ptr<const GalleryTileMeaning>&) const;
 [[nodiscard]] bool Empty(std::size_t physical, bool placeholder) const;
 // Invalidate before the first GPU write, including a write that may throw.
 void Touch(std::size_t physical);
 // Stage only completely submitted pixels. Commit follows GPU settlement.
 void Stage(std::size_t physical, std::shared_ptr<const GalleryTileMeaning> = {}, std::uint64_t semantic_identity = 0U, bool placeholder = false);
 void Commit() noexcept;
 void Rollback() noexcept;
 void Invalidate(mmltk::frameworks::gpu::ImageAllocation) noexcept;
 void Clear() noexcept;
 [[nodiscard]] const ExploreAtlasLayout& layout() const noexcept { return layout_; }
 [[nodiscard]] std::size_t MetadataBytes() const noexcept;
 [[nodiscard]] std::size_t AppendMeanings(std::span<std::shared_ptr<const GalleryTileMeaning>>) const;
 [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceCoverage WorkspaceCoverage(const mmltk::frameworks::gpu::ImageWorkspaceObservation&);

private:
 struct Cell final {
  std::shared_ptr<const GalleryTileMeaning> meaning;
  std::uint64_t semantics = 0U;
  bool valid = false;
  bool placeholder = false;
 };
 struct Allocation final {
  mmltk::frameworks::gpu::ImageAllocation clean{}, semantic{};
  GalleryThumbnailCache::Identity pixels{};
  std::uint32_t columns = 0U;
  std::uint32_t rows = 0U;
  std::uint64_t layout_generation = 0U;
  std::uint64_t revision = 0U;
  std::vector<Cell> cells;
 };
 struct Write final {
  std::size_t physical;
  Cell cell;
 };
 std::array<Allocation, 3U> allocations_{};
 Allocation* active_ = nullptr;
 std::uint64_t pending_revision_ = 0U;
 ExploreAtlasLayout layout_{};
 std::vector<Write> writes_;
 std::vector<mmltk::frameworks::gpu::ImageWorkspaceRegion> coverage_;
};
}  // namespace mmltk::controller::explore_detail
