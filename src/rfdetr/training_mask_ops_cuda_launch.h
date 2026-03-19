#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace fastloader::rfdetr {

void launch_matcher_point_sample_cuda(const float* input,
                                      const float* coords,
                                      float* output,
                                      int64_t batch_size,
                                      int64_t coord_batches,
                                      int64_t channels,
                                      int64_t height,
                                      int64_t width,
                                      int64_t point_count,
                                      bool nearest,
                                      cudaStream_t stream);

void launch_sample_packed_masks_cuda(const int64_t* packed_bits,
                                     const int64_t* mask_indices,
                                     const float* coords,
                                     float* output,
                                     int64_t mask_count,
                                     int64_t coord_batches,
                                     int64_t words_per_mask,
                                     int64_t height,
                                     int64_t width,
                                     int64_t point_count,
                                     cudaStream_t stream);

} // namespace fastloader::rfdetr
