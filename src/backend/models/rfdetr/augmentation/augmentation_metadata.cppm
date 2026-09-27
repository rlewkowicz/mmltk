module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
export module mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
export namespace mmltk::backend::models::rfdetr {
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
void build_augmentation_preview_annotations(std::span<const mmltk::backend::data::PackedInstance> source_instances, const mmltk::backend::data::PackedInstance* donor_instance,
 const AugmentationImagePlan* plan, int image_width, int image_height, std::vector<AugmentationPreviewAnnotation>& output, std::span<const mmltk::backend::data::RLEPair> source_runs = {});
}  // namespace mmltk::backend::models::rfdetr
