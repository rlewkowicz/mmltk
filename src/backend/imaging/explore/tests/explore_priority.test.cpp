#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
#include "src/backend/models/rfdetr/augmentation/tests/gpu_augment_test_support.h"
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "src/backend/data/compiled_format.h"
#include "src/backend/imaging/explore/explore_render_storage.h"
#include "src/controller/subsystems/explore/native_explore_storage.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"
#include "src/backend/imaging/raster/detail/raster_color.h"

import mmltk.backend.imaging.explore.explore_render_core;
import mmltk.backend.imaging.explore.compiled_explore_store;
import mmltk.backend.imaging.raster;

namespace mmltk::controller::explore_detail {

struct NativeExploreStorageTestAccess final {
    [[nodiscard]] static auto Leaves(NativeExploreStorage& storage) {
        std::vector<mmltk::backend::imaging::explore::ExploreHighWaterBuffer*> leaves;
        storage.traversal().Visit(storage.buffers_, [&](auto& buffer) { leaves.push_back(&buffer); });
        return leaves;
    }
};

}  // namespace mmltk::controller::explore_detail

namespace mmltk::backend::imaging::explore {
namespace {

TEST_CASE("Class palettes retain full-catalog hues and cyclic saturation/value dispersion", "[backend][imaging][explore]") {
    namespace color = mmltk::backend::imaging::raster::detail::color;
    for (const int count : {1, 2, 12, 13, 14, 255, 256}) {
        for (int index = 0; index < count; ++index) {
            float hue, saturation, value, next_hue, next_saturation, next_value;
            color::class_hsv(index, count, hue, saturation, value);
            color::class_hsv((index + 1) % count, count, next_hue, next_saturation, next_value);
            CHECK(std::abs(hue - 360.0F * static_cast<float>(index) / static_cast<float>(count)) < 0.0001F);
            if (count > 12) {
                CHECK(saturation != next_saturation);
                CHECK(value != next_value);
            }
            std::uint8_t red, green, blue, repeated_red, repeated_green, repeated_blue;
            color::class_color(index, count, red, green, blue);
            color::class_color(index, count, repeated_red, repeated_green, repeated_blue);
            CHECK(std::array{red, green, blue} == std::array{repeated_red, repeated_green, repeated_blue});
        }
    }
    CHECK(color::safe_class_count(0) == 1);
    CHECK(color::normalize_label(-1, 13) == 0);
    const std::array labels{12, 0, 12, 7};
    const auto complete = mmltk::backend::imaging::raster::category_colors(labels, 13);
    const std::array filtered{7, 12};
    const auto subset = mmltk::backend::imaging::raster::category_colors(filtered, 13);
    REQUIRE(complete.size() == 12U);
    REQUIRE(subset.size() == 6U);
    CHECK(std::equal(complete.begin(), complete.begin() + 3U, complete.begin() + 6U));
    CHECK(std::equal(subset.begin(), subset.begin() + 3U, complete.begin() + 9U));
    CHECK(std::equal(subset.begin() + 3U, subset.end(), complete.begin()));
}

struct HighWaterReleaseProbe final {
    std::array<std::byte, 64U> identities{};
    std::size_t next = 0U;
    bool fail_release = false;
    std::array<void*, 64U> live{};
    std::array<ExploreStorageStatus, 64U> release_failures{};
    std::size_t live_count = 0U;

    static ExploreStorageStatus Allocate(void* raw, void** output, std::size_t) noexcept {
        auto& probe = *static_cast<HighWaterReleaseProbe*>(raw);
        if (probe.next == probe.identities.size()) return static_cast<ExploreStorageStatus>(cudaErrorMemoryAllocation);
        *output = &probe.identities[probe.next++];
        probe.live[probe.live_count++] = *output;
        return kExploreStorageSuccess;
    }
    static ExploreStorageStatus Release(void* raw, void* value) noexcept {
        auto& probe = *static_cast<HighWaterReleaseProbe*>(raw);
        if (probe.fail_release) return static_cast<ExploreStorageStatus>(cudaErrorUnknown);
        const auto identity = static_cast<std::byte*>(value) - probe.identities.data();
        if (probe.release_failures[identity] != kExploreStorageSuccess) return probe.release_failures[identity];
        const auto found = std::ranges::find(probe.live, value);
        if (found != probe.live.end()) {
            *found = nullptr;
            --probe.live_count;
        }
        return kExploreStorageSuccess;
    }
    [[nodiscard]] ExploreCudaAllocationApi api() noexcept {
        return {
            .context = this,
            .allocate_device = Allocate,
            .release_device = Release,
            .allocate_pinned = Allocate,
            .release_pinned = Release,
        };
    }
};

class CudaBuffer final {
   public:
    explicit CudaBuffer(const std::size_t bytes) { REQUIRE(cudaMalloc(&data_, std::max<std::size_t>(bytes, 1U)) == cudaSuccess); }
    ~CudaBuffer() { CHECK(cudaFree(data_) == cudaSuccess); }
    CudaBuffer(const CudaBuffer&) = delete;
    CudaBuffer& operator=(const CudaBuffer&) = delete;

    template <class Value>
    void upload(const std::span<const Value> values) {
        if (!values.empty()) REQUIRE(cudaMemcpy(data_, values.data(), values.size_bytes(), cudaMemcpyHostToDevice) == cudaSuccess);
    }
    [[nodiscard]] void* data() const noexcept { return data_; }

   private:
    void* data_ = nullptr;
};

class CudaStream final {
   public:
    CudaStream() { REQUIRE(cudaStreamCreate(&stream_) == cudaSuccess); }
    ~CudaStream() {
        if (stream_ != nullptr) static_cast<void>(cudaStreamDestroy(stream_));
    }
    CudaStream(const CudaStream&) = delete;
    CudaStream& operator=(const CudaStream&) = delete;
    [[nodiscard]] cudaStream_t get() const noexcept { return stream_; }
    [[nodiscard]] std::uintptr_t address() const noexcept { return reinterpret_cast<std::uintptr_t>(stream_); }

