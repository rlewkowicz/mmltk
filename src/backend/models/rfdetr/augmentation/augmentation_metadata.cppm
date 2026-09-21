module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
export module mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
export namespace mmltk::backend::models::rfdetr {
struct AugmentationMappedInstance {
    std::array<float, 4> source_box_xyxy{};
    std::array<float, 4> output_box_xyxy{};
    std::array<float, 4> mask_bounds{};
    float source_area_pixels = 0.0F;
    float output_area = 0.0F;
    bool visible = false;
};
struct AugmentationPreviewAnnotation {
    // Sources retain their original ordinal after culling. The appended
    // donor uses source_instances.size().
    std::size_t source_ordinal = 0U;
    float visible_area_pixels = 0;
    AugmentationSpatialErasure erasure;
    std::array<float, 4> box_xyxy{};
    std::array<float, 4> mask_bounds{};
    bool mask_present = false;
    std::array<float, kAugmentationTransformSize> inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    std::array<float, kAugmentationTransformSize> occluder_inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    decltype(mmltk::backend::data::PackedInstance::mask_rle_offset) mask_rle_offset = 0U;
    std::uint16_t mask_rle_pairs = 0U;
    std::uint16_t class_id = 0U;
    std::int32_t occluder_index = -1;
};
using ::mmltk::backend::models::rfdetr::augmentation_box_area;
using ::mmltk::backend::models::rfdetr::transform_augmentation_box_xyxy;
[[nodiscard]] inline AugmentationMappedInstance map_augmentation_instance(const mmltk::backend::data::PackedInstance& instance, int image_width,
                                                                          int image_height, const AugmentationImagePlan* plan,
                                                                          std::span<const mmltk::backend::data::RLEPair> mask = {}) {
    const float inverse_width = 1.0F / static_cast<float>(image_width);
    const float inverse_height = 1.0F / static_cast<float>(image_height);
    AugmentationMappedInstance mapped;
    mapped.source_box_xyxy = {
        static_cast<float>(instance.bbox_x1) * inverse_width,
        static_cast<float>(instance.bbox_y1) * inverse_height,
        static_cast<float>(instance.bbox_x2) * inverse_width,
        static_cast<float>(instance.bbox_y2) * inverse_height,
    };
    mapped.source_area_pixels = augmentation_box_area(mapped.source_box_xyxy) * static_cast<float>(image_width) * static_cast<float>(image_height);
    const auto support = resolve_augmentation_annotation_support(mapped.source_box_xyxy, mask, image_width, image_height, plan, false, instance.has_mask());
    mapped.output_box_xyxy = support.box_xyxy;
    mapped.mask_bounds = support.mask_bounds;
    mapped.output_area = support.area_pixels;
    mapped.visible = support.present;
    return mapped;
}
void build_augmentation_preview_annotations(std::span<const mmltk::backend::data::PackedInstance> source_instances,
                                            const mmltk::backend::data::PackedInstance* donor_instance, const AugmentationImagePlan* plan, int image_width,
                                            int image_height, std::vector<AugmentationPreviewAnnotation>& output,
                                            std::span<const mmltk::backend::data::RLEPair> source_runs = {});
}  // namespace mmltk::backend::models::rfdetr
