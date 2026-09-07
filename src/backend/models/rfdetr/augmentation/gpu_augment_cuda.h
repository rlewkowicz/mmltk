#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "src/backend/models/rfdetr/augmentation/gpu_augment_types.h"

namespace mmltk::backend::models::rfdetr {

void normalize_gpu_batch(const float* input, void* output, std::int64_t active_batch_size, std::int64_t output_batch_size, int height,
                         int width, GpuPreprocessOutputType output_type, cudaStream_t stream);
void update_gpu_augmentation_donor_cache(const float* source_images, float* donor_images, const std::int64_t* source_ordinals,
                                         std::int64_t batch_size, std::int64_t pixels_per_image, cudaStream_t stream);

}  // namespace mmltk::backend::models::rfdetr
