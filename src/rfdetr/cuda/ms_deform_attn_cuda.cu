/*!
**************************************************************************************************
* Deformable DETR
* Copyright (c) 2020 SenseTime. All Rights Reserved.
* Licensed under the Apache License, Version 2.0 [see LICENSE for details]
**************************************************************************************************
* Modified from https://github.com/chengdazhi/Deformable-Convolution-V2-PyTorch/tree/pytorch_1.0.0
**************************************************************************************************
*/

#include "rfdetr/cuda/ms_deform_im2col_cuda.cuh"
#include "rfdetr/torch_cuda_utils.h"
#include "rfdetr/ms_deform_attn_cuda_launch.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstdint>

namespace mmltk::rfdetr {

namespace {

template <typename Fn>
void for_each_ms_deform_attn_batch(const int batch, const int im2col_step, Fn&& fn) {
    const int chunk_count = batch / im2col_step;
    for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        fn(chunk_index);
    }
}

} // namespace

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
                                        cudaStream_t stream) {
    const int batch_n = im2col_step;
    const auto per_value_size = spatial_size * num_heads * channels;
    const auto per_sample_loc_size = num_query * num_heads * num_levels * num_point * 2;
    const auto per_attn_weight_size = num_query * num_heads * num_levels * num_point;
    for_each_ms_deform_attn_batch(batch, im2col_step, [&](const int n) {
        ms_deformable_im2col_cuda(
            stream,
            value + n * im2col_step * per_value_size,
            spatial_shapes,
            level_start_index,
            sampling_loc + n * im2col_step * per_sample_loc_size,
            attn_weight + n * im2col_step * per_attn_weight_size,
            batch_n,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            output + static_cast<int64_t>(n) * batch_n * num_query * num_heads * channels);
        ensure_cuda_ok(cudaGetLastError(), "ms_deform_attn forward launch");
    });
}

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
                                         cudaStream_t stream) {
    const int batch_n = im2col_step;
    const auto per_value_size = spatial_size * num_heads * channels;
    const auto per_sample_loc_size = num_query * num_heads * num_levels * num_point * 2;
    const auto per_attn_weight_size = num_query * num_heads * num_levels * num_point;
    const auto per_grad_output_size = static_cast<int64_t>(batch_n) * num_query * num_heads * channels;
    for_each_ms_deform_attn_batch(batch, im2col_step, [&](const int n) {
        ms_deformable_col2im_cuda(
            stream,
            grad_output + static_cast<int64_t>(n) * per_grad_output_size,
            value + n * im2col_step * per_value_size,
            spatial_shapes,
            level_start_index,
            sampling_loc + n * im2col_step * per_sample_loc_size,
            attn_weight + n * im2col_step * per_attn_weight_size,
            batch_n,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value + n * im2col_step * per_value_size,
            grad_sampling_loc + n * im2col_step * per_sample_loc_size,
            grad_attn_weight + n * im2col_step * per_attn_weight_size);
        ensure_cuda_ok(cudaGetLastError(), "ms_deform_attn backward launch");
    });
}

} // namespace mmltk::rfdetr
