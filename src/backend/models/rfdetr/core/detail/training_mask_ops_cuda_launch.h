#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "src/backend/models/rfdetr/augmentation/gpu_augment_types.h"

namespace mmltk::backend::models::rfdetr {

namespace training_mask_ops_launch {

struct LinearPointSampleShape {
    std::int64_t sample_count = 0;
    std::int64_t point_count = 0;
    std::int64_t coord_batches = 0;
    std::int64_t total = 0;
};

[[nodiscard]] inline LinearPointSampleShape make_linear_point_sample_shape(const std::int64_t sample_count,
                                                                           const std::int64_t coord_batches,
                                                                           const std::int64_t point_count) {
    return LinearPointSampleShape{sample_count, point_count, coord_batches, sample_count * point_count};
}

[[nodiscard]] __host__ __device__ __forceinline__ std::int64_t point_index(const std::int64_t index, const std::int64_t point_count) {
    return index % point_count;
}

[[nodiscard]] __host__ __device__ __forceinline__ std::int64_t sample_index(const std::int64_t index, const std::int64_t point_count) {
    return index / point_count;
}

[[nodiscard]] __host__ __device__ __forceinline__ std::int64_t coord_batch_index(const std::int64_t coord_batches,
                                                                                 const std::int64_t sample_index) {
    return coord_batches == 1 ? 0 : sample_index;
}

[[nodiscard]] __host__ __device__ __forceinline__ std::int64_t coord_offset(const std::int64_t coord_batch, const std::int64_t point_index,
                                                                            const std::int64_t point_count) {
    return (coord_batch * point_count + point_index) * 2;
}

struct MatcherPointSampleLaunch {
    const float* input = nullptr;
    const float* coords = nullptr;
    float* output = nullptr;
    int64_t batch_size = 0;
    int64_t coord_batches = 0;
    int64_t channels = 0;
    int64_t height = 0;
    int64_t width = 0;
    int64_t point_count = 0;
    bool nearest = false;
    cudaStream_t stream = nullptr;
};

struct PackedMaskSampleLaunch {
    const int64_t* packed_bits = nullptr;
    const int64_t* mask_indices = nullptr;
    const float* coords = nullptr;
    const float* inverse_transforms = nullptr;
    const int64_t* occluder_mask_indices = nullptr;
    const float* occluder_inverse_transforms = nullptr;
    const AugmentationSpatialErasure* erasure = nullptr;
    float* output = nullptr;
    int64_t mask_count = 0;
    int64_t coord_batches = 0;
    int64_t words_per_mask = 0;
    int64_t height = 0;
    int64_t width = 0;
    int64_t point_count = 0;
    int64_t transform_stride = 0;
    cudaStream_t stream = nullptr;
};

}  // namespace training_mask_ops_launch

// The launch descriptors above are this boundary's calling convention: callers name the fields they
// set instead of going through a positional overload that has to be kept in step with them.
void launch_matcher_point_sample_cuda(const training_mask_ops_launch::MatcherPointSampleLaunch& launch);
void launch_sample_packed_masks_cuda(const training_mask_ops_launch::PackedMaskSampleLaunch& launch);

}  // namespace mmltk::backend::models::rfdetr
