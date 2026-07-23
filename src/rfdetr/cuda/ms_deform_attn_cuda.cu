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

template <typename BatchArgs, typename LaunchFn>
void launch_with_common_ms_deform_args(const cudaStream_t stream, const BatchArgs& args,
                                       const ms_deform_attn_launch::LaunchPlan& plan, LaunchFn&& launch_fn) {
    launch_fn(stream, args.value, args.spatial_shapes, args.level_start_index, args.sampling_loc, args.attn_weight,
              plan.batch_n, plan.spatial_size, plan.num_heads, plan.channels, plan.num_levels, plan.num_query,
              plan.num_point);
}

template <typename Launch>
[[nodiscard]] ms_deform_attn_launch::LaunchPlan make_plan_for_launch(const Launch& launch) {
    return ms_deform_attn_launch::make_launch_plan(launch.batch, launch.spatial_size, launch.num_heads, launch.channels,
                                                   launch.num_levels, launch.num_query, launch.num_point,
                                                   launch.im2col_step);
}

template <typename Launch, typename MakeBatchArgsFn, typename LaunchBatchFn>
void launch_ms_deform_attn_batches(const Launch& launch, MakeBatchArgsFn&& make_batch_args,
                                   LaunchBatchFn&& launch_batch, const char* error_context) {
    const auto plan = make_plan_for_launch(launch);
    ms_deform_attn_launch::for_each_batch(plan, [&](const int n) {
        const auto args = make_batch_args(plan, n);
        launch_with_common_ms_deform_args(launch.stream, args, plan,
                                          [&](const auto... common_args) { launch_batch(args, common_args...); });
        ensure_cuda_ok(cudaGetLastError(), error_context);
    });
}

}  // namespace

void launch_ms_deform_attn_cuda_forward(const ms_deform_attn_launch::ForwardLaunch& launch) {
    launch_ms_deform_attn_batches(
        launch,
        [&](const auto& plan, const int n) {
            return ms_deform_attn_launch::make_forward_batch_args(launch.value, launch.spatial_shapes,
                                                                  launch.level_start_index, launch.sampling_loc,
                                                                  launch.attn_weight, launch.output, plan, n);
        },
        [](const auto& args, const auto... common_args) { ms_deformable_im2col_cuda(common_args..., args.output); },
        "ms_deform_attn forward launch");
}

void launch_ms_deform_attn_cuda_backward(const ms_deform_attn_launch::BackwardLaunch& launch) {
    launch_ms_deform_attn_batches(
        launch,
        [&](const auto& plan, const int n) {
            return ms_deform_attn_launch::make_backward_batch_args(
                launch.value, launch.spatial_shapes, launch.level_start_index, launch.sampling_loc, launch.attn_weight,
                launch.grad_output, launch.grad_value, launch.grad_sampling_loc, launch.grad_attn_weight, plan, n);
        },
        [](const auto& args, const auto stream, const auto value, const auto spatial_shapes,
           const auto level_start_index, const auto sampling_loc, const auto attn_weight, const auto batch_n,
           const auto spatial_size, const auto num_heads, const auto channels, const auto num_levels,
           const auto num_query, const auto num_point) {
            ms_deformable_col2im_cuda(stream, args.grad_output, value, spatial_shapes, level_start_index, sampling_loc,
                                      attn_weight, batch_n, spatial_size, num_heads, channels, num_levels, num_query,
                                      num_point, args.grad_value, args.grad_sampling_loc, args.grad_attn_weight);
        },
        "ms_deform_attn backward launch");
}

}  // namespace mmltk::rfdetr