   private:
    cudaStream_t stream_ = nullptr;
};

TEST_CASE("Rendered probes compare owned pitched RGBA references including alpha", "[backend][imaging][explore][probe]") {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) SKIP("CUDA device unavailable");
    constexpr std::size_t pitch = 32U;
    std::array<std::uint8_t, pitch * 4U> pixels{};
    for (std::size_t y = 0U; y < 4U; ++y)
        for (std::size_t x = 0U; x < 4U; ++x) {
            pixels[y * pitch + x * 4U + 1U] = 255U;
            pixels[y * pitch + x * 4U + 3U] = 255U;
        }
    auto reference = pixels;
    reference[pitch + 4U + 3U] = 0U;
    CudaBuffer clean(pixels.size());
    CudaBuffer semantic(pixels.size());
    CudaBuffer retained(reference.size());
    CudaBuffer counts(5U * sizeof(std::uint64_t));
    clean.upload<std::uint8_t>(pixels);
    semantic.upload<std::uint8_t>(pixels);
    retained.upload<std::uint8_t>(reference);
    CudaStream stream;
    REQUIRE(cudaMemsetAsync(counts.data(), 0, 5U * sizeof(std::uint64_t), stream.get()) == cudaSuccess);
    const auto target = [](CudaBuffer& buffer) {
        return ExploreRenderTargetView{.data = static_cast<std::uint8_t*>(buffer.data()), .pitch_bytes = pitch, .width = 4U, .height = 4U};
    };
    const ExploreRenderedCardProbe probe{.reference = target(retained), .content_x = 1U, .content_y = 1U,
                                         .content_width = 2U, .content_height = 2U, .box_width = 4U, .box_height = 4U};
    REQUIRE(probe_explore_rendered_card(target(clean), target(semantic), probe, static_cast<std::uint64_t*>(counts.data()), stream.address()) ==
            kExploreStorageSuccess);
    std::array<std::uint64_t, 5U> result{};
    REQUIRE(cudaMemcpyAsync(result.data(), counts.data(), sizeof(result), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    CHECK(result[0U] == 4U);
    CHECK(result[2U] == 12U);
    CHECK(result[3U] == 4U);
    CHECK(result[4U] == 6U);
}

struct SemanticOracleResult final {
    std::vector<std::array<std::uint8_t, 4U>> pixels;
    std::uint64_t nonzero_alpha = 0U;
};

class SemanticOracleBuffers final {
   public:
    SemanticOracleBuffers(const std::span<const ExploreRenderAnnotationDescriptor> annotations,
                          const std::span<const mmltk::backend::data::RLEPair> runs,
                          const std::span<const ExploreRenderClassDescriptor> classes)
        : annotations_(std::max<std::size_t>(annotations.size_bytes(), 1U)),
          runs_(std::max<std::size_t>(runs.size_bytes(), 1U)),
          classes_(std::max<std::size_t>(classes.size_bytes(), 1U)),
          annotation_count_(static_cast<std::uint32_t>(annotations.size())),
          run_count_(static_cast<std::uint32_t>(runs.size())),
          class_count_(static_cast<std::uint32_t>(classes.size())) {
        annotations_.upload<ExploreRenderAnnotationDescriptor>(annotations);
        runs_.upload<mmltk::backend::data::RLEPair>(runs);
        classes_.upload<ExploreRenderClassDescriptor>(classes);
        REQUIRE(cudaMemset(count_.data(), 0, sizeof(std::uint64_t)) == cudaSuccess);
    }

    [[nodiscard]] ExploreRenderScratchView scratch(CudaBuffer& cards, CudaBuffer* tiles = nullptr) noexcept {
        return {
            .cards = static_cast<const ExploreRenderCardDescriptor*>(cards.data()),
            .card_capacity = 1U,
            .annotations = static_cast<const ExploreRenderAnnotationDescriptor*>(annotations_.data()),
            .annotation_capacity = annotation_count_,
            .rle_pairs = static_cast<const mmltk::backend::data::RLEPair*>(runs_.data()),
            .rle_capacity = run_count_,
            .classes = static_cast<const ExploreRenderClassDescriptor*>(classes_.data()),
            .class_capacity = class_count_,
            .tiles = tiles == nullptr ? nullptr : static_cast<const ExploreRenderTileDescriptor*>(tiles->data()),
            .tile_capacity = tiles == nullptr ? 0U : 1U,
        };
    }

    [[nodiscard]] ExploreRenderSemanticView semantics(const bool boxes, const bool masks_with_boxes = false) const noexcept {
        return {
            .annotation_count = annotation_count_,
            .rle_count = run_count_,
            .class_count = class_count_,
            .show_boxes = static_cast<std::uint8_t>(boxes),
            .show_masks = static_cast<std::uint8_t>(!boxes || masks_with_boxes),
        };
    }

    [[nodiscard]] std::uint64_t* count() noexcept { return static_cast<std::uint64_t*>(count_.data()); }

    [[nodiscard]] SemanticOracleResult download(CudaBuffer& pixels, const std::size_t pixel_count, const CudaStream& stream) {
        SemanticOracleResult result{.pixels = std::vector<std::array<std::uint8_t, 4U>>(pixel_count)};
        REQUIRE(cudaMemcpyAsync(result.pixels.data(), pixels.data(), pixel_count * 4U, cudaMemcpyDeviceToHost, stream.get()) ==
                cudaSuccess);
        REQUIRE(cudaMemcpyAsync(&result.nonzero_alpha, count_.data(), sizeof(std::uint64_t), cudaMemcpyDeviceToHost, stream.get()) ==
                cudaSuccess);
        REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
        return result;
    }

   private:
    CudaBuffer annotations_;
    CudaBuffer runs_;
    CudaBuffer classes_;
    CudaBuffer count_{sizeof(std::uint64_t)};
    std::uint32_t annotation_count_ = 0U;
    std::uint32_t run_count_ = 0U;
    std::uint32_t class_count_ = 0U;
};

[[nodiscard]] ExploreRenderCardDescriptor make_card(const float* pixels, const std::uint32_t source_width,
                                                    const std::uint32_t source_height, const std::uint32_t card_extent,
                                                    const std::uint32_t annotation_count = 0U) noexcept {
    const auto contain = make_explore_contain_rect(source_width, source_height, card_extent);
    return {
        .pixels = pixels,
        .source_width = source_width,
        .source_height = source_height,
        .image_x = contain.x,
        .image_y = contain.y,
        .image_width = contain.width,
        .image_height = contain.height,
        .annotation_count = annotation_count,
    };
}

[[nodiscard]] std::array<ExploreRenderAnnotationDescriptor, 2U> make_occlusion_annotations(
    const std::array<float, 4U>& donor_box) noexcept {
    return {
        ExploreRenderAnnotationDescriptor{.box_xyxy = {0.0F, 0.0F, 1.0F, 1.0F}, .rle_count = 1U, .class_id = 0U, .occluder_index = 1},
        ExploreRenderAnnotationDescriptor{
            .box_xyxy = {donor_box[0U], donor_box[1U], donor_box[2U], donor_box[3U]},
            .rle_offset = 1U,
            .rle_count = 1U,
            .class_id = 1U,
        },
    };
}

[[nodiscard]] bool has_cuda_device() noexcept {
    int count = 0;
    // CLEANUP-IGNORE: Device availability followed by the detail oracle is not the atlas oracle's independent setup.
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

struct PixelOracleGeometry final {
    std::uint32_t width = 4U;
    std::uint32_t height = 4U;
    std::uint32_t extent = 4U;
    std::uint32_t crop_x = 0U;
    std::uint32_t crop_y = 0U;
    std::uint32_t crop_width = 0U;
    std::uint32_t crop_height = 0U;
};

[[nodiscard]] SemanticOracleResult render_semantic_oracle(const std::span<const ExploreRenderAnnotationDescriptor> annotations,
                                                          const std::span<const mmltk::backend::data::RLEPair> runs,
                                                          const std::span<const ExploreRenderClassDescriptor> classes,
                                                          const bool boxes = false,
                                                          const mmltk::backend::models::rfdetr::AugmentationSpatialErasure erasure = {},
                                                          const PixelOracleGeometry geometry = {}, const bool masks_with_boxes = false) {
    const auto kExtent = geometry.extent;
    const std::size_t kPixelCount = kExtent * kExtent;
    const std::vector<float> source(geometry.width * geometry.height * 3U);
    const std::array<ExploreRenderCardDescriptor, 1U> cards{};
    CudaBuffer source_device(source.size() * sizeof(float));
    CudaBuffer cards_device(sizeof(cards));
    SemanticOracleBuffers semantic_buffers(annotations, runs, classes);
    CudaBuffer target_device(kPixelCount * 4U);
    source_device.upload<float>(source);
    cards_device.upload<ExploreRenderCardDescriptor>(cards);
    CudaStream stream;
    const auto scratch = semantic_buffers.scratch(cards_device);
    const ExploreRenderTargetView target{
        .data = static_cast<std::uint8_t*>(target_device.data()),
        .pitch_bytes = kExtent * 4U,
        .width = kExtent,
        .height = kExtent,
    };
    const ExploreRenderDetailView detail{
        .erasure = erasure,
        .pixels = static_cast<const float*>(source_device.data()),
        .source_width = geometry.width,
        .source_height = geometry.height,
        .crop_x = geometry.crop_x,
        .crop_y = geometry.crop_y,
        .crop_width = geometry.crop_width == 0 ? geometry.width : geometry.crop_width,
        .crop_height = geometry.crop_height == 0 ? geometry.height : geometry.crop_height,
        .draw_base = 0U,
    };
    REQUIRE(render_explore_detail(detail, semantic_buffers.semantics(boxes, masks_with_boxes), scratch, target, stream.address()) ==
            kExploreStorageSuccess);
    REQUIRE(count_explore_nonzero_alpha(target, semantic_buffers.count(), stream.address()) == kExploreStorageSuccess);
    return semantic_buffers.download(target_device, kPixelCount, stream);
}

// CLEANUP-IGNORE: Atlas and detail pixel oracles call different renderers through the same semantic input types.
[[nodiscard]] SemanticOracleResult render_atlas_composition_oracle(
    const std::span<const ExploreRenderAnnotationDescriptor> annotations, const std::span<const mmltk::backend::data::RLEPair> runs,
    const std::span<const ExploreRenderClassDescriptor> classes, const bool boxes = false,
    const mmltk::backend::models::rfdetr::AugmentationSpatialErasure erasure = {},
    const PixelOracleGeometry geometry = {.width = 4U, .height = 2U}, const bool masks_with_boxes = false) {
    namespace raster = mmltk::backend::imaging::raster;
    const auto kSourceWidth = geometry.width;
    const auto kSourceHeight = geometry.height;
    const auto kExtent = geometry.extent;
    const std::size_t kPixelCount = kExtent * kExtent;
    std::vector<float> source(kSourceWidth * kSourceHeight * 3U);
    std::ranges::fill(source.begin() + 2U * kSourceWidth * kSourceHeight, source.end(), 1.0F);
    CudaBuffer source_device(source.size() * sizeof(float));
    source_device.upload<float>(source);
    std::array cards{make_card(static_cast<const float*>(source_device.data()), kSourceWidth, kSourceHeight, kExtent,
                               static_cast<std::uint32_t>(annotations.size()))};
    cards.front().erasure = erasure;
    const std::array tiles{ExploreRenderTileDescriptor{
        .destination_width = kExtent,
        .destination_height = kExtent,
        .generation = {.viewport = 11U, .tile = 1U},
    }};
    CudaBuffer cards_device(sizeof(cards));
    SemanticOracleBuffers semantic_buffers(annotations, runs, classes);
    CudaBuffer tiles_device(sizeof(tiles));
    CudaBuffer clean_device(kPixelCount * 4U);
    CudaBuffer semantic_device(kPixelCount * 4U);
    CudaBuffer composed_device(kPixelCount * 4U);
    cards_device.upload<ExploreRenderCardDescriptor>(cards);
    tiles_device.upload<ExploreRenderTileDescriptor>(tiles);
    CudaStream stream;
    const auto scratch = semantic_buffers.scratch(cards_device, &tiles_device);
    const ExploreRenderAtlasView atlas{
        .card_extent = kExtent, .card_count = 1U, .source_width = kSourceWidth, .source_height = kSourceHeight, .draw_base = 1U};
    const ExploreRenderTileBatchView batch{
        .tile_count = 1U, .tile_capacity = 1U, .max_tile_width = kExtent, .max_tile_height = kExtent, .viewport_generation = 11U};
    const ExploreRenderTargetView clean{
        .data = static_cast<std::uint8_t*>(clean_device.data()), .pitch_bytes = kExtent * 4U, .width = kExtent, .height = kExtent};
    const ExploreRenderTargetView semantic{
        .data = static_cast<std::uint8_t*>(semantic_device.data()), .pitch_bytes = kExtent * 4U, .width = kExtent, .height = kExtent};
    REQUIRE(render_explore_atlas_tiles(atlas, batch, {}, scratch, clean, stream.address()) == kExploreStorageSuccess);
    auto semantic_atlas = atlas;
    semantic_atlas.draw_base = 0U;
    REQUIRE(render_explore_atlas_tiles(semantic_atlas, batch, semantic_buffers.semantics(boxes, masks_with_boxes), scratch, semantic,
                                       stream.address()) == kExploreStorageSuccess);
    REQUIRE(count_explore_nonzero_alpha(semantic, semantic_buffers.count(), stream.address()) == kExploreStorageSuccess);
    REQUIRE(cudaMemcpyAsync(composed_device.data(), clean_device.data(), kPixelCount * 4U, cudaMemcpyDeviceToDevice, stream.get()) ==
            cudaSuccess);
    REQUIRE(
        raster::composite_rgba({
            .base_rgba = raster::pitched_rgba_target(static_cast<std::uint8_t*>(composed_device.data()), kExtent * 4U, kExtent, kExtent),
            .overlay_rgba = {static_cast<const std::uint8_t*>(semantic_device.data()), kExtent * 4U, static_cast<int>(kExtent),
                             static_cast<int>(kExtent)},
            .stream = stream.get(),
        }) == cudaSuccess);
    return semantic_buffers.download(composed_device, kPixelCount, stream);
}

TEST_CASE("Explore tiny support keeps exact outer edges under atlas and detail scaling", "[backend][imaging][explore][cuda][support]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    namespace augment = mmltk::backend::models::rfdetr;
    using mmltk::backend::data::RLEPair;
    constexpr std::uint32_t width = 8U;
    constexpr std::uint32_t height = 4U;
    const auto nearest_pixel = [](const float position) {
        const int cell = static_cast<int>(std::floor(position));
        // Equidistant pixel centers select the even index.
        return cell - static_cast<int>(position == static_cast<float>(cell) && (cell & 1) != 0);
    };
    const std::array classes{ExploreRenderClassDescriptor{.color = {255U, 0U, 0U}}};
    // Single pixels at every corner, two-pixel dots, and horizontal/vertical filaments.
    const std::array<std::array<std::uint32_t, 4>, 8> rectangles{
        {{0, 0, 1, 1}, {7, 0, 8, 1}, {0, 3, 1, 4}, {7, 3, 8, 4}, {3, 1, 4, 2}, {3, 1, 5, 2}, {2, 1, 6, 2}, {3, 0, 4, 4}}};
    for (const auto& rectangle : rectangles) {
        CAPTURE(rectangle);
        std::vector<RLEPair> runs;
        for (auto y = rectangle[1]; y < rectangle[3]; ++y) {
            runs.push_back({y * width + rectangle[0], rectangle[2] - rectangle[0]});
        }
        const std::array<float, 4> identity_bounds{float(rectangle[0]) / width, float(rectangle[1]) / height, float(rectangle[2]) / width,
                                                   float(rectangle[3]) / height};
        const auto support = augment::resolve_augmentation_annotation_support({0, 0, 1, 1}, runs, width, height, nullptr);
        REQUIRE(support.present);
        REQUIRE(support.box_xyxy == identity_bounds);
        REQUIRE(support.area_pixels == static_cast<float>((rectangle[2] - rectangle[0]) * (rectangle[3] - rectangle[1])));
        for (int transform = 0; transform < 4; ++transform) {
            CAPTURE(transform);
            const auto plan = augment::test_support::small_object_plan(transform);
            // Integer pixel cells give independent exact edges for these fixed transforms.
            const auto edges = augment::test_support::small_object_edges(
                {int(rectangle[0]), int(rectangle[1]), int(rectangle[2]), int(rectangle[3])}, transform);
            const bool present = edges[0] < edges[2] && edges[1] < edges[3];
            const auto resolved = augment::resolve_augmentation_annotation_support(identity_bounds, runs, width, height, &plan);
            REQUIRE(resolved.present == present);
            if (!present) continue;
            const std::array<float, 4> expected{float(edges[0]) / 8, float(edges[1]) / 4, float(edges[2]) / 8, float(edges[3]) / 4};
            REQUIRE(resolved.box_xyxy == expected);
            ExploreRenderAnnotationDescriptor annotation{.box_xyxy = {expected[0], expected[1], expected[2], expected[3]},
                                                         .rle_count = static_cast<std::uint32_t>(runs.size())};
            std::ranges::copy(plan.inverse, annotation.inverse);
            for (const auto extent : {3U, 8U, 13U, 32U}) {
                const PixelOracleGeometry geometry{.width = width, .height = height, .extent = extent};
                for (const bool atlas : {false, true}) {
                    CAPTURE(extent, atlas);
                    const auto render = [&](const bool boxes) {
                        return atlas
                                   ? render_atlas_composition_oracle(std::span{&annotation, 1U}, runs, classes, boxes, plan.erasure,
                                                                     geometry, true)
                                   : render_semantic_oracle(std::span{&annotation, 1U}, runs, classes, boxes, plan.erasure, geometry, true);
                    };
                    const auto masks = render(false);
                    const auto combined = render(true);
                    auto hidden_classes = classes;
                    hidden_classes[0].visible = 0U;
                    const auto hidden = atlas ? render_atlas_composition_oracle(std::span{&annotation, 1U}, runs, hidden_classes, true,
                                                                                plan.erasure, geometry, true)
                                              : render_semantic_oracle(std::span{&annotation, 1U}, runs, hidden_classes, true, plan.erasure,
                                                                       geometry, true);
                    const auto image = atlas ? make_explore_contain_rect(width, height, extent)
                                             : decltype(make_explore_contain_rect(width, height, extent)){0, 0, extent, extent};
                    const int left =
                        static_cast<int>(std::floor(static_cast<float>(image.x) + expected[0] * static_cast<float>(image.width)));
                    const int top =
                        static_cast<int>(std::floor(static_cast<float>(image.y) + expected[1] * static_cast<float>(image.height)));
                    const int right =
                        static_cast<int>(std::ceil(static_cast<float>(image.x) + expected[2] * static_cast<float>(image.width)));
                    const int bottom =
                        static_cast<int>(std::ceil(static_cast<float>(image.y) + expected[3] * static_cast<float>(image.height)));
                    for (std::uint32_t y = 0; y < extent; ++y)
                        for (std::uint32_t x = 0; x < extent; ++x) {
                            const auto index = y * extent + x;
                            const bool inside_image =
                                x >= image.x && y >= image.y && x < image.x + image.width && y < image.y + image.height;
                            const bool exterior = ((int(x) == left - 1 || int(x) == right) && int(y) >= top - 1 && int(y) <= bottom) ||
                                                  ((int(y) == top - 1 || int(y) == bottom) && int(x) >= left - 1 && int(x) <= right);
                            // Independently sample explicit source RLE at the destination pixel center.
                            const float nx = (float(x) - static_cast<float>(image.x) + 0.5F) / static_cast<float>(image.width);
                            const float ny = (float(y) - static_cast<float>(image.y) + 0.5F) / static_cast<float>(image.height);
                            const float sx = transform == 1 ? 1 - nx : transform == 2 ? nx / 2 + 0.25F : nx;
                            const float sy = transform == 1 ? 1 - ny : transform == 2 ? ny / 2 + 0.25F : ny;
                            const int source_x = nearest_pixel(sx * width);
                            const int source_y = nearest_pixel(sy * height);
                            const int source_pixel = source_y * int(width) + source_x;
                            const bool supported =
                                inside_image && sx >= 0 && sx < 1 && sy >= 0 && sy < 1 && !(transform == 3 && nx >= 0.5F) &&
                                std::ranges::any_of(
                                    runs,
                                    [&](const auto run) {
                                        return source_pixel >= int(run.start) && source_pixel < int(run.start + run.length);
                                    });
                            const std::array<std::uint8_t, 4> clear = atlas ? (inside_image ? std::array<std::uint8_t, 4>{0, 0, 255, 255}
                                                                                            : std::array<std::uint8_t, 4>{24, 18, 35, 255})
                                                                            : std::array<std::uint8_t, 4>{0, 0, 0, 0};
                            const std::array<std::uint8_t, 4> foreground =
                                atlas ? std::array<std::uint8_t, 4>{92, 0, 163, 255} : std::array<std::uint8_t, 4>{255, 0, 0, 92};
                            CHECK(masks.pixels[index] == (supported ? foreground : clear));
                            CHECK(hidden.pixels[index] == clear);
                            if (supported) {
                                CHECK_FALSE(exterior);
                                CHECK(combined.pixels[index] == foreground);
                            } else if (inside_image && exterior) {
                                CHECK(combined.pixels[index] == std::array<std::uint8_t, 4>{255U, 0U, 0U, 255U});
                            } else
                                CHECK(combined.pixels[index] == clear);
                        }
                }
            }
        }
    }
}

TEST_CASE("Explore cropped detail clips exterior edges without painting surviving pixels", "[backend][imaging][explore][cuda][support]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    const std::array annotations{ExploreRenderAnnotationDescriptor{.box_xyxy = {0.25F, 0.25F, 0.75F, 0.5F}, .rle_count = 1}};
    const std::array runs{mmltk::backend::data::RLEPair{10, 4}};
    const std::array classes{ExploreRenderClassDescriptor{}};
    const PixelOracleGeometry geometry{.width = 8, .height = 4, .extent = 13, .crop_x = 3, .crop_y = 1, .crop_width = 2, .crop_height = 1};
    const auto masks = render_semantic_oracle(annotations, runs, classes, false, {}, geometry);
    const auto combined = render_semantic_oracle(annotations, runs, classes, true, {}, geometry, true);
    REQUIRE(masks.nonzero_alpha == 13U * 13U);
    CHECK(combined.pixels == masks.pixels);
}

TEST_CASE("current focus and viewport precede cursor residency", "[backend][imaging][explore]") {
    constexpr std::array<std::uint32_t, 8U> kOrder{10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U};
    std::array<std::uint32_t, kOrder.size()> output{};

    const std::size_t count =
        prioritize_explore_work(kOrder, ExploreViewport{.first_row = 1U, .row_count = 1U, .columns = 2U}, 15U, 4U, output);

    REQUIRE(count == output.size());
    CHECK((output == std::array<std::uint32_t, 8U>{15U, 12U, 13U, 14U, 16U, 17U, 10U, 11U}));
}

TEST_CASE("atlas target prefix retains exact visible slot order", "[backend][imaging][explore]") {
    constexpr std::array<std::uint32_t, 8U> kOrder{10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U};
    std::array<std::uint32_t, kOrder.size()> output{};

    const std::size_t count =
        prioritize_explore_work(kOrder, ExploreViewport{.first_row = 1U, .row_count = 2U, .columns = 2U}, std::nullopt, 6U, output);

    REQUIRE(count == output.size());
    CHECK((output == std::array<std::uint32_t, 8U>{12U, 13U, 14U, 15U, 16U, 17U, 10U, 11U}));
}

TEST_CASE("priority output remains fixed capacity", "[backend][imaging][explore]") {
    std::array<std::uint32_t, kExploreWorkCapacity + 32U> order{};
    std::iota(order.begin(), order.end(), 0U);
    std::array<std::uint32_t, kExploreWorkCapacity + 32U> output{};

    const std::size_t count = prioritize_explore_work(
        order, ExploreViewport{.first_row = 0U, .row_count = static_cast<std::uint32_t>(order.size()), .columns = 1U}, std::nullopt, 0U,
        output);

    REQUIRE(count == kExploreWorkCapacity);
    for (std::size_t index = 0U; index != count; ++index)
        CHECK(output[index] == index);
}

TEST_CASE("Explore high-water storage preserves every identity after failed release", "[backend][imaging][explore]") {
    HighWaterReleaseProbe probe;
    ExploreHighWaterBuffer buffer;
    buffer.bind(probe.api());
    REQUIRE(buffer.ensure_bytes(16U));
    REQUIRE(probe.live_count == 1U);

    probe.fail_release = true;
    CHECK_FALSE(buffer.ensure_bytes(32U));
    CHECK(buffer.owns_allocation());
    CHECK(probe.live_count == 2U);
    CHECK(buffer.reset() != kExploreStorageSuccess);
    CHECK(buffer.owns_allocation());
    CHECK(probe.live_count == 2U);

    probe.fail_release = false;
    CHECK(buffer.reset() == kExploreStorageSuccess);
    CHECK_FALSE(buffer.owns_allocation());
    CHECK(probe.live_count == 0U);
}

TEST_CASE("Explore fixed storage retries every retained leaf and preserves the first release failure", "[backend][imaging][explore]") {
    using mmltk::controller::explore_detail::NativeExploreStorage;
    using mmltk::controller::explore_detail::NativeExploreStorageTestAccess;
    NativeExploreStorage inventory;
    const auto leaf_count = NativeExploreStorageTestAccess::Leaves(inventory).size();
    REQUIRE(leaf_count == 15U);  // eleven buffers and both two-element caches
    for (std::size_t selected = 0U; selected != leaf_count; ++selected) {
        for (const auto status : {cudaErrorUnknown, cudaErrorContextIsDestroyed}) {
            INFO("leaf=" << selected << " failure=" << static_cast<int>(status));
            HighWaterReleaseProbe probe;
            NativeExploreStorage storage;
            storage.Bind(probe.api());
            const auto leaves = NativeExploreStorageTestAccess::Leaves(storage);
            for (auto* leaf : leaves)
                REQUIRE(leaf->ensure_bytes(16U));
            auto* const original = leaves[selected]->data();
            probe.release_failures[selected] = static_cast<ExploreStorageStatus>(status);
            // Retryable failures retain a pending replacement; unproved
            // failures retain the candidate. Both must survive teardown.
            CHECK_FALSE(leaves[selected]->ensure_bytes(32U));
            CHECK(leaves[selected]->data() == original);
            REQUIRE(probe.live_count == leaf_count + 1U);
            probe.release_failures[leaf_count] = static_cast<ExploreStorageStatus>(status);
            const auto failed = storage.ResetChecked();
            CHECK_FALSE(failed.all_released);
            CHECK(failed.failure == static_cast<ExploreStorageStatus>(status));
            CHECK(storage.OwnsAllocation());
            CHECK(probe.live_count == 2U);
            CHECK(leaves[selected]->data() == original);
            for (std::size_t index = 0U; index != leaves.size(); ++index)
                CHECK(leaves[index]->owns_allocation() == (index == selected));
            probe.release_failures.fill(kExploreStorageSuccess);
            const auto retried = storage.ResetChecked();
            CHECK(retried.all_released);
            CHECK(retried.failure == kExploreStorageSuccess);
            CHECK_FALSE(storage.OwnsAllocation());
            CHECK(probe.live_count == 0U);
            CHECK(storage.ResetChecked().all_released);
        }
    }
    HighWaterReleaseProbe probe;
    inventory.Bind(probe.api());
    const auto leaves = NativeExploreStorageTestAccess::Leaves(inventory);
    for (auto* leaf : leaves)
        REQUIRE(leaf->ensure_bytes(8U));
    probe.release_failures[1U] = static_cast<ExploreStorageStatus>(cudaErrorInvalidValue);
    probe.release_failures[leaf_count - 1U] = static_cast<ExploreStorageStatus>(cudaErrorUnknown);
    CHECK(inventory.ResetChecked().failure == static_cast<ExploreStorageStatus>(cudaErrorInvalidValue));
    CHECK(probe.live_count == 2U);
    probe.release_failures.fill(kExploreStorageSuccess);
    CHECK(inventory.ResetChecked().all_released);
}

TEST_CASE("Explore gallery contain geometry preserves complete non-square backing", "[backend][imaging][explore]") {
    CHECK((make_explore_contain_rect(640U, 320U, 200U) == ExploreContainRect{.x = 0U, .y = 50U, .width = 200U, .height = 100U}));
    CHECK((make_explore_contain_rect(320U, 640U, 200U) == ExploreContainRect{.x = 50U, .y = 0U, .width = 100U, .height = 200U}));
    CHECK((make_explore_contain_rect(640U, 640U, 200U) == ExploreContainRect{.x = 0U, .y = 0U, .width = 200U, .height = 200U}));
    CHECK(make_explore_contain_rect(0U, 640U, 200U) == ExploreContainRect{});
}

TEST_CASE("Explore CUDA atlas and detail preserve pixels, padding, and bilinear ramps", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    std::uint32_t source_width = 4U;
    constexpr std::uint32_t kSourceHeight = 2U;
    constexpr std::uint32_t kCardExtent = 4U;
    std::array<float, 24U> source{};
    bool ramp = false;
    SECTION("wide red image with padding") { std::ranges::fill(source.begin(), source.begin() + 8U, 1.0F); }
    SECTION("bilinear corner and horizontal and vertical ramps") {
        source_width = 2U;
        ramp = true;
        // Red has only its top-right corner set. Green is x, blue is y.
        source = {0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1};
    }
    CudaBuffer source_device(source.size() * sizeof(float));
    source_device.upload<float>(source);
    std::array cards{make_card(static_cast<const float*>(source_device.data()), source_width, kSourceHeight, kCardExtent)};
    const std::array tiles{ExploreRenderTileDescriptor{
        .destination_width = kCardExtent,
        .destination_height = kCardExtent,
        .generation = {.viewport = 7U, .tile = 1U},
    }};
    const std::array<ExploreRenderAnnotationDescriptor, 1U> annotations{};
    const std::array<ExploreRenderClassDescriptor, 1U> classes{};
    CudaBuffer cards_device(sizeof(cards));
    CudaBuffer annotations_device(sizeof(annotations));
    CudaBuffer classes_device(sizeof(classes));
    CudaBuffer tiles_device(sizeof(tiles));
    CudaBuffer target_device(kCardExtent * kCardExtent * 4U);
    cards_device.upload<ExploreRenderCardDescriptor>(cards);
    annotations_device.upload<ExploreRenderAnnotationDescriptor>(annotations);
    classes_device.upload<ExploreRenderClassDescriptor>(classes);
    tiles_device.upload<ExploreRenderTileDescriptor>(tiles);
    CudaStream stream;
    const ExploreRenderScratchView scratch{
        .cards = static_cast<const ExploreRenderCardDescriptor*>(cards_device.data()),
        .card_capacity = 1U,
        .annotations = static_cast<const ExploreRenderAnnotationDescriptor*>(annotations_device.data()),
        .classes = static_cast<const ExploreRenderClassDescriptor*>(classes_device.data()),
        .class_capacity = 1U,
        .tiles = static_cast<const ExploreRenderTileDescriptor*>(tiles_device.data()),
        .tile_capacity = 1U,
    };
    const ExploreRenderTargetView target{.data = static_cast<std::uint8_t*>(target_device.data()),
                                         .pitch_bytes = kCardExtent * 4U,
                                         .width = kCardExtent,
                                         .height = kCardExtent};
    REQUIRE(
        render_explore_atlas_tiles(
            {.card_extent = kCardExtent, .card_count = 1U, .source_width = source_width, .source_height = kSourceHeight, .draw_base = 1U},
            {.tile_count = 1U,
             .tile_capacity = 1U,
             .max_tile_width = kCardExtent,
             .max_tile_height = kCardExtent,
             .viewport_generation = 7U},
            {.class_count = 1U, .show_boxes = 0U, .show_masks = 0U}, scratch, target, stream.address()) == kExploreStorageSuccess);
    std::array<std::array<std::uint8_t, 4U>, kCardExtent * kCardExtent> pixels{};
    REQUIRE(cudaMemcpyAsync(pixels.data(), target_device.data(), pixels.size() * 4U, cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);

    if (ramp) {
        // At destination (1,2), source (x,y)=(.25,.75). The red
        // corner contributes .25*(1-.75)=.0625, rounded to 16.
        CHECK((pixels[9U] == std::array<std::uint8_t, 4U>{16U, 64U, 191U, 255U}));
        CHECK((pixels[0U] == std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}));
        CHECK((pixels[15U] == std::array<std::uint8_t, 4U>{0U, 255U, 255U, 255U}));
        const auto atlas_pixels = pixels;
        REQUIRE(render_explore_detail({.pixels = static_cast<const float*>(source_device.data()),
                                       .source_width = source_width,
                                       .source_height = kSourceHeight,
                                       .crop_width = source_width,
                                       .crop_height = kSourceHeight},
                                      {.show_boxes = 0U, .show_masks = 0U}, scratch, target, stream.address()) == kExploreStorageSuccess);
        REQUIRE(cudaMemcpyAsync(pixels.data(), target_device.data(), pixels.size() * 4U, cudaMemcpyDeviceToHost, stream.get()) ==
                cudaSuccess);
        REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
        CHECK(pixels == atlas_pixels);
    } else {
        for (std::uint32_t x = 0U; x != kCardExtent; ++x) {
            CHECK((pixels[x] == std::array<std::uint8_t, 4U>{24U, 18U, 35U, 255U}));
            CHECK((pixels[3U * kCardExtent + x] == std::array<std::uint8_t, 4U>{24U, 18U, 35U, 255U}));
            CHECK((pixels[kCardExtent + x] == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U}));
            CHECK((pixels[2U * kCardExtent + x] == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U}));
        }
    }
}

TEST_CASE("Semantic upscaling preserves class color and alpha at exact nearest samples", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    const std::array<std::uint8_t, 8U> source{255U, 0U, 0U, 92U, 0U, 255U, 255U, 0U};
    CudaBuffer input{source.size()};
    CudaBuffer output{8U * 4U * 4U};
    input.upload<std::uint8_t>(source);
    CudaStream stream;
    REQUIRE(mmltk::backend::imaging::raster::scale_rgba_nearest({static_cast<const std::uint8_t*>(input.data()), 8U, 2, 1},
                                                                {static_cast<std::uint8_t*>(output.data()), 32U, 8, 4},
                                                                stream.address()) == cudaSuccess);
    std::array<std::uint8_t, 128U> pixels{};
    REQUIRE(cudaMemcpyAsync(pixels.data(), output.data(), pixels.size(), cudaMemcpyDeviceToHost, stream.get()) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
    for (std::size_t y = 0U; y < 4U; ++y)
        for (std::size_t x = 0U; x < 8U; ++x)
            for (std::size_t channel = 0U; channel < 4U; ++channel)
                CHECK(pixels[(y * 8U + x) * 4U + channel] == source[(x / 4U) * 4U + channel]);
}

TEST_CASE("Explore atlas semantic planes compose through the Presentation raster path", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    constexpr std::array<std::uint8_t, 4U> kBase{0U, 0U, 255U, 255U};
    constexpr std::array<std::uint8_t, 4U> kPadding{24U, 18U, 35U, 255U};
    ExploreRenderAnnotationDescriptor source{.box_xyxy = {0.0F, 0.0F, 1.0F, 1.0F}, .rle_count = 1U, .class_id = 0U};
    const std::array single_run{mmltk::backend::data::RLEPair{.start = 0U, .length = 1U}};
    std::array classes{ExploreRenderClassDescriptor{}, ExploreRenderClassDescriptor{}};

    const auto visible = render_atlas_composition_oracle(std::span{&source, 1U}, single_run, classes);
    REQUIRE(visible.nonzero_alpha == 1U);
    CHECK(visible.pixels[4U] != kBase);
    CHECK(visible.pixels[5U] == kBase);
    CHECK(visible.pixels[0U] == kPadding);
    CHECK(visible.pixels[15U] == kPadding);

    classes[0].visible = 0U;
    const auto hidden = render_atlas_composition_oracle(std::span{&source, 1U}, single_run, classes);
    CHECK(hidden.nonzero_alpha == 0U);
    CHECK(hidden.pixels[4U] == kBase);
    classes[0].visible = 1U;
    source.rle_count = 0U;
    const auto empty = render_atlas_composition_oracle(std::span{&source, 1U}, std::span<const mmltk::backend::data::RLEPair>{}, classes);
    CHECK(empty.nonzero_alpha == 0U);
    CHECK(empty.pixels[4U] == kBase);

    source.rle_count = 1U;
    source.inverse[0] = -1.0F;
    source.inverse[2] = 1.0F;
    const auto transformed = render_atlas_composition_oracle(std::span{&source, 1U}, single_run, classes);
    CHECK(transformed.nonzero_alpha == 1U);
    CHECK(transformed.pixels[7U] != kBase);
    CHECK(transformed.pixels[4U] == kBase);

    auto annotations = make_occlusion_annotations({0.0F, 0.0F, 0.25F, 0.5F});
    const std::array occluded_runs{mmltk::backend::data::RLEPair{.start = 0U, .length = 8U},
                                   mmltk::backend::data::RLEPair{.start = 0U, .length = 1U}};
    const auto occluded = render_atlas_composition_oracle(annotations, occluded_runs, classes);
    annotations[0].rle_count = 0U;
    const auto donor_only = render_atlas_composition_oracle(annotations, occluded_runs, classes);
    CHECK(occluded.pixels[4U] == donor_only.pixels[4U]);
    CHECK(occluded.pixels[5U] != kBase);

    annotations[0].rle_count = 1U;
    classes[1].visible = 0U;
    const auto hidden_donor = render_atlas_composition_oracle(annotations, occluded_runs, classes);
    CHECK(hidden_donor.pixels[4U] != donor_only.pixels[4U]);
    annotations[0].occluder_index = -1;
    std::ranges::copy(std::array{0.25F, 0.25F, 0.75F, 0.75F}, annotations[0].box_xyxy);
    const auto visible_box =
        render_atlas_composition_oracle(std::span{annotations}.first(1U), std::span<const mmltk::backend::data::RLEPair>{}, classes, true);
    CHECK(visible_box.nonzero_alpha != 0U);
    classes[0].visible = 0U;
    const auto hidden_box =
        render_atlas_composition_oracle(std::span{annotations}.first(1U), std::span<const mmltk::backend::data::RLEPair>{}, classes, true);
    CHECK(hidden_box.nonzero_alpha == 0U);
}

TEST_CASE("Explore CUDA masks admit checked RLE and produce semantic composition pixels", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    ExploreRenderAnnotationDescriptor annotation{.box_xyxy = {0.0F, 0.0F, 1.0F, 1.0F}, .rle_count = 1U, .class_id = 0U};
    const std::array runs{mmltk::backend::data::RLEPair{.start = 5U, .length = 1U}};
    std::array classes{ExploreRenderClassDescriptor{}};

    const auto visible = render_semantic_oracle(std::span{&annotation, 1U}, runs, classes);
    CHECK(visible.nonzero_alpha == 1U);
    CHECK(visible.pixels[5U][3U] == 92U);
    CHECK(visible.pixels[0U][3U] == 0U);

    classes[0].visible = 0U;
    const auto hidden = render_semantic_oracle(std::span{&annotation, 1U}, runs, classes);
    CHECK(hidden.nonzero_alpha == 0U);

    classes[0].visible = 1U;
    annotation.rle_count = 0U;
    CHECK(render_semantic_oracle(std::span{&annotation, 1U}, std::span<const mmltk::backend::data::RLEPair>{}, classes).nonzero_alpha ==
          0U);
    annotation.rle_offset = 1U;
    annotation.rle_count = 1U;
    CHECK(render_semantic_oracle(std::span{&annotation, 1U}, runs, classes).nonzero_alpha == 0U);

    annotation.rle_offset = 0U;
    annotation.inverse[0] = -1.0F;
    annotation.inverse[2] = 1.0F;
    const auto transformed = render_semantic_oracle(std::span{&annotation, 1U}, runs, classes);
    CHECK(transformed.nonzero_alpha == 1U);
    CHECK(transformed.pixels[6U][3U] == 92U);
    CHECK(transformed.pixels[5U][3U] == 0U);
}

TEST_CASE("Explore renderer rejects semantic descriptors beyond admitted storage", "[backend][imaging][explore]") {
    const auto* const address = reinterpret_cast<const std::byte*>(1U);
    const ExploreRenderScratchView scratch{
        .cards = reinterpret_cast<const ExploreRenderCardDescriptor*>(address),
        .card_capacity = 1U,
        .annotations = reinterpret_cast<const ExploreRenderAnnotationDescriptor*>(address),
        .classes = reinterpret_cast<const ExploreRenderClassDescriptor*>(address),
        .class_capacity = 1U,
    };
    const ExploreRenderDetailView detail{
        .pixels = reinterpret_cast<const float*>(address), .source_width = 1U, .source_height = 1U, .crop_width = 1U, .crop_height = 1U};
    const ExploreRenderTargetView target{.data = reinterpret_cast<std::uint8_t*>(1U), .pitch_bytes = 4U, .width = 1U, .height = 1U};
    CHECK(render_explore_detail(detail, {.annotation_count = 1U, .class_count = 1U}, scratch, target, 1U) != kExploreStorageSuccess);
}

TEST_CASE("Explore CUDA donor occlusion remains independent of semantic class visibility", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    auto annotations = make_occlusion_annotations({0.0F, 0.0F, 1.0F, 1.0F});
    const std::array runs{mmltk::backend::data::RLEPair{.start = 0U, .length = 16U},
                          mmltk::backend::data::RLEPair{.start = 5U, .length = 1U}};
    std::array classes{ExploreRenderClassDescriptor{}, ExploreRenderClassDescriptor{}};
    const auto donor_visible = render_semantic_oracle(annotations, runs, classes);
    CHECK(donor_visible.nonzero_alpha == 16U);
    const auto donor_pixel = donor_visible.pixels[5U];

    annotations[0].rle_count = 0U;
    const auto donor_only = render_semantic_oracle(annotations, runs, classes);
    CHECK(donor_pixel == donor_only.pixels[5U]);

    annotations[0].rle_count = 1U;
    classes[1].visible = 0U;
    const auto hidden_donor = render_semantic_oracle(annotations, runs, classes);
    CHECK(hidden_donor.nonzero_alpha == 15U);
    CHECK(hidden_donor.pixels[5U][3U] == 0U);
    CHECK(hidden_donor.pixels[5U] != donor_pixel);
}

TEST_CASE("Explore CUDA class filtering applies to box composition", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    const std::array annotations{ExploreRenderAnnotationDescriptor{.box_xyxy = {0.25F, 0.25F, 0.75F, 0.75F}, .class_id = 0U}};
    std::array classes{ExploreRenderClassDescriptor{}};
    CHECK(render_semantic_oracle(annotations, std::span<const mmltk::backend::data::RLEPair>{}, classes, true).nonzero_alpha != 0U);
    classes[0].visible = 0U;
    CHECK(render_semantic_oracle(annotations, std::span<const mmltk::backend::data::RLEPair>{}, classes, true).nonzero_alpha == 0U);
}

TEST_CASE("Explore detail and atlas mask support follows final spatial erasure", "[backend][imaging][explore][cuda]") {
    if (!has_cuda_device()) { SKIP("CUDA unavailable"); }
    namespace rfdetr = mmltk::backend::models::rfdetr;
    const std::array annotations{ExploreRenderAnnotationDescriptor{.box_xyxy = {0.0F, 0.0F, 1.0F, 1.0F}, .rle_count = 1U}};
    const std::array runs{mmltk::backend::data::RLEPair{.start = 0U, .length = 16U}};
    const std::array classes{ExploreRenderClassDescriptor{}};
    const std::array erasures{
        rfdetr::AugmentationSpatialErasure{},
        rfdetr::AugmentationSpatialErasure{.key = 37U, .dropout_probability = 0.5F},
        rfdetr::AugmentationSpatialErasure{.x1 = 0.5F, .y1 = 0.5F, .rectangular = 1U},
        rfdetr::AugmentationSpatialErasure{.dropout_probability = 1.0F},
    };
    for (const auto& erasure : erasures) {
        const auto detail = render_semantic_oracle(annotations, runs, classes, false, erasure);
        std::uint64_t expected_detail = 0U;
        std::uint64_t expected_atlas = 0U;
        for (std::int64_t y = 0; y < 4; ++y) {
            for (std::int64_t x = 0; x < 4; ++x) {
                const bool visible = !rfdetr::augment_math::erases_pixel(erasure, x, y, 4, 4);
                CHECK((detail.pixels[static_cast<std::size_t>(y * 4 + x)][3U] != 0U) == visible);
                expected_detail += visible;
                if (y < 2) { expected_atlas += !rfdetr::augment_math::erases_pixel(erasure, x, y, 4, 2); }
            }
        }
        CHECK(detail.nonzero_alpha == expected_detail);
        const auto atlas = render_atlas_composition_oracle(annotations, runs, classes, false, erasure);
        CHECK(atlas.nonzero_alpha == expected_atlas);
    }
    const auto donor_annotations = make_occlusion_annotations({0.0F, 0.0F, 1.0F, 1.0F});
    const std::array donor_runs{runs.front(), mmltk::backend::data::RLEPair{.start = 5U, .length = 1U}};
    const std::array donor_classes{classes.front(), classes.front()};
    const auto donor = render_semantic_oracle(donor_annotations, donor_runs, donor_classes, false, erasures[2]);
    CHECK(donor.nonzero_alpha == 12U);
    CHECK(donor.pixels[5U][3U] == 0U);
    CHECK(donor.pixels[6U][3U] == 92U);
    const auto boxes = render_semantic_oracle(annotations, runs, classes, true);
    CHECK(render_semantic_oracle(annotations, runs, classes, true, erasures.back()).nonzero_alpha == boxes.nonzero_alpha);
}

}  // namespace
}  // namespace mmltk::backend::imaging::explore
