#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/math/deterministic_sampling.h"
#include <span>
#include "augmentation_plan.h"
#include "src/backend/imaging/sampling.h"
namespace mmltk::backend::models::rfdetr {
struct AugmentationAnnotationSupport {
 std::array<float, 4> box_xyxy{};
 float area_pixels = 0;
 bool present = false;
 std::array<float, 4> mask_bounds{};
};
[[nodiscard]] bool augmentation_changes_support(const AugmentationImagePlan* plan) noexcept;
[[nodiscard]] AugmentationAnnotationSupport resolve_augmentation_annotation_support(const std::array<float, 4>& source_box, std::span<const mmltk::backend::data::RLEPair> source_mask, int width,
 int height, const AugmentationImagePlan* plan, bool donor = false, bool mask_present = false);
[[nodiscard]] inline std::array<float, 4> transform_augmentation_box_xyxy(const std::array<float, 4>& box, const std::array<float, kAugmentationTransformSize>& transform) noexcept {
 float minimum_x = 1.0F;
 float minimum_y = 1.0F;
 float maximum_x = 0.0F;
 float maximum_y = 0.0F;
 for (int corner = 0; corner < 4; ++corner) {
  const float x = (corner & 1) != 0 ? box[2] : box[0];
  const float y = (corner & 2) != 0 ? box[3] : box[1];
  const auto [transformed_x, transformed_y] = mmltk::backend::imaging::sampling::affine_point(transform.data(), x, y);
  minimum_x = std::min(minimum_x, transformed_x);
  minimum_y = std::min(minimum_y, transformed_y);
  maximum_x = std::max(maximum_x, transformed_x);
  maximum_y = std::max(maximum_y, transformed_y);
 }
 return {
  std::clamp(minimum_x, 0.0F, 1.0F),
  std::clamp(minimum_y, 0.0F, 1.0F),
  std::clamp(maximum_x, 0.0F, 1.0F),
  std::clamp(maximum_y, 0.0F, 1.0F),
 };
}
[[nodiscard]] inline float augmentation_box_area(const std::array<float, 4>& box) noexcept { return std::max(0.0F, box[2] - box[0]) * std::max(0.0F, box[3] - box[1]); }
struct AugmentationMappedInstance {
 std::array<float, 4> source_box_xyxy{};
 std::array<float, 4> output_box_xyxy{};
 std::array<float, 4> mask_bounds{};
 float source_area_pixels = 0.0F;
 float output_area = 0.0F;
 bool visible = false;
};
[[nodiscard]] inline std::array<float, 4> augmentation_instance_box(const mmltk::backend::data::PackedInstance& instance, int image_width, int image_height) {
 const float inverse_width = 1.0F / static_cast<float>(image_width);
 const float inverse_height = 1.0F / static_cast<float>(image_height);
 return {
  static_cast<float>(instance.bbox_x1) * inverse_width,
  static_cast<float>(instance.bbox_y1) * inverse_height,
  static_cast<float>(instance.bbox_x2) * inverse_width,
  static_cast<float>(instance.bbox_y2) * inverse_height,
 };
}
// Uses precisely the support mapper's eligibility rules, without computing unused
// area and bounds. Pure geometry retains continuous detection-box visibility.
[[nodiscard]] bool augmentation_instance_visible(const mmltk::backend::data::PackedInstance& instance, int width, int height, const AugmentationImagePlan* plan,
 std::span<const mmltk::backend::data::RLEPair> mask = {});
[[nodiscard]] inline AugmentationMappedInstance map_augmentation_instance(
 const mmltk::backend::data::PackedInstance& instance, int image_width, int image_height, const AugmentationImagePlan* plan, std::span<const mmltk::backend::data::RLEPair> mask = {}) {
 AugmentationMappedInstance mapped;
 mapped.source_box_xyxy = augmentation_instance_box(instance, image_width, image_height);
 mapped.source_area_pixels = augmentation_box_area(mapped.source_box_xyxy) * static_cast<float>(image_width) * static_cast<float>(image_height);
 const auto support = resolve_augmentation_annotation_support(mapped.source_box_xyxy, mask, image_width, image_height, plan, false, instance.has_mask());
 mapped.output_box_xyxy = support.box_xyxy;
 mapped.mask_bounds = support.mask_bounds;
 mapped.output_area = support.area_pixels;
 mapped.visible = support.present;
 return mapped;
}
[[nodiscard]] inline bool augmentation_reservoir_select(float choice, std::int64_t candidates, std::int64_t ordinal) {
 if (candidates <= 1) return true;
 const auto key = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(choice));
 return mmltk::common::math::deterministic_mix64(key ^ (static_cast<std::uint64_t>(ordinal) * 0xd2b74407b1ce6e93ULL)) % static_cast<std::uint64_t>(candidates) == 0;
}
}  // namespace mmltk::backend::models::rfdetr
