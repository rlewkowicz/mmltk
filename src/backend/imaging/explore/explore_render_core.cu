#include "src/backend/imaging/raster/image_containment.h"
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include "detail/explore_render_cuda_launch.h"
#include "mask_sample.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"
namespace mmltk::backend::imaging::explore::detail {
namespace {
using ExploreRenderAnnotationDescriptor = ExploreRenderAnnotationDescriptorAbi;
using ExploreRenderAtlasView = ExploreRenderAtlasViewAbi;
using ExploreRenderCardDescriptor = ExploreRenderCardDescriptorAbi;
using ExploreRenderClassDescriptor = ExploreRenderClassDescriptorAbi;
using ExploreRenderDetailView = ExploreRenderDetailViewAbi;
using ExploreRenderRlePair = ExploreRenderRlePairAbi;
using ExploreRenderScratchView = ExploreRenderScratchViewAbi;
using ExploreRenderSemanticView = ExploreRenderSemanticViewAbi;
using ExploreRenderTargetView = ExploreRenderTargetViewAbi;
using ExploreRenderTileBatchView = ExploreRenderTileBatchViewAbi;
using ExploreRenderTileDescriptor = ExploreRenderTileDescriptorAbi;
struct Target final {
    std::uint8_t* data = nullptr;
    std::size_t pitch = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};
[[nodiscard]] __device__ __forceinline__ Target target_view(const ExploreRenderTargetView view) noexcept {
    return {view.data, view.pitch_bytes, view.width, view.height};
}
[[nodiscard]] __device__ __forceinline__ uchar4 class_colour(const ExploreRenderClassDescriptor* classes, const std::uint16_t class_id) noexcept {
    const auto& color = classes[class_id].color;
    return make_uchar4(color[0], color[1], color[2], 255U);
}
[[nodiscard]] __device__ __forceinline__ bool class_visible(const ExploreRenderClassDescriptor* classes, const std::uint32_t class_count,
                                                            const std::uint16_t class_id) noexcept {
    return classes != nullptr && class_id < class_count && classes[class_id].visible != 0U;
}
[[nodiscard]] __device__ __forceinline__ unsigned char to_byte(const float value) noexcept {
    return static_cast<unsigned char>(__float2int_rn(fminf(1.0F, fmaxf(0.0F, value)) * 255.0F));
}
[[nodiscard]] __device__ __forceinline__ float read_nchw(const float* pixels, const std::uint32_t width, const std::uint32_t height,
                                                         const std::uint32_t channel, const int x, const int y) noexcept {
    const int clamped_x = max(0, min(static_cast<int>(width) - 1, x));
    const int clamped_y = max(0, min(static_cast<int>(height) - 1, y));
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    return pixels[static_cast<std::size_t>(channel) * plane + static_cast<std::size_t>(clamped_y) * width + static_cast<std::size_t>(clamped_x)];
}
[[nodiscard]] __device__ __forceinline__ float sample_nchw(const float* pixels, const std::uint32_t width, const std::uint32_t height,
                                                           const std::uint32_t channel, const float x, const float y) noexcept {
    const int x0 = static_cast<int>(floorf(x));
    const int y0 = static_cast<int>(floorf(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float a = read_nchw(pixels, width, height, channel, x0, y0);
    const float b = read_nchw(pixels, width, height, channel, x0 + 1, y0);
    const float c = read_nchw(pixels, width, height, channel, x0, y0 + 1);
    const float d = read_nchw(pixels, width, height, channel, x0 + 1, y0 + 1);
    const float top = fmaf(fx, b - a, a);
    const float bottom = fmaf(fx, d - c, c);
    return fmaf(fy, bottom - top, top);
}
__device__ __forceinline__ void store(const Target target, const int x, const int y, const uchar4 value) noexcept {
    auto* row = reinterpret_cast<uchar4*>(target.data + static_cast<std::size_t>(y) * target.pitch);
    row[x] = value;
}
[[nodiscard]] __device__ __forceinline__ bool rle_contains(const ExploreRenderRlePair* pairs, const ExploreRenderAnnotationDescriptor& annotation,
                                                           const std::uint32_t rle_capacity, const std::uint32_t width, const std::uint32_t height,
                                                           const float x, const float y) noexcept {
    return sample_annotation_mask(pairs, annotation, rle_capacity, width, height, x, y);
}
__device__ __forceinline__ void blend_mask(float& red, float& green, float& blue, const uchar4 colour) noexcept {
    constexpr float alpha = 0.36F;
    constexpr float scale = 1.0F / 255.0F;
    red = fmaf(alpha, static_cast<float>(colour.x) * scale - red, red);
    green = fmaf(alpha, static_cast<float>(colour.y) * scale - green, green);
    blue = fmaf(alpha, static_cast<float>(colour.z) * scale - blue, blue);
}
[[nodiscard]] __device__ __forceinline__ bool apply_masks(float& red, float& green, float& blue, float& alpha,
                                                          const ExploreRenderAnnotationDescriptor* annotations, const ExploreRenderRlePair* pairs,
                                                          const ExploreRenderClassDescriptor* classes, const std::uint32_t annotation_offset,
                                                          const std::uint32_t annotation_count, const std::uint32_t annotation_capacity,
                                                          const std::uint32_t rle_capacity, const std::uint32_t class_count, const std::uint32_t width,
                                                          const std::uint32_t height, const float x, const float y) noexcept {
    if (annotations == nullptr || annotation_offset > annotation_capacity || annotation_count > annotation_capacity - annotation_offset) { return false; }
    bool applied = false;
    for (std::uint32_t local = 0U; local < annotation_count; ++local) {
        const auto& annotation = annotations[annotation_offset + local];
        if (!class_visible(classes, class_count, annotation.class_id) || !rle_contains(pairs, annotation, rle_capacity, width, height, x, y)) { continue; }
        if (annotation.occluder_index >= 0 && static_cast<std::uint32_t>(annotation.occluder_index) < annotation_capacity) {
            const auto& occluder = annotations[annotation.occluder_index];
            if (sample_annotation_support(pairs, occluder, rle_capacity, width, height, x, y)) continue;
        }
        blend_mask(red, green, blue, class_colour(classes, annotation.class_id));
        alpha = 0.36F + alpha * 0.64F;
        applied = true;
    }
    return applied;
}
__global__ void atlas_tile_base_kernel(const ExploreRenderTargetView target_view_value, const ExploreRenderCardDescriptor* cards,
                                       const ExploreRenderAnnotationDescriptor* annotations, const ExploreRenderRlePair* pairs,
                                       const ExploreRenderClassDescriptor* classes, const ExploreRenderTileDescriptor* tiles, const ExploreRenderAtlasView view,
                                       const ExploreRenderTileBatchView batch, const ExploreRenderSemanticView semantics,
                                       const std::uint32_t annotation_capacity) {
    const Target target = target_view(target_view_value);
    const std::uint32_t tile_index = blockIdx.z;
    if (tile_index >= batch.tile_count) return;
    auto tile = tiles[tile_index];
    if (view.draw_base == 0U) tile.destination_y = static_cast<std::uint32_t>(static_cast<std::int64_t>(tile.destination_y) + tile.semantic_y_offset);
    if (tile.generation.viewport != batch.viewport_generation || tile.destination_width == 0U || tile.destination_height == 0U ||
        tile.destination_x >= target.width || tile.destination_y >= target.height || tile.destination_width > target.width - tile.destination_x ||
        tile.destination_height > target.height - tile.destination_y) {
        return;
    }
    const std::uint32_t local_x = blockIdx.x * blockDim.x + threadIdx.x;
    const std::uint32_t local_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (local_x >= tile.destination_width || local_y >= tile.destination_height) return;
    const std::uint32_t x = tile.destination_x + local_x;
    const std::uint32_t y = tile.destination_y + local_y;
    if (x >= target.width || y >= target.height) return;
    uchar4 output = view.draw_base != 0U ? make_uchar4(mmltk::backend::imaging::raster::kAtlasPadding.r, mmltk::backend::imaging::raster::kAtlasPadding.g, mmltk::backend::imaging::raster::kAtlasPadding.b, mmltk::backend::imaging::raster::kAtlasPadding.a) : make_uchar4(0U, 0U, 0U, 0U);
    if (tile.placeholder != 0U || tile.card_index >= view.card_count || cards[tile.card_index].placeholder != 0U) {
        store(target, static_cast<int>(x), static_cast<int>(y), output);
        return;
    }
    const auto card = cards[tile.card_index];
    if (card.source_width == 0U || card.source_height == 0U) {
        store(target, static_cast<int>(x), static_cast<int>(y), output);
        return;
    }
    const std::uint32_t image_x = tile.destination_x + card.image_x;
    const std::uint32_t image_y = tile.destination_y + card.image_y;
    if (x < image_x || y < image_y || x >= image_x + card.image_width || y >= image_y + card.image_height) {
        store(target, static_cast<int>(x), static_cast<int>(y), output);
        return;
    }
    const float normalized_x = (static_cast<float>(x - image_x) + 0.5F) / static_cast<float>(card.image_width);
    const float normalized_y = (static_cast<float>(y - image_y) + 0.5F) / static_cast<float>(card.image_height);
    if (view.draw_base != 0U && card.pixels != nullptr) {
        const float sample_x = normalized_x * static_cast<float>(card.source_width) - 0.5F;
        const float sample_y = normalized_y * static_cast<float>(card.source_height) - 0.5F;
        output = make_uchar4(to_byte(sample_nchw(card.pixels, card.source_width, card.source_height, 0U, sample_x, sample_y)),
                             to_byte(sample_nchw(card.pixels, card.source_width, card.source_height, 1U, sample_x, sample_y)),
                             to_byte(sample_nchw(card.pixels, card.source_width, card.source_height, 2U, sample_x, sample_y)), 255U);
    }
    if (semantics.show_masks != 0U && card.annotation_count != 0U &&
        !mmltk::backend::models::rfdetr::augment_math::erases_sample(card.erasure, normalized_x, normalized_y, card.source_width, card.source_height)) {
        float red = static_cast<float>(output.x) / 255.0F;
        float green = static_cast<float>(output.y) / 255.0F;
        float blue = static_cast<float>(output.z) / 255.0F;
        float alpha = static_cast<float>(output.w) / 255.0F;
        const bool mask_applied =
            apply_masks(red, green, blue, alpha, annotations, pairs, classes, card.annotation_offset, card.annotation_count, annotation_capacity,
                        semantics.rle_count, semantics.class_count, card.source_width, card.source_height, normalized_x, normalized_y);
        if (mask_applied && alpha > 0.0F) { output = make_uchar4(to_byte(red / alpha), to_byte(green / alpha), to_byte(blue / alpha), to_byte(alpha)); }
    }
    store(target, static_cast<int>(x), static_cast<int>(y), output);
}
__global__ void detail_base_kernel(const ExploreRenderTargetView target_view_value, const ExploreRenderDetailView view,
                                   const ExploreRenderAnnotationDescriptor* annotations, const ExploreRenderRlePair* pairs,
                                   const ExploreRenderClassDescriptor* classes, const ExploreRenderSemanticView semantics,
                                   const std::uint32_t annotation_capacity) {
    const Target target = target_view(target_view_value);
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(target.width) * target.height;
    if (index >= total) return;
    const std::uint32_t x = static_cast<std::uint32_t>(index % target.width);
    const std::uint32_t y = static_cast<std::uint32_t>(index / target.width);
    uchar4 output = view.draw_base != 0U ? make_uchar4(0U, 0U, 0U, 255U) : make_uchar4(0U, 0U, 0U, 0U);
    const float normalized_x = (static_cast<float>(x) + 0.5F) / static_cast<float>(target.width);
    const float normalized_y = (static_cast<float>(y) + 0.5F) / static_cast<float>(target.height);
    const float source_x = static_cast<float>(view.crop_x) + normalized_x * static_cast<float>(view.crop_width) - 0.5F;
    const float source_y = static_cast<float>(view.crop_y) + normalized_y * static_cast<float>(view.crop_height) - 0.5F;
    if (view.draw_base != 0U && view.pixels != nullptr) {
        output = make_uchar4(to_byte(sample_nchw(view.pixels, view.source_width, view.source_height, 0U, source_x, source_y)),
                             to_byte(sample_nchw(view.pixels, view.source_width, view.source_height, 1U, source_x, source_y)),
                             to_byte(sample_nchw(view.pixels, view.source_width, view.source_height, 2U, source_x, source_y)), 255U);
    }
    const float mask_x = (source_x + 0.5F) / static_cast<float>(view.source_width);
    const float mask_y = (source_y + 0.5F) / static_cast<float>(view.source_height);
    if (semantics.show_masks != 0U && semantics.annotation_count != 0U &&
        !mmltk::backend::models::rfdetr::augment_math::erases_sample(view.erasure, mask_x, mask_y, view.source_width, view.source_height)) {
        float red = static_cast<float>(output.x) / 255.0F;
        float green = static_cast<float>(output.y) / 255.0F;
        float blue = static_cast<float>(output.z) / 255.0F;
        float alpha = static_cast<float>(output.w) / 255.0F;
        const bool mask_applied = apply_masks(red, green, blue, alpha, annotations, pairs, classes, 0U, semantics.annotation_count, annotation_capacity,
                                              semantics.rle_count, semantics.class_count, view.source_width, view.source_height, mask_x, mask_y);
        if (mask_applied && alpha > 0.0F) { output = make_uchar4(to_byte(red / alpha), to_byte(green / alpha), to_byte(blue / alpha), to_byte(alpha)); }
    }
    store(target, static_cast<int>(x), static_cast<int>(y), output);
}
__device__ __forceinline__ void draw_box_edges(const Target target, const ExploreRenderAnnotationDescriptor annotation, const std::uint32_t offset_x,
                                               const std::uint32_t offset_y, const float scale_x, const float scale_y, const float source_origin_x,
                                               const float source_origin_y, const float source_extent_x, const float source_extent_y,
                                               const std::uint32_t clip_x, const std::uint32_t clip_y, const std::uint32_t clip_width,
                                               const std::uint32_t clip_height, const ExploreRenderClassDescriptor* classes,
                                               const std::uint32_t class_count) noexcept {
    if (!class_visible(classes, class_count, annotation.class_id)) return;
    const float x0_normalized = fmaxf(0.0F, fminf(1.0F, (annotation.box_xyxy[0] - source_origin_x) / source_extent_x));
    const float y0_normalized = fmaxf(0.0F, fminf(1.0F, (annotation.box_xyxy[1] - source_origin_y) / source_extent_y));
    const float x1_normalized = fmaxf(0.0F, fminf(1.0F, (annotation.box_xyxy[2] - source_origin_x) / source_extent_x));
    const float y1_normalized = fmaxf(0.0F, fminf(1.0F, (annotation.box_xyxy[3] - source_origin_y) / source_extent_y));
    if (x1_normalized <= x0_normalized || y1_normalized <= y0_normalized) return;
    // Object bounds are half-open pixel edges. Round the projected support
    // outward, then put the one-pixel stroke wholly outside it. Clipping below
    // discards an off-image stroke instead of moving it onto foreground.
    const int x0 = static_cast<int>(floorf(static_cast<float>(offset_x) + x0_normalized * scale_x)) - 1;
    const int y0 = static_cast<int>(floorf(static_cast<float>(offset_y) + y0_normalized * scale_y)) - 1;
    const int x1 = static_cast<int>(ceilf(static_cast<float>(offset_x) + x1_normalized * scale_x));
    const int y1 = static_cast<int>(ceilf(static_cast<float>(offset_y) + y1_normalized * scale_y));
    const std::uint32_t width = static_cast<std::uint32_t>(max(1, x1 - x0 + 1));
    const std::uint32_t height = static_cast<std::uint32_t>(max(1, y1 - y0 + 1));
    const std::uint32_t edge_pixels = 2U * width + 2U * height;
    for (std::uint32_t edge_index = threadIdx.x; edge_index < edge_pixels; edge_index += blockDim.x) {
        int x = 0;
        int y = 0;
        if (edge_index < width) {
            x = x0 + static_cast<int>(edge_index);
            y = y0;
        } else if (edge_index < 2U * width) {
            x = x0 + static_cast<int>(edge_index - width);
            y = y1;
        } else if (edge_index < 2U * width + height) {
            x = x0;
            y = y0 + static_cast<int>(edge_index - 2U * width);
        } else {
            x = x1;
            y = y0 + static_cast<int>(edge_index - 2U * width - height);
        }
        if (x >= static_cast<int>(clip_x) && y >= static_cast<int>(clip_y) && x < static_cast<int>(clip_x + clip_width) &&
            y < static_cast<int>(clip_y + clip_height) && x < static_cast<int>(target.width) && y < static_cast<int>(target.height)) {
            store(target, x, y, class_colour(classes, annotation.class_id));
        }
    }
}
__global__ void atlas_tile_box_kernel(const ExploreRenderTargetView target_view_value, const ExploreRenderCardDescriptor* cards,
                                      const ExploreRenderAnnotationDescriptor* annotations, const ExploreRenderClassDescriptor* classes,
                                      const ExploreRenderTileDescriptor* tiles, const ExploreRenderAtlasView view, const ExploreRenderTileBatchView batch,
                                      const ExploreRenderSemanticView semantics, const std::uint32_t annotation_capacity) {
    const Target target = target_view(target_view_value);
    const std::uint32_t tile_index = blockIdx.z;
    if (tile_index >= batch.tile_count || semantics.show_boxes == 0U) return;
    auto tile = tiles[tile_index];
    if (view.draw_base == 0U) tile.destination_y = static_cast<std::uint32_t>(static_cast<std::int64_t>(tile.destination_y) + tile.semantic_y_offset);
    if (tile.generation.viewport != batch.viewport_generation || tile.placeholder != 0U || tile.card_index >= view.card_count || tile.destination_width == 0U ||
        tile.destination_height == 0U || tile.destination_x >= target.width || tile.destination_y >= target.height ||
        tile.destination_width > target.width - tile.destination_x || tile.destination_height > target.height - tile.destination_y ||
        cards[tile.card_index].placeholder != 0U) {
        return;
    }
    const auto card = cards[tile.card_index];
    if (card.annotation_offset > annotation_capacity || card.annotation_count > annotation_capacity - card.annotation_offset) { return; }
    const std::uint32_t image_x = tile.destination_x + card.image_x;
    const std::uint32_t image_y = tile.destination_y + card.image_y;
    for (std::uint32_t local = 0U; local < card.annotation_count; ++local) {
        const auto annotation = annotations[card.annotation_offset + local];
        draw_box_edges(target, annotation, image_x, image_y, static_cast<float>(card.image_width), static_cast<float>(card.image_height), 0.0F, 0.0F, 1.0F,
                       1.0F, image_x, image_y, min(card.image_width, tile.destination_width - min(card.image_x, tile.destination_width)),
                       min(card.image_height, tile.destination_height - min(card.image_y, tile.destination_height)), classes, semantics.class_count);
    }
}
__global__ void detail_box_kernel(const ExploreRenderTargetView target_view_value, const ExploreRenderAnnotationDescriptor* annotations,
                                  const ExploreRenderClassDescriptor* classes, const ExploreRenderDetailView view, const ExploreRenderSemanticView semantics) {
    const Target target = target_view(target_view_value);
    const std::uint32_t annotation_index = blockIdx.x;
    if (annotation_index >= semantics.annotation_count || semantics.show_boxes == 0U) return;
    // Detail annotations are expressed in the full model-image coordinate
    // space; map them into the selected letterboxed crop before drawing.
    const float source_width = static_cast<float>(view.source_width);
    const float source_height = static_cast<float>(view.source_height);
    draw_box_edges(target, annotations[annotation_index], 0U, 0U, static_cast<float>(target.width), static_cast<float>(target.height),
                   static_cast<float>(view.crop_x) / source_width, static_cast<float>(view.crop_y) / source_height,
                   static_cast<float>(view.crop_width) / source_width, static_cast<float>(view.crop_height) / source_height, 0U, 0U, target.width,
                   target.height, classes, semantics.class_count);
}
__global__ void count_nonzero_alpha_kernel(const ExploreRenderTargetView target_view_value, unsigned long long* count) {
    const Target target = target_view(target_view_value);
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(target.width) * target.height;
    if (index >= total) return;
    const auto* row = reinterpret_cast<const uchar4*>(target.data + (index / target.width) * target.pitch);
    if (row[index % target.width].w != 0U) atomicAdd(count, 1ULL);
}
__global__ void checksum_pixels_kernel(const ExploreRenderTargetView target_view_value, unsigned long long* checksum) {
    const Target target = target_view(target_view_value);
    const unsigned long long index = static_cast<unsigned long long>(blockIdx.x) * blockDim.x + threadIdx.x;
    const unsigned long long total = static_cast<unsigned long long>(target.width) * target.height;
    if (index >= total) return;
    const auto* row = reinterpret_cast<const uchar4*>(target.data + (index / target.width) * target.pitch);
    const uchar4 pixel = row[index % target.width];
    const unsigned long long packed = static_cast<unsigned long long>(pixel.x) | (static_cast<unsigned long long>(pixel.y) << 8U) |
                                      (static_cast<unsigned long long>(pixel.z) << 16U) | (static_cast<unsigned long long>(pixel.w) << 24U);
    atomicAdd(checksum, packed * (index + 1ULL));
}
__global__ void probe_rendered_card_kernel(const ExploreRenderTargetView clean_view, const ExploreRenderTargetView semantic_view,
                                           const ExploreRenderedCardProbeAbi probe, unsigned long long* counts) {
    const Target clean = target_view(clean_view);
    const Target semantic = target_view(semantic_view);
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t total = static_cast<std::uint64_t>(clean.width) * clean.height;
    if (index >= total) return;
    const std::uint32_t x = static_cast<std::uint32_t>(index % clean.width);
    const std::uint32_t y = static_cast<std::uint32_t>(index / clean.width);
    const auto* clean_row = reinterpret_cast<const uchar4*>(clean.data + static_cast<std::size_t>(y) * clean.pitch);
    const auto* semantic_row = reinterpret_cast<const uchar4*>(semantic.data + static_cast<std::size_t>(y) * semantic.pitch);
    const uchar4 clean_pixel = clean_row[x];
    const uchar4 semantic_pixel = semantic_row[x];
    const std::uint32_t content_right = probe.content_x + probe.content_width;
    const std::uint32_t content_bottom = probe.content_y + probe.content_height;
    const bool in_content = x >= probe.content_x && y >= probe.content_y && x < content_right && y < content_bottom;
    if (in_content && (clean_pixel.x != 0U || clean_pixel.y != 0U || clean_pixel.z != 0U)) atomicAdd(counts, 1ULL);
    const bool content_column = x >= probe.content_x && x < content_right;
    const bool content_row = y >= probe.content_y && y < content_bottom;
    // Linear filtering can color the immediate outside edge during enlargement.
    // Check black padding within each band, and compare both sides of the exact
    // geometric transition with the retained physical-copy reference.
    const std::uint32_t padding_count =
        (probe.content_y != 0U && content_column && y == (probe.content_y - 1U) / 2U ? 1U : 0U) +
        (content_bottom < clean.height && content_column && y == content_bottom + (clean.height - content_bottom) / 2U ? 1U : 0U) +
        (probe.content_x != 0U && content_row && x == (probe.content_x - 1U) / 2U ? 1U : 0U) +
        (content_right < clean.width && content_row && x == content_right + (clean.width - content_right) / 2U ? 1U : 0U);
    if (padding_count != 0U && clean_pixel.x == 0U && clean_pixel.y == 0U && clean_pixel.z == 0U)
        atomicAdd(counts + 1U, static_cast<unsigned long long>(padding_count));
    const bool on_content_edge = x == probe.content_x || y == probe.content_y || x + 1U == content_right || y + 1U == content_bottom;
    if (in_content && on_content_edge) {
        const auto matches_reference = [&](const std::uint32_t column, const std::uint32_t row) {
            const auto* actual_row = reinterpret_cast<const uchar4*>(clean.data + static_cast<std::size_t>(row) * clean.pitch);
            const auto* reference_row = reinterpret_cast<const uchar4*>(probe.reference.data + static_cast<std::size_t>(row) * probe.reference.pitch_bytes);
            const auto actual = actual_row[column];
            const auto expected = reference_row[column];
            return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z && actual.w == expected.w;
        };
        if (matches_reference(x, y)) {
            const std::uint32_t matching_transitions = (probe.content_y != 0U && y == probe.content_y && matches_reference(x, y - 1U) ? 1U : 0U) +
                                                       (content_bottom < clean.height && y + 1U == content_bottom && matches_reference(x, y + 1U) ? 1U : 0U) +
                                                       (probe.content_x != 0U && x == probe.content_x && matches_reference(x - 1U, y) ? 1U : 0U) +
                                                       (content_right < clean.width && x + 1U == content_right && matches_reference(x + 1U, y) ? 1U : 0U);
            if (matching_transitions != 0U) atomicAdd(counts + 4U, static_cast<unsigned long long>(matching_transitions));
        }
    }
    const bool in_box = x >= probe.box_x && y >= probe.box_y && x < probe.box_x + probe.box_width && y < probe.box_y + probe.box_height;
    if (!in_box || semantic_pixel.w == 0U) return;
    const bool on_edge = x == probe.box_x || y == probe.box_y || x + 1U == probe.box_x + probe.box_width || y + 1U == probe.box_y + probe.box_height;
    atomicAdd(counts + (on_edge ? 2U : 3U), 1ULL);
}
__global__ void sample_rendered_card_kernel(const ExploreRenderTargetView clean, const ExploreRenderTargetView semantic,
                                            const ExploreRenderTargetView reference, const ExploreRenderedCardSampleGridAbi grid, std::uint64_t* samples) {
    const auto index = static_cast<std::size_t>(threadIdx.x);
    if (index >= grid.kSampleCount) return;
    const auto x_percent = grid.percent[index % grid.kAxisCount];
    const auto y_percent = grid.percent[index / grid.kAxisCount];
    const auto sample = [&](const ExploreRenderTargetView target) -> std::uint64_t {
        if (target.data == nullptr) return 0U;
        const auto x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x_percent) * target.width / 100U);
        const auto y = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y_percent) * target.height / 100U);
        const auto* row = reinterpret_cast<const uchar4*>(target.data + static_cast<std::size_t>(y) * target.pitch_bytes);
        const auto pixel = row[x];
        return static_cast<std::uint64_t>(pixel.x) | (static_cast<std::uint64_t>(pixel.y) << 8U) | (static_cast<std::uint64_t>(pixel.z) << 16U) |
               (static_cast<std::uint64_t>(pixel.w) << 24U);
    };
    samples[index * grid.kWordsPerSample] = sample(clean) | (sample(semantic) << 32U);
    samples[index * grid.kWordsPerSample + 1U] = sample(reference);
}
}  // namespace
cudaError_t render_explore_atlas_tiles_cuda(const ExploreRenderAtlasView& view, const ExploreRenderTileBatchView& tiles,
                                            const ExploreRenderSemanticView& semantics, const ExploreRenderScratchView& scratch,
                                            const ExploreRenderTargetView& target, const cudaStream_t stream, const ExploreRenderDemand demand) noexcept {
    const auto* cards = scratch.cards;
    const auto* annotations = scratch.annotations;
    const auto* pairs = static_cast<const ExploreRenderRlePair*>(scratch.rle_pairs);
    const auto* classes = scratch.classes;
    const auto* tile_descriptors = scratch.tiles;
    constexpr std::uint32_t threads = 256U;
    const dim3 tile_block{16U, 16U, 1U};
    const dim3 tile_grid{(tiles.max_tile_width + tile_block.x - 1U) / tile_block.x, (tiles.max_tile_height + tile_block.y - 1U) / tile_block.y,
                         tiles.tile_count};
    const dim3 tile_box_grid{1U, 1U, tiles.tile_count};
    if (!demand.valid()) return cudaSuccess;
    atlas_tile_base_kernel<<<tile_grid, tile_block, 0U, stream>>>(target, cards, annotations, pairs, classes, tile_descriptors, view, tiles, semantics,
                                                                  scratch.capacities.annotations);
    if (demand.valid() && semantics.show_boxes != 0U && semantics.annotation_count != 0U) {
        atlas_tile_box_kernel<<<tile_box_grid, threads, 0U, stream>>>(target, cards, annotations, classes, tile_descriptors, view, tiles, semantics,
                                                                      scratch.capacities.annotations);
    }
    return cudaPeekAtLastError();
}
cudaError_t render_explore_detail_cuda(const ExploreRenderDetailView& view, const ExploreRenderSemanticView& semantics, const ExploreRenderScratchView& scratch,
                                       const ExploreRenderTargetView& target, const cudaStream_t stream, const ExploreRenderDemand demand) noexcept {
    const auto* annotations = scratch.annotations;
    const auto* pairs = static_cast<const ExploreRenderRlePair*>(scratch.rle_pairs);
    const auto* classes = scratch.classes;
    constexpr std::uint32_t threads = 256U;
    const std::uint64_t pixels = static_cast<std::uint64_t>(target.width) * target.height;
    if (!demand.valid()) return cudaSuccess;
    detail_base_kernel<<<static_cast<unsigned int>((pixels + threads - 1U) / threads), threads, 0U, stream>>>(target, view, annotations, pairs, classes,
                                                                                                              semantics, scratch.capacities.annotations);
    if (demand.valid() && semantics.show_boxes != 0U && semantics.annotation_count != 0U) {
        detail_box_kernel<<<semantics.annotation_count, threads, 0U, stream>>>(target, annotations, classes, view, semantics);
    }
    return cudaPeekAtLastError();
}
cudaError_t count_explore_nonzero_alpha_cuda(const ExploreRenderTargetView& target, std::uint64_t* const count, const cudaStream_t stream) noexcept {
    constexpr std::uint32_t threads = 256U;
    const std::uint64_t pixels = static_cast<std::uint64_t>(target.width) * target.height;
    count_nonzero_alpha_kernel<<<static_cast<unsigned int>((pixels + threads - 1U) / threads), threads, 0U, stream>>>(
        target, reinterpret_cast<unsigned long long*>(count));
    return cudaPeekAtLastError();
}
cudaError_t checksum_explore_pixels_cuda(const ExploreRenderTargetView& target, std::uint64_t* const checksum, const cudaStream_t stream) noexcept {
    constexpr std::uint32_t threads = 256U;
    const std::uint64_t pixels = static_cast<std::uint64_t>(target.width) * target.height;
    checksum_pixels_kernel<<<static_cast<unsigned int>((pixels + threads - 1U) / threads), threads, 0U, stream>>>(
        target, reinterpret_cast<unsigned long long*>(checksum));
    return cudaPeekAtLastError();
}
cudaError_t probe_explore_rendered_card_cuda(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                             const ExploreRenderedCardProbeAbi& probe, std::uint64_t* const counts, const cudaStream_t stream) noexcept {
    constexpr std::uint32_t threads = 256U;
    const std::uint64_t pixels = static_cast<std::uint64_t>(clean.width) * clean.height;
    probe_rendered_card_kernel<<<static_cast<unsigned int>((pixels + threads - 1U) / threads), threads, 0U, stream>>>(
        clean, semantic, probe, reinterpret_cast<unsigned long long*>(counts));
    return cudaPeekAtLastError();
}
cudaError_t sample_explore_rendered_card_cuda(const ExploreRenderTargetView& clean, const ExploreRenderTargetView& semantic,
                                              const ExploreRenderTargetView& reference, std::uint64_t* const samples, const cudaStream_t stream) noexcept {
    sample_rendered_card_kernel<<<1U, 32U, 0U, stream>>>(clean, semantic, reference, ExploreRenderedCardSampleGridAbi{}, samples);
    return cudaPeekAtLastError();
}
}  // namespace mmltk::backend::imaging::explore::detail
