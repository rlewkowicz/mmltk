/*!
**************************************************************************************************
* Deformable DETR
* Copyright (c) 2020 SenseTime. All Rights Reserved.
* Licensed under the Apache License, Version 2.0 [see LICENSE for details]
**************************************************************************************************
* Modified from https://github.com/chengdazhi/Deformable-Convolution-V2-PyTorch/tree/pytorch_1.0.0
**************************************************************************************************
*/

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "detail/ms_deform_attn_cuda_launch.h"
#include "detail/ms_deform_im2col_cuda.cuh"

namespace mmltk::backend::ml::layers {

namespace {

[[nodiscard]] ms_deform_attn_launch::LaunchPlan make_plan_for_launch(const ms_deform_attn_launch::CommonLaunch& launch) {
    return ms_deform_attn_launch::make_launch_plan(launch.batch, launch.spatial_size, launch.num_heads, launch.channels, launch.num_levels,
                                                   launch.num_query, launch.num_point, launch.im2col_step);
}

void require_successful_launch(const char* context) {
    if (const cudaError_t status = cudaGetLastError(); status != cudaSuccess) {
        throw std::runtime_error(std::string(context) + ": " + cudaGetErrorString(status));
    }
}

}  // namespace

void launch_ms_deform_attn_cuda_forward(const ms_deform_attn_launch::ForwardLaunch& launch) {
    const auto plan = make_plan_for_launch(launch);
    for (int chunk = 0; chunk < plan.chunk_count; ++chunk) {
        const auto args = ms_deform_attn_launch::make_forward_batch_args(launch, plan, chunk);
        ms_deformable_im2col_cuda(launch.stream, args.value, args.layout, args.sampling_loc, args.attn_weight, plan.batch_n,
                                  plan.spatial_size, plan.num_heads, plan.channels, plan.num_levels, plan.num_query, plan.num_point,
                                  args.output);
        require_successful_launch("ms_deform_attn forward launch");
    }
}

void launch_ms_deform_attn_cuda_backward(const ms_deform_attn_launch::BackwardLaunch& launch) {
    const auto plan = make_plan_for_launch(launch);
    for (int chunk = 0; chunk < plan.chunk_count; ++chunk) {
        const auto args = ms_deform_attn_launch::make_backward_batch_args(launch, plan, chunk);
        ms_deformable_col2im_cuda(launch.stream, args.grad_output, args.value, args.layout, args.sampling_loc, args.attn_weight,
                                  plan.batch_n, plan.spatial_size, plan.num_heads, plan.channels, plan.num_levels, plan.num_query,
                                  plan.num_point, args.grad_value, args.grad_sampling_loc, args.grad_attn_weight);
        require_successful_launch("ms_deform_attn backward launch");
    }
}

}  // namespace mmltk::backend::ml::layers
