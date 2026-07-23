#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmltk::rfdetr {

inline constexpr std::size_t kAugmentationTransformSize = 6;

struct AugmentationImagePlan {
    std::array<float, kAugmentationTransformSize> forward{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    std::array<float, kAugmentationTransformSize> inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    float area_scale = 1.0F;
    float resize_scale = 1.0F;
    float resize_offset_x = 0.0F;
    float resize_offset_y = 0.0F;

    float cache_choice = 0.0F;
    std::int64_t cache_source_target_index = -1;
    std::int64_t cache_source_label = -1;
    std::uint32_t cache_source_dataset_index = 0;
    float cache_source_area = 0.0F;
    std::array<float, 4> cache_source_box{};

    std::int64_t paste_donor_slot = -1;
    std::int64_t paste_target_index = -1;
    std::int64_t paste_label = -1;
    float paste_source_area = 0.0F;
    std::array<float, 4> paste_source_box{};
    std::array<float, 4> paste_output_box{};
    std::array<float, kAugmentationTransformSize> paste_inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
};

struct AugmentationBatchPlan {
    std::vector<AugmentationImagePlan> images;
    std::size_t active_size = 0;
    bool transforms_geometry = false;
    bool copy_paste_enabled = false;
    bool include_masks = false;
};

}  
