#pragma once

#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/augmentation/augmentation_plan.h"
#include <algorithm>
#include <array>

namespace mmltk::backend::models::rfdetr::test_support {

[[nodiscard]] inline GpuAugmentationConfig isolated_augmentation_config(const float copy_paste_probability = 0.0F) {
    return {
        .enabled = true,
        .geometry = {},
        .resize = {},
        .color = {},
        .noise = {},
        .blur = {},
        .occlusion = {},
        .copy_paste_probability = copy_paste_probability,
    };
}

[[nodiscard]] inline GpuAugmentationConfig spatial_occlusion_config(const bool geometry = false) {
    auto config = isolated_augmentation_config();
    config.occlusion = {.probability = 1.0F, .min_strength = 1.0F, .max_strength = 1.0F};
    if (geometry) { config.geometry = {.probability = 1.0F, .min_strength = 1.0F, .max_strength = 1.0F}; }
    return config;
}

[[nodiscard]] inline AugmentationImagePlan small_object_plan(int transform) {
    AugmentationImagePlan plan;
    if (transform == 1) {
        plan.forward = {-1, 0, 1, 0, -1, 1};
        plan.inverse = plan.forward;
    } else if (transform == 2) {
        plan.forward = {2, 0, -0.5F, 0, 2, -0.5F};
        plan.inverse = {0.5F, 0, 0.25F, 0, 0.5F, 0.25F};
    } else if (transform == 3) {
        plan.erasure = {.x0 = 0.5F, .y0 = 0, .x1 = 1, .y1 = 1, .rectangular = 1};
    }
    return plan;
}

// Independent integer-cell oracle for the explicit 8-by-4 pixel fixture.
[[nodiscard]] inline std::array<int, 4> small_object_edges(std::array<int, 4> edges, int transform) {
    if (transform == 1) edges = {8 - edges[2], 4 - edges[3], 8 - edges[0], 4 - edges[1]};
    if (transform == 2)
        edges = {std::clamp(2 * edges[0] - 4, 0, 8), std::clamp(2 * edges[1] - 2, 0, 4), std::clamp(2 * edges[2] - 4, 0, 8),
                 std::clamp(2 * edges[3] - 2, 0, 4)};
    if (transform == 3) edges[2] = std::min(edges[2], 4);
    return edges;
}

}  // namespace mmltk::backend::models::rfdetr::test_support
