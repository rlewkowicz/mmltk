#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/training_mask_ops_cuda_launch.h"

#include <cuda_runtime.h>

namespace mmltk::rfdetr {

namespace {

constexpr int kCudaThreads = 256;

int ceil_div(int64_t value, int divisor) {
    return static_cast<int>((value + divisor - 1) / divisor);
}

__device__ float clamp_coord(float value, float limit) {
    return fminf(fmaxf(value, 0.0f), limit);
}

__device__ int64_t clamp_index(int64_t value, int64_t limit) {
    if (value < 0) {
        return 0;
    }
    if (value >= limit) {
        return limit - 1;
    }
    return value;
}

__device__ int64_t nearest_grid_sample_index(float coord, int64_t size) {
    return clamp_index(static_cast<int64_t>(nearbyintf(coord * static_cast<float>(size) - 0.5f)), size);
}

__global__ void matcher_point_sample_kernel(const float* input, const float* coords, float* output, int64_t batch_size,
                                            int64_t coord_batches, int64_t channels, int64_t height, int64_t width,
                                            int64_t point_count, bool nearest) {
    const int64_t index = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const int64_t total = batch_size * channels * point_count;
    if (index >= total) {
        return;
    }

    const int64_t point_index = training_mask_ops_launch::point_index(index, point_count);
    const int64_t channel_index = (index / point_count) % channels;
    const int64_t batch_index = index / (channels * point_count);
    const int64_t coord_batch = training_mask_ops_launch::coord_batch_index(coord_batches, batch_index);
    const int64_t coord_offset = training_mask_ops_launch::coord_offset(coord_batch, point_index, point_count);
    const float coord_x = coords[coord_offset];
    const float coord_y = coords[coord_offset + 1];
    const int64_t input_base = (batch_index * channels + channel_index) * height * width;

    if (nearest) {
        const int64_t x = nearest_grid_sample_index(coord_x, width);
        const int64_t y = nearest_grid_sample_index(coord_y, height);
        output[index] = input[input_base + y * width + x];
        return;
    }

    const float x = clamp_coord(coord_x * static_cast<float>(width) - 0.5f, static_cast<float>(width - 1));
    const float y = clamp_coord(coord_y * static_cast<float>(height) - 0.5f, static_cast<float>(height - 1));
    const int64_t x0 = static_cast<int64_t>(floorf(x));
    const int64_t y0 = static_cast<int64_t>(floorf(y));
    const int64_t x1 = x0 + 1 < width ? x0 + 1 : width - 1;
    const int64_t y1 = y0 + 1 < height ? y0 + 1 : height - 1;
    const float wx = x - static_cast<float>(x0);
    const float wy = y - static_cast<float>(y0);

    const float v00 = input[input_base + y0 * width + x0];
    const float v01 = input[input_base + y0 * width + x1];
    const float v10 = input[input_base + y1 * width + x0];
    const float v11 = input[input_base + y1 * width + x1];
    output[index] = (1.0f - wx) * (1.0f - wy) * v00 + wx * (1.0f - wy) * v01 + (1.0f - wx) * wy * v10 + wx * wy * v11;
}

__global__ void sample_packed_masks_kernel(const int64_t* packed_bits, const int64_t* mask_indices, const float* coords,
                                           float* output, int64_t mask_count, int64_t coord_batches,
                                           int64_t words_per_mask, int64_t height, int64_t width, int64_t point_count) {
    const int64_t index = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const int64_t total = mask_count * point_count;
    if (index >= total) {
        return;
    }

    const int64_t point_index = training_mask_ops_launch::point_index(index, point_count);
    const int64_t mask_slot = training_mask_ops_launch::sample_index(index, point_count);
    const int64_t coord_batch = training_mask_ops_launch::coord_batch_index(coord_batches, mask_slot);
    const int64_t coord_offset = training_mask_ops_launch::coord_offset(coord_batch, point_index, point_count);
    const float coord_x = coords[coord_offset];
    const float coord_y = coords[coord_offset + 1];
    const int64_t x = nearest_grid_sample_index(coord_x, width);
    const int64_t y = nearest_grid_sample_index(coord_y, height);
    const int64_t pixel_index = y * width + x;
    const int64_t word_index = pixel_index / 64;
    const int64_t bit_index = pixel_index % 64;
    const int64_t mask_index = mask_indices[mask_slot];
    const auto* words = reinterpret_cast<const unsigned long long*>(packed_bits + mask_index * words_per_mask);
    const unsigned long long word = words[word_index];
    output[index] = static_cast<float>((word >> bit_index) & 1ULL);
}

}  // namespace

void launch_matcher_point_sample_cuda(const training_mask_ops_launch::MatcherPointSampleLaunch& launch) {
    const auto shape = training_mask_ops_launch::make_linear_point_sample_shape(
        launch.batch_size * launch.channels, launch.coord_batches, launch.point_count);
    matcher_point_sample_kernel<<<ceil_div(shape.total, kCudaThreads), kCudaThreads, 0, launch.stream>>>(
        launch.input, launch.coords, launch.output, launch.batch_size, launch.coord_batches, launch.channels,
        launch.height, launch.width, launch.point_count, launch.nearest);
    ensure_cuda_ok(cudaGetLastError(), "matcher_point_sample_cuda_forward launch");
}

void launch_sample_packed_masks_cuda(const training_mask_ops_launch::PackedMaskSampleLaunch& launch) {
    const auto shape = training_mask_ops_launch::make_linear_point_sample_shape(launch.mask_count, launch.coord_batches,
                                                                                launch.point_count);
    sample_packed_masks_kernel<<<ceil_div(shape.total, kCudaThreads), kCudaThreads, 0, launch.stream>>>(
        launch.packed_bits, launch.mask_indices, launch.coords, launch.output, launch.mask_count, launch.coord_batches,
        launch.words_per_mask, launch.height, launch.width, launch.point_count);
    ensure_cuda_ok(cudaGetLastError(), "sample_packed_masks_cuda launch");
}

}  // namespace mmltk::rfdetr
