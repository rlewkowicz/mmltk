#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace mmltk::rfdetr {

void launch_ms_deform_attn_cuda_forward(const float* value,
                                        const int64_t* spatial_shapes,
                                        const int64_t* level_start_index,
                                        const float* sampling_loc,
                                        const float* attn_weight,
                                        int batch,
                                        int spatial_size,
                                        int num_heads,
                                        int channels,
                                        int num_levels,
                                        int num_query,
                                        int num_point,
                                        int im2col_step,
                                        float* output,
                                        cudaStream_t stream);

void launch_ms_deform_attn_cuda_backward(const float* value,
                                         const int64_t* spatial_shapes,
                                         const int64_t* level_start_index,
                                         const float* sampling_loc,
                                         const float* attn_weight,
                                         const float* grad_output,
                                         int batch,
                                         int spatial_size,
                                         int num_heads,
                                         int channels,
                                         int num_levels,
                                         int num_query,
                                         int num_point,
                                         int im2col_step,
                                         float* grad_value,
                                         float* grad_sampling_loc,
                                         float* grad_attn_weight,
                                         cudaStream_t stream);

} // namespace mmltk::rfdetr
