#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace mmltk::rfdetr {

namespace ms_deform_attn_launch {

struct LaunchPlan {
    int batch = 0;
    int im2col_step = 0;
    int chunk_count = 0;
    int batch_n = 0;
    int spatial_size = 0;
    int num_heads = 0;
    int channels = 0;
    int num_levels = 0;
    int num_query = 0;
    int num_point = 0;
    std::int64_t value_stride = 0;
    std::int64_t sampling_loc_stride = 0;
    std::int64_t attn_weight_stride = 0;
    std::int64_t output_stride = 0;
};

[[nodiscard]] inline LaunchPlan make_launch_plan(const int batch, const int spatial_size, const int num_heads,
                                                 const int channels, const int num_levels, const int num_query,
                                                 const int num_point, const int im2col_step) {
    return LaunchPlan{
        batch,
        im2col_step,
        batch / im2col_step,
        im2col_step,
        spatial_size,
        num_heads,
        channels,
        num_levels,
        num_query,
        num_point,
        static_cast<std::int64_t>(spatial_size) * num_heads * channels,
        static_cast<std::int64_t>(num_query) * num_heads * num_levels * num_point * 2,
        static_cast<std::int64_t>(num_query) * num_heads * num_levels * num_point,
        static_cast<std::int64_t>(num_query) * num_heads * channels,
    };
}

template <typename Fn>
inline void for_each_batch(const LaunchPlan& plan, Fn&& fn) {
    for (int chunk_index = 0; chunk_index < plan.chunk_count; ++chunk_index) {
        fn(chunk_index);
    }
}

[[nodiscard]] inline std::int64_t batch_offset(const LaunchPlan& plan, const int batch_index,
                                               const std::int64_t stride) {
    return static_cast<std::int64_t>(batch_index) * plan.im2col_step * stride;
}

struct ForwardBatchArgs {
    const float* value = nullptr;
    const int64_t* spatial_shapes = nullptr;
    const int64_t* level_start_index = nullptr;
    const float* sampling_loc = nullptr;
    const float* attn_weight = nullptr;
    float* output = nullptr;
};

[[nodiscard]] inline ForwardBatchArgs make_forward_batch_args(const float* value, const int64_t* spatial_shapes,
                                                              const int64_t* level_start_index,
                                                              const float* sampling_loc, const float* attn_weight,
                                                              float* output, const LaunchPlan& plan,
                                                              const int batch_index) {
    return ForwardBatchArgs{
        value + batch_offset(plan, batch_index, plan.value_stride),
        spatial_shapes,
        level_start_index,
        sampling_loc + batch_offset(plan, batch_index, plan.sampling_loc_stride),
        attn_weight + batch_offset(plan, batch_index, plan.attn_weight_stride),
        output + batch_offset(plan, batch_index, plan.output_stride),
    };
}

struct BackwardBatchArgs {
    const float* value = nullptr;
    const int64_t* spatial_shapes = nullptr;
    const int64_t* level_start_index = nullptr;
    const float* sampling_loc = nullptr;
    const float* attn_weight = nullptr;
    const float* grad_output = nullptr;
    float* grad_value = nullptr;
    float* grad_sampling_loc = nullptr;
    float* grad_attn_weight = nullptr;
};

[[nodiscard]] inline BackwardBatchArgs make_backward_batch_args(const float* value, const int64_t* spatial_shapes,
                                                                const int64_t* level_start_index,
                                                                const float* sampling_loc, const float* attn_weight,
                                                                const float* grad_output, float* grad_value,
                                                                float* grad_sampling_loc, float* grad_attn_weight,
                                                                const LaunchPlan& plan, const int batch_index) {
    return BackwardBatchArgs{
        value + batch_offset(plan, batch_index, plan.value_stride),
        spatial_shapes,
        level_start_index,
        sampling_loc + batch_offset(plan, batch_index, plan.sampling_loc_stride),
        attn_weight + batch_offset(plan, batch_index, plan.attn_weight_stride),
        grad_output + batch_offset(plan, batch_index, plan.output_stride),
        grad_value + batch_offset(plan, batch_index, plan.value_stride),
        grad_sampling_loc + batch_offset(plan, batch_index, plan.sampling_loc_stride),
        grad_attn_weight + batch_offset(plan, batch_index, plan.attn_weight_stride),
    };
}

struct CommonLaunch {
    const float* value = nullptr;
    const int64_t* spatial_shapes = nullptr;
    const int64_t* level_start_index = nullptr;
    const float* sampling_loc = nullptr;
    const float* attn_weight = nullptr;
    int batch = 0;
    int spatial_size = 0;
    int num_heads = 0;
    int channels = 0;
    int num_levels = 0;
    int num_query = 0;
    int num_point = 0;
    int im2col_step = 0;
    cudaStream_t stream = nullptr;
};

[[nodiscard]] inline CommonLaunch make_common_launch(const float* value, const int64_t* spatial_shapes,
                                                     const int64_t* level_start_index, const float* sampling_loc,
                                                     const float* attn_weight, const int batch, const int spatial_size,
                                                     const int num_heads, const int channels, const int num_levels,
                                                     const int num_query, const int num_point, const int im2col_step,
                                                     cudaStream_t stream) {
    return CommonLaunch{
        value,     spatial_shapes, level_start_index, sampling_loc, attn_weight, batch,       spatial_size,
        num_heads, channels,       num_levels,        num_query,    num_point,   im2col_step, stream,
    };
}

struct ForwardLaunch : CommonLaunch {
    float* output = nullptr;
};

struct BackwardLaunch : CommonLaunch {
    const float* grad_output = nullptr;
    float* grad_value = nullptr;
    float* grad_sampling_loc = nullptr;
    float* grad_attn_weight = nullptr;
};

}  

void launch_ms_deform_attn_cuda_forward(const ms_deform_attn_launch::ForwardLaunch& launch);
void launch_ms_deform_attn_cuda_backward(const ms_deform_attn_launch::BackwardLaunch& launch);

}  
