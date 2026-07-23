#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace mmltk::rfdetr {

enum class GpuPreprocessOutputType : std::uint8_t {
    Float32,
    Float16,
    BFloat16,
};

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

inline constexpr std::int64_t kGpuAugmentationParameterCount = 40;
inline constexpr std::int64_t kGpuCopyPasteParameterCount = 8;

void launch_gpu_batch_normalization(const float* input, void* output, std::int64_t active_batch_size,
                                    std::int64_t output_batch_size, int height, int width,
                                    GpuPreprocessOutputType output_type, cudaStream_t stream);

void launch_gpu_augmentation_parameters(float* parameters, std::int64_t batch_size,
                                        const GpuAugmentationLaunchConfig& config, std::uint64_t seed, int epoch,
                                        int rank, std::uint64_t sequence, cudaStream_t stream);

void launch_gpu_augmentation_images(const float* input, float* output, const float* parameters,
                                    const float* copy_paste_parameters, const float* donor_images,
                                    const std::int64_t* donor_masks, const float* donor_boxes,
                                    std::int64_t donor_mask_words, std::int64_t batch_size, int height, int width,
                                    const GpuAugmentationLaunchConfig& config, std::uint64_t seed, int epoch, int rank,
                                    std::uint64_t sequence, bool remap, cudaStream_t stream);

void launch_gpu_donor_cache_update(const float* source_images, float* donor_images, const std::int64_t* source_masks,
                                   std::int64_t* donor_masks, const std::int64_t* replacement_target_indices,
                                   std::int64_t batch_size, std::int64_t pixels_per_image, std::int64_t mask_words,
                                   cudaStream_t stream);

void launch_gpu_copy_cached_masks(const std::int64_t* donor_masks, std::int64_t* target_masks,
                                  const std::int64_t* donor_slots, const std::int64_t* target_indices,
                                  std::int64_t batch_size, std::int64_t mask_words, cudaStream_t stream);

}  
