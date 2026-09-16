#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "src/backend/models/rfdetr/augmentation/gpu_augment_types.h"
namespace mmltk::backend::imaging::explore::detail {
// Non-owning host submission frontier. Never copied into a CUDA kernel;
// already-submitted work keeps its ordinary completion/lifetime contract.
struct ExploreRenderDemand final {
    const std::atomic<std::uint64_t>* latest_generation = nullptr;
    std::uint64_t generation = 0U;
    [[nodiscard]] bool valid() const noexcept { return latest_generation == nullptr || latest_generation->load(std::memory_order_acquire) == generation; }
};
// CLEANUP-IGNORE: The lock-free demand assertion precedes an independent fixed kernel input ABI.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct ExploreRenderCardDescriptorAbi final {
    // CLEANUP-IGNORE: Card erasure and source fields form a fixed input ABI unrelated to the diagnostic envelope.
    mmltk::backend::models::rfdetr::AugmentationSpatialErasure erasure{};
    // CLEANUP-IGNORE: The card pixel pointer and following dimensions are a fixed CUDA input layout, not trace storage.
    const float* pixels = nullptr;
    // CLEANUP-IGNORE: This fixed card-input kernel ABI preserves its independently versioned source geometry.
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t image_x = 0U;
    std::uint32_t image_y = 0U;
    std::uint32_t image_width = 0U;
    std::uint32_t image_height = 0U;
    std::uint32_t annotation_offset = 0U;
    std::uint32_t annotation_count = 0U;
    std::uint32_t compiled_index = 0U;
    std::uint8_t selected = 0U;
    std::uint8_t placeholder = 0U;
    std::uint8_t reserved[2U]{};
};
struct ExploreRenderAnnotationDescriptorAbi final {
    float box_xyxy[4U]{};
    float inverse[6U]{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    std::uint32_t rle_offset = 0U;
    std::uint32_t rle_count = 0U;
    std::uint32_t card_index = 0U;
    std::uint16_t class_id = 0U;
    std::int32_t occluder_index = -1;
};
struct ExploreRenderClassDescriptorAbi final {
    std::uint8_t color[3U]{255U, 0U, 0U};
    char name[32U]{};
    std::uint8_t length = 0U;
    std::uint8_t visible = 1U;
    std::uint8_t reserved[2U]{};
    // CLEANUP-IGNORE: This explicit Explore CUDA ABI record preserves its kernel-specific field order and types.
};
struct ExploreRenderTileGenerationAbi final {
    std::uint64_t viewport = 0U;
    std::uint64_t tile = 0U;
    [[nodiscard]] bool operator==(const ExploreRenderTileGenerationAbi&) const noexcept = default;
};
struct ExploreRenderTileDescriptorAbi final {
    std::uint32_t card_index = 0U;
    std::uint32_t destination_x = 0U;
    std::uint32_t destination_y = 0U;
    std::int32_t semantic_y_offset = 0;
    std::uint32_t destination_width = 0U;
    std::uint32_t destination_height = 0U;
    ExploreRenderTileGenerationAbi generation{};
    std::uint8_t placeholder = 0U;
    std::uint8_t reserved[7U]{};
    [[nodiscard]] bool operator==(const ExploreRenderTileDescriptorAbi&) const noexcept = default;
};
struct ExploreRenderTargetViewAbi final {
    std::uint8_t* data = nullptr;
    std::size_t pitch_bytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] bool valid() const noexcept { return data != nullptr && width != 0U && height != 0U && pitch_bytes >= static_cast<std::size_t>(width) * 4U; }
};
struct ExploreRenderedCardProbeAbi final {
    // The receiver-owned clean thumbnail is the physical-copy reference.
    // Diagnostic collection never retains a raw compiled-image lane.
    // CLEANUP-IGNORE: The receiver-owned reference begins a fixed probe ABI, not a host diagnostic record.
    ExploreRenderTargetViewAbi reference{};
    // CLEANUP-IGNORE: Probe content geometry is not interchangeable with source or crop geometry.
    std::uint32_t content_x = 0U;
    std::uint32_t content_y = 0U;
    std::uint32_t content_width = 0U;
    std::uint32_t content_height = 0U;
    // CLEANUP-IGNORE: Probe box geometry is a separate semantic region within the fixed output ABI.
    std::uint32_t box_x = 0U;
    std::uint32_t box_y = 0U;
    std::uint32_t box_width = 0U;
    std::uint32_t box_height = 0U;
};
// A bounded row-major grid shared by collection and diagnostic formatting.
// Each point produces two uint64 values: clean/semantic RGBA in low/high
// halves of the first, and retained clean RGBA in the low half of the second.
struct ExploreRenderedCardSampleGridAbi final {
    static constexpr std::size_t kAxisCount = 5U;
    static constexpr std::size_t kSampleCount = kAxisCount * kAxisCount;
    static constexpr std::size_t kWordsPerSample = 2U;
    std::uint32_t percent[kAxisCount]{5U, 35U, 50U, 65U, 95U};
};
struct ExploreRenderScratchCapacitiesAbi final {
    std::uint32_t cards = 0U;
    std::uint32_t annotations = 0U;
    std::uint32_t rle_pairs = 0U;
    std::uint32_t classes = 0U;
    std::uint32_t tiles = 0U;
};
struct ExploreRenderScratchViewAbi final {
    const ExploreRenderCardDescriptorAbi* cards = nullptr;
    const ExploreRenderAnnotationDescriptorAbi* annotations = nullptr;
    const void* rle_pairs = nullptr;
    const ExploreRenderClassDescriptorAbi* classes = nullptr;
    const ExploreRenderTileDescriptorAbi* tiles = nullptr;
    ExploreRenderScratchCapacitiesAbi capacities{};
};
struct ExploreRenderTileBatchViewAbi final {
    // CLEANUP-IGNORE: Tile-batch capacities are fixed kernel ABI fields, not host payload-layout bookkeeping.
    std::uint32_t tile_count = 0U;
    std::uint32_t tile_capacity = 0U;
    std::uint32_t max_tile_width = 0U;
    std::uint32_t max_tile_height = 0U;
    std::uint64_t viewport_generation = 0U;
};
struct ExploreRenderSemanticViewAbi final {
    std::uint32_t annotation_count = 0U;
    std::uint32_t rle_count = 0U;
    std::uint32_t class_count = 0U;
    std::uint8_t show_boxes = 1U;
    std::uint8_t show_masks = 1U;
    std::uint8_t reserved[2U]{};
};
struct ExploreRenderAtlasViewAbi final {
    std::uint32_t card_extent = 0U;
    std::uint32_t card_count = 0U;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint8_t draw_base = 1U;
    std::uint8_t reserved[3U]{};
};
struct ExploreRenderDetailViewAbi final {
    mmltk::backend::models::rfdetr::AugmentationSpatialErasure erasure{};
    const float* pixels = nullptr;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t crop_x = 0U;
    std::uint32_t crop_y = 0U;
    std::uint32_t crop_width = 0U;
    std::uint32_t crop_height = 0U;
    std::uint8_t draw_base = 1U;
    std::uint8_t reserved[3U]{};
};
struct __attribute__((packed)) ExploreRenderRlePairAbi final {
    std::uint32_t start = 0U;
    std::uint32_t length = 0U;
};
}  // namespace mmltk::backend::imaging::explore::detail
