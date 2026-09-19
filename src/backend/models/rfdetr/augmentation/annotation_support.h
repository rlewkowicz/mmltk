#pragma once
#include <algorithm>
#include <array>
#include <span>
#include "augmentation_plan.h"
namespace mmltk::backend::models::rfdetr {
struct AugmentationAnnotationSupport {
    std::array<float, 4> box_xyxy{};
    float area_pixels = 0;
    bool present = false;
};
[[nodiscard]] bool augmentation_changes_support(const AugmentationImagePlan* plan) noexcept;
[[nodiscard]] AugmentationAnnotationSupport resolve_augmentation_annotation_support(const std::array<float, 4>& source_box,
                                                                                    std::span<const mmltk::backend::data::RLEPair> source_mask, int width,
                                                                                    int height, const AugmentationImagePlan* plan, bool donor = false,
                                                                                    bool mask_present = false);
[[nodiscard]] inline std::array<float, 4> transform_augmentation_box_xyxy(const std::array<float, 4>& box,
                                                                          const std::array<float, kAugmentationTransformSize>& transform) noexcept {
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    for (int corner = 0; corner < 4; ++corner) {
        const float x = (corner & 1) != 0 ? box[2] : box[0];
        const float y = (corner & 2) != 0 ? box[3] : box[1];
        const float transformed_x = transform[0] * x + transform[1] * y + transform[2];
        const float transformed_y = transform[3] * x + transform[4] * y + transform[5];
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
[[nodiscard]] inline float augmentation_box_area(const std::array<float, 4>& box) noexcept {
    return std::max(0.0F, box[2] - box[0]) * std::max(0.0F, box[3] - box[1]);
}
}  // namespace mmltk::backend::models::rfdetr
