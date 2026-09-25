#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "gpu_augment_types.h"
#include "src/backend/data/compiled_format.h"
namespace mmltk::backend::models::rfdetr {
struct GpuAugmentationConfig;
inline constexpr std::size_t kAugmentationTransformSize = 6;
struct AugmentationImagePlan {
 AugmentationSpatialErasure erasure;
 std::array<float, kAugmentationTransformSize> forward{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
 std::array<float, kAugmentationTransformSize> inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
 float area_scale = 1.0F;
 float resize_scale = 1.0F;
 float resize_offset_x = 0.0F;
 float resize_offset_y = 0.0F;
 float cache_choice = 0.0F;
 std::int64_t cache_source_ordinal = -1;
 std::int64_t cache_source_label = -1;
 std::uint32_t cache_source_dataset_index = 0;
 float cache_source_area = 0.0F;
 std::array<float, 4> cache_source_box{};
 // Borrowed original host support, retained by the adapter until target preparation finishes.
 const mmltk::backend::data::RLEPair* paste_support = nullptr;
 std::size_t paste_support_count = 0;
 bool paste_masked = false;
 std::int64_t paste_donor_slot = -1;
 std::int64_t paste_label = -1;
 std::uint64_t paste_sampling_identity = 0;
 float paste_source_area = 0.0F;
 std::array<float, 4> paste_source_box{};
 std::array<float, 4> paste_output_box{};
 std::array<float, kAugmentationTransformSize> paste_inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
 constexpr bool operator==(const AugmentationImagePlan&) const noexcept = default;
};
struct GpuAugmentationDonor {
 std::int64_t label = -1;
 std::uint32_t dataset_index = 0U;
 float area = 0.0F;
 std::array<float, 4> box{};
 bool has_mask = false;
 std::uint64_t sampling_identity = 0;
};
[[nodiscard]] bool augmentation_paste_admitted(const GpuAugmentationConfig& config, std::uint64_t key) noexcept;
// The same pure semantic plan feeds logical donor history and physical launches.
[[nodiscard]] AugmentationImagePlan plan_augmentation_image(const GpuAugmentationConfig&, std::uint64_t key, std::uint32_t dataset_index,
 const GpuAugmentationDonor* selected, std::int64_t donor_slot);
struct AugmentationBatchPlan {
 std::vector<AugmentationImagePlan> images;
 std::size_t active_size = 0;
 bool transforms_geometry = false;
 bool erases_spatial_support = false;
 bool copy_paste_enabled = false;
};
}  // namespace mmltk::backend::models::rfdetr
