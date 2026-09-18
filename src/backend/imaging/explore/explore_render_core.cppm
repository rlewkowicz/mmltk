module;
#include <algorithm>
#include "src/backend/imaging/raster/image_containment.h"
#include <cstddef>
#include <cstdint>
#include "detail/explore_render_cuda_abi.h"
#include "explore_render_storage.h"
#include "src/backend/data/compiled_format.h"
export module mmltk.backend.imaging.explore.explore_render_core;
import mmltk.backend.imaging.explore.compiled_explore_store;
export namespace mmltk::backend::imaging::explore {
// The core is deliberately a value-only CUDA boundary. All descriptors are
// bounded, device-resident views supplied by the physical owner; the core
// patches a persistent atlas or selected target but does not open files,
// allocate storage, own streams, or retain completion state.
inline constexpr std::size_t kExploreRenderClassCapacity = mmltk::backend::data::MAX_CLASSES;
inline constexpr std::size_t kExploreRenderCardCapacity = 256U;
inline constexpr std::size_t kExploreRenderTileCapacity = kExploreWorkCapacity;
inline constexpr std::size_t kExploreRenderAnnotationCapacity = 4096U;
inline constexpr std::size_t kExploreRenderRleCapacity = 1U << 22U;
using ExploreRenderCardDescriptor = detail::ExploreRenderCardDescriptorAbi;
using ExploreRenderAnnotationDescriptor = detail::ExploreRenderAnnotationDescriptorAbi;
using ExploreRenderClassDescriptor = detail::ExploreRenderClassDescriptorAbi;
using ExploreContainRect = mmltk::backend::imaging::raster::ImageContainRect;
[[nodiscard]] inline ExploreContainRect make_explore_contain_rect(std::uint32_t width, std::uint32_t height, std::uint32_t extent) noexcept {
    return mmltk::backend::imaging::raster::contain_image(width, height, extent, extent);
}
// A tile is an independently patchable region of a persistent atlas.  Its
// generation fields are copied from the owning lane request and remain part
// of the device descriptor until the completion callback releases that lane.
using ExploreRenderTileDescriptor = detail::ExploreRenderTileDescriptorAbi;
using ExploreRenderTargetView = detail::ExploreRenderTargetViewAbi;
struct ExploreRenderScratchView final {
    const ExploreRenderCardDescriptor* cards = nullptr;
    std::uint32_t card_capacity = 0U;
    const ExploreRenderAnnotationDescriptor* annotations = nullptr;
    std::uint32_t annotation_capacity = 0U;
    const mmltk::backend::data::RLEPair* rle_pairs = nullptr;
    std::uint32_t rle_capacity = 0U;
    const ExploreRenderClassDescriptor* classes = nullptr;
    std::uint32_t class_capacity = 0U;
    const ExploreRenderTileDescriptor* tiles = nullptr;
    std::uint32_t tile_capacity = 0U;
    [[nodiscard]] bool valid() const noexcept {
        return cards != nullptr && annotations != nullptr && (rle_pairs != nullptr || rle_capacity == 0U) && (classes != nullptr || class_capacity == 0U);
    }
};
using ExploreRenderTileBatchView = detail::ExploreRenderTileBatchViewAbi;
[[nodiscard]] inline bool valid_tile_batch(const ExploreRenderTileBatchView& tiles, const ExploreRenderScratchView& scratch) noexcept {
    return tiles.tile_count != 0U && tiles.tile_count <= kExploreRenderTileCapacity && tiles.tile_count <= tiles.tile_capacity &&
           tiles.tile_count <= scratch.tile_capacity && scratch.tiles != nullptr && tiles.max_tile_width != 0U && tiles.max_tile_height != 0U &&
           tiles.viewport_generation != 0U;
}
// Semantic inputs remain separate from the clean image target.  The owner
// may submit the same descriptors once for a clean target and once for an
// annotation plane without creating another image representation.
using ExploreRenderSemanticView = detail::ExploreRenderSemanticViewAbi;
// Atlas card indices address the compact descriptor batch. Tile destination
// coordinates independently select the retained-atlas region to patch.
using ExploreRenderAtlasView = detail::ExploreRenderAtlasViewAbi;
using ExploreRenderDetailView = detail::ExploreRenderDetailViewAbi;
using ExploreRenderedCardProbe = detail::ExploreRenderedCardProbeAbi;
using ExploreRenderedCardSampleGrid = detail::ExploreRenderedCardSampleGridAbi;
// Patches one or more disjoint regions of an already allocated atlas. A tile
// with placeholder set writes the deterministic placeholder without reading a
// card; every tile is filtered by the exact viewport generation carried by its
// lane descriptor.
[[nodiscard]] ExploreStorageStatus render_explore_atlas_tiles(const ExploreRenderAtlasView& view, const ExploreRenderTileBatchView& tiles,
                                                              const ExploreRenderSemanticView& semantics, const ExploreRenderScratchView& scratch,
                                                              const ExploreRenderTargetView& target, std::uintptr_t stream,
                                                              detail::ExploreRenderDemand demand = {}) noexcept;
[[nodiscard]] ExploreStorageStatus render_explore_detail(const ExploreRenderDetailView& view, const ExploreRenderSemanticView& semantics,
                                                         const ExploreRenderScratchView& scratch, const ExploreRenderTargetView& target, std::uintptr_t stream,
                                                         detail::ExploreRenderDemand demand = {}) noexcept;
// Adds the target's nonzero-alpha pixel count to a caller-owned device counter.
// The caller controls clearing and transfer so diagnostics can reuse storage.
[[nodiscard]] ExploreStorageStatus count_explore_nonzero_alpha(const ExploreRenderTargetView& target, std::uint64_t* device_count,
                                                               std::uintptr_t stream) noexcept;
// Adds a deterministic position-sensitive checksum of every RGBA pixel to a
// caller-owned device value. This is diagnostic evidence, not image identity.
[[nodiscard]] ExploreStorageStatus checksum_explore_pixels(const ExploreRenderTargetView& target, std::uint64_t* device_checksum,
                                                           std::uintptr_t stream) noexcept;
// Counts rendered content, exact immediately-outside padding samples, semantic
// box-edge pixels, semantic mask-interior pixels, and exact expected immediately-
// inside content samples against the retained RGBA copy into five caller-owned
// device values. The reference contains no borrowed compiled-image storage.
[[nodiscard]] ExploreStorageStatus probe_explore_rendered_card(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                                               const ExploreRenderedCardProbe& probe, std::uint64_t* device_counts,
                                                               std::uintptr_t stream) noexcept;
// Samples a fixed compact grid of clean, semantic, and optional retained clean
// pixels at floor(percent * extent / 100) independently in each target.
// The caller supplies ExploreRenderedCardSampleGrid::kSampleCount *
// ExploreRenderedCardSampleGrid::kWordsPerSample device values
// and owns all storage, submission, and transfer as for the other probes.
[[nodiscard]] ExploreStorageStatus sample_explore_rendered_card(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                                                const ExploreRenderTargetView& reference, std::uint64_t* device_samples,
                                                                std::uintptr_t stream) noexcept;
}  // namespace mmltk::backend::imaging::explore
