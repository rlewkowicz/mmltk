module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
module mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
namespace mmltk::backend::models::rfdetr {
namespace {
constexpr std::array<float, kAugmentationTransformSize> kIdentity{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
}
void build_augmentation_preview_annotations(const std::span<const mmltk::backend::data::PackedInstance> source_instances,
                                            const mmltk::backend::data::PackedInstance* donor_instance, const AugmentationImagePlan* plan,
                                            const int image_width, const int image_height, std::vector<AugmentationPreviewAnnotation>& output,
                                            std::span<const mmltk::backend::data::RLEPair> source_runs) {
    output.clear();
    const bool has_paste = plan != nullptr && plan->paste_donor_slot >= 0;
    if (has_paste && donor_instance == nullptr) throw std::invalid_argument("augmentation preview paste has no donor instance");
    const std::size_t required = source_instances.size() + (has_paste ? 1U : 0U);
    if (output.capacity() < required) { output.reserve(required); }
    for (std::size_t ordinal = 0U; ordinal != source_instances.size(); ++ordinal) {
        const auto& instance = source_instances[ordinal];
        const auto offset = instance.mask_rle_pairs != 0 ? instance.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair) : 0;
        if (offset > source_runs.size() || instance.mask_rle_pairs > source_runs.size() - offset)
            throw std::invalid_argument("augmentation source mask exceeds host run storage");
        const auto mapped = map_augmentation_instance(instance, image_width, image_height, plan, source_runs.subspan(offset, instance.mask_rle_pairs));
        if (!mapped.visible) continue;
        output.push_back(AugmentationPreviewAnnotation{
            .source_ordinal = ordinal,
            .visible_area_pixels = mapped.output_area,
            .erasure = plan != nullptr ? plan->erasure : AugmentationSpatialErasure{},
            .box_xyxy = mapped.output_box_xyxy,
            .mask_bounds = mapped.mask_bounds,
            .mask_present = instance.has_mask(),
            .inverse = plan != nullptr ? plan->inverse : kIdentity,
            .occluder_inverse = has_paste ? plan->paste_inverse : kIdentity,
            .mask_rle_offset = instance.mask_rle_offset,
            .mask_rle_pairs = instance.mask_rle_pairs,
            .class_id = instance.class_id,
        });
    }
    if (!has_paste) { return; }
    const auto support = resolve_augmentation_annotation_support(plan->paste_source_box, std::span{plan->paste_support, plan->paste_support_count}, image_width,
                                                                 image_height, plan, true);
    if (!support.present) return;
    const std::int32_t occluder_index = static_cast<std::int32_t>(output.size());
    for (AugmentationPreviewAnnotation& annotation : output) { annotation.occluder_index = occluder_index; }
    output.push_back(AugmentationPreviewAnnotation{
        .source_ordinal = source_instances.size(),
        .visible_area_pixels = support.area_pixels,
        .erasure = plan->erasure,
        .box_xyxy = support.box_xyxy,
        .mask_bounds = support.mask_bounds,
        .mask_present = plan->paste_masked && donor_instance->has_mask(),
        .inverse = plan->paste_inverse,
        .mask_rle_offset = donor_instance->mask_rle_offset,
        .mask_rle_pairs = donor_instance->mask_rle_pairs,
        .class_id = donor_instance->class_id,
    });
}
}  // namespace mmltk::backend::models::rfdetr
