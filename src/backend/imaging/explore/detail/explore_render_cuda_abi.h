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
    [[nodiscard]] bool valid() const noexcept {
        return latest_generation == nullptr || latest_generation->load(std::memory_order_acquire) == generation;
    }
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// CLEANUP-IGNORE: The card input descriptor is a fixed kernel ABI, not an acceptance-state or host-layout record.
struct ExploreRenderCardDescriptorAbi final {
    mmltk::backend::models::rfdetr::AugmentationSpatialErasure erasure{};
    // CLEANUP-IGNORE: Card source storage begins a distinct input ABI from rendered geometry probe output.
    const float* pixels = nullptr;
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

    [[nodiscard]] bool valid() const noexcept {
        return data != nullptr && width != 0U && height != 0U && pitch_bytes >= static_cast<std::size_t>(width) * 4U;
    }
};

struct ExploreRenderedCardProbeAbi final {
    // The receiver-owned clean thumbnail is the physical-copy reference.
    // Diagnostic collection never retains a raw compiled-image lane.
    ExploreRenderTargetViewAbi reference{};
    std::uint32_t content_x = 0U;
    std::uint32_t content_y = 0U;
    std::uint32_t content_width = 0U;
    // CLEANUP-IGNORE: Probe content height starts output box geometry, not a host descriptor layout.
    std::uint32_t content_height = 0U;
    std::uint32_t box_x = 0U;
    std::uint32_t box_y = 0U;
    std::uint32_t box_width = 0U;
    std::uint32_t box_height = 0U;
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
    // CLEANUP-IGNORE: The detail kernel ABI owns crop geometry; the atlas ABI owns card-grid geometry.
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
