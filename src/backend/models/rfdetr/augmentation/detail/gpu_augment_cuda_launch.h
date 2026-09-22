#pragma once
#include <cstdint>
#include "src/backend/models/rfdetr/augmentation/gpu_augment_cuda.h"
namespace mmltk::backend::models::rfdetr {
struct GpuAugmentationGroupLaunchConfig {
 float probability = 0.0F;
 float min_strength = 0.0F;
 float max_strength = 0.0F;
};
struct GpuAugmentationLaunchConfig {
 int enabled = 0;
 GpuAugmentationGroupLaunchConfig geometry;
 GpuAugmentationGroupLaunchConfig resize;
 GpuAugmentationGroupLaunchConfig color;
 GpuAugmentationGroupLaunchConfig noise;
 GpuAugmentationGroupLaunchConfig blur;
 GpuAugmentationGroupLaunchConfig occlusion;
};
// Physical pixel layout is independent of the original normalized annotation geometry.
struct GpuAugmentationPreparedView {
 const float* pixels = nullptr;
 int width = 0, height = 0;
 std::int64_t row_stride = 0, plane_stride = 0;
};
inline constexpr std::int64_t kGpuAugmentationParameterCount = 40;
inline constexpr std::int64_t kGpuCopyPasteParameterCount = 8;
void launch_gpu_rgba8_to_planar_float(const std::uint8_t* input, float* output, std::int64_t batch_size, int height, int width, cudaStream_t stream);
void launch_gpu_batch_normalization(
 const float* input, void* output, std::int64_t active_batch_size, std::int64_t output_batch_size, int height, int width, GpuPreprocessOutputType output_type, cudaStream_t stream);
void launch_gpu_augmentation_images(const float* input, float* output, const float* parameters, const float* copy_paste_parameters, const float* donor_images, const std::int64_t* donor_masks,
 const float* donor_boxes, std::int64_t donor_mask_words, std::int64_t batch_size, int height, int width, const GpuAugmentationLaunchConfig& config, std::uint64_t seed, int epoch, int rank,
 std::uint64_t sequence, bool remap, GpuAugmentationOutputDomain output_domain, cudaStream_t stream, const float* const* input_slots = nullptr, const float* const* donor_slots = nullptr,
 const GpuAugmentationPreparedView* prepared = nullptr);
void launch_gpu_augmentation_images_explicit(const float* input, float* output, const float* parameters, const float* copy_paste_parameters, const float* donor_images, const std::int64_t* donor_masks,
 const float* donor_boxes, std::int64_t donor_mask_words, const std::uint64_t* image_keys, std::int64_t batch_size, int height, int width, const GpuAugmentationLaunchConfig& config, bool remap,
 GpuAugmentationOutputDomain output_domain, cudaStream_t stream, const float* const* input_slots = nullptr, const float* const* donor_slots = nullptr,
 const GpuAugmentationPreparedView* prepared = nullptr);
}  // namespace mmltk::backend::models::rfdetr
