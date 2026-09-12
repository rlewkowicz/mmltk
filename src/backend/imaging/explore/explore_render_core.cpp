module;
#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "detail/explore_render_cuda_launch.h"
#include "explore_render_storage.h"
#include "src/backend/data/compiled_format.h"

module mmltk.backend.imaging.explore.explore_render_core;

namespace mmltk::backend::imaging::explore {
namespace {

[[nodiscard]] consteval bool rle_abi_layout_matches() {
    using Public = mmltk::backend::data::RLEPair;
    using Abi = detail::ExploreRenderRlePairAbi;
    return std::is_trivially_copyable_v<Public> && std::is_trivially_copyable_v<Abi> && std::is_standard_layout_v<Public> &&
           std::is_standard_layout_v<Abi> && sizeof(Public) == sizeof(Abi) && alignof(Public) == alignof(Abi) &&
           offsetof(Public, start) == offsetof(Abi, start) && offsetof(Public, length) == offsetof(Abi, length);
}

[[nodiscard]] detail::ExploreRenderScratchViewAbi to_cuda_scratch_abi(const ExploreRenderScratchView& value) noexcept {
    return {
        .cards = value.cards,
        .annotations = value.annotations,
        .rle_pairs = value.rle_pairs,
        .classes = value.classes,
        .tiles = value.tiles,
        .capacities =
            {
                .cards = value.card_capacity,
                .annotations = value.annotation_capacity,
                .rle_pairs = value.rle_capacity,
                .classes = value.class_capacity,
                .tiles = value.tile_capacity,
            },
    };
}

static_assert(rle_abi_layout_matches());

[[nodiscard]] bool atlas_view_valid(const ExploreRenderAtlasView& view) noexcept {
    return view.card_count != 0U && view.card_extent != 0U && view.card_count <= kExploreRenderCardCapacity && view.source_width != 0U &&
           view.source_height != 0U;
}

[[nodiscard]] bool semantic_storage_valid(const ExploreRenderSemanticView& semantics, const ExploreRenderScratchView& scratch) noexcept {
    return semantics.annotation_count <= kExploreRenderAnnotationCapacity && semantics.annotation_count <= scratch.annotation_capacity &&
           semantics.rle_count <= kExploreRenderRleCapacity && semantics.rle_count <= scratch.rle_capacity &&
           semantics.class_count <= kExploreRenderClassCapacity && semantics.class_count <= scratch.class_capacity;
}

}  // namespace

ExploreStorageStatus render_explore_atlas_tiles(const ExploreRenderAtlasView& view, const ExploreRenderTileBatchView& tiles,
                                                const ExploreRenderSemanticView& semantics, const ExploreRenderScratchView& scratch,
                                                const ExploreRenderTargetView& target, const std::uintptr_t stream,
                                                const detail::ExploreRenderDemand demand) noexcept {
    if (!demand.valid()) return kExploreStorageSuccess;
    if (stream == 0U || !target.valid() || !scratch.valid() || !atlas_view_valid(view) || !valid_tile_batch(tiles, scratch) ||
        tiles.max_tile_width > view.card_extent || tiles.max_tile_height > view.card_extent || tiles.max_tile_width > target.width ||
        tiles.max_tile_height > target.height || view.card_count > scratch.card_capacity || !semantic_storage_valid(semantics, scratch)) {
        return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    }
    return static_cast<ExploreStorageStatus>(detail::render_explore_atlas_tiles_cuda(
        view, tiles, semantics, to_cuda_scratch_abi(scratch), target, reinterpret_cast<cudaStream_t>(stream), demand));
}

ExploreStorageStatus render_explore_detail(const ExploreRenderDetailView& view, const ExploreRenderSemanticView& semantics,
                                           const ExploreRenderScratchView& scratch, const ExploreRenderTargetView& target,
                                           const std::uintptr_t stream, const detail::ExploreRenderDemand demand) noexcept {
    if (!demand.valid()) return kExploreStorageSuccess;
    if (stream == 0U || !target.valid() || !scratch.valid() || view.pixels == nullptr || view.source_width == 0U ||
        view.source_height == 0U || view.crop_width == 0U || view.crop_height == 0U || view.crop_x > view.source_width - 1U ||
        view.crop_y > view.source_height - 1U || view.crop_width > view.source_width - view.crop_x ||
        view.crop_height > view.source_height - view.crop_y || !semantic_storage_valid(semantics, scratch)) {
        return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    }
    return static_cast<ExploreStorageStatus>(detail::render_explore_detail_cuda(view, semantics, to_cuda_scratch_abi(scratch), target,
                                                                                reinterpret_cast<cudaStream_t>(stream), demand));
}

ExploreStorageStatus count_explore_nonzero_alpha(const ExploreRenderTargetView& target, std::uint64_t* const device_count,
                                                 const std::uintptr_t stream) noexcept {
    if (stream == 0U || device_count == nullptr || !target.valid()) return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    return static_cast<ExploreStorageStatus>(
        detail::count_explore_nonzero_alpha_cuda(target, device_count, reinterpret_cast<cudaStream_t>(stream)));
}

ExploreStorageStatus checksum_explore_pixels(const ExploreRenderTargetView& target, std::uint64_t* const device_checksum,
                                             const std::uintptr_t stream) noexcept {
    if (stream == 0U || device_checksum == nullptr || !target.valid()) return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    return static_cast<ExploreStorageStatus>(
        detail::checksum_explore_pixels_cuda(target, device_checksum, reinterpret_cast<cudaStream_t>(stream)));
}

ExploreStorageStatus probe_explore_rendered_card(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                                 const ExploreRenderedCardProbe& probe, std::uint64_t* const device_counts,
                                                 const std::uintptr_t stream) noexcept {
    const bool content_valid = probe.reference.valid() && probe.reference.width == clean.width && probe.reference.height == clean.height &&
                               probe.content_width != 0U && probe.content_height != 0U && probe.content_x < clean.width &&
                               probe.content_y < clean.height && probe.content_width <= clean.width - probe.content_x &&
                               probe.content_height <= clean.height - probe.content_y;
    const bool box_valid = probe.box_width > 2U && probe.box_height > 2U && probe.box_x < semantic.width && probe.box_y < semantic.height &&
                           probe.box_width <= semantic.width - probe.box_x && probe.box_height <= semantic.height - probe.box_y;
    if (stream == 0U || device_counts == nullptr || !clean.valid() || !semantic.valid() || clean.width != semantic.width ||
        clean.height != semantic.height || !content_valid || !box_valid)
        return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    return static_cast<ExploreStorageStatus>(
        detail::probe_explore_rendered_card_cuda(clean, semantic, probe, device_counts, reinterpret_cast<cudaStream_t>(stream)));
}

ExploreStorageStatus sample_explore_rendered_card(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                                  const ExploreRenderTargetView& reference, std::uint64_t* const device_samples,
                                                  const std::uintptr_t stream) noexcept {
    if (stream == 0U || device_samples == nullptr || !clean.valid() || !semantic.valid() || clean.width != semantic.width ||
        clean.height != semantic.height || (reference.data != nullptr && !reference.valid()))
        return static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    return static_cast<ExploreStorageStatus>(
        detail::sample_explore_rendered_card_cuda(clean, semantic, reference, device_samples, reinterpret_cast<cudaStream_t>(stream)));
}

}  // namespace mmltk::backend::imaging::explore
