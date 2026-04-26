/*!
**************************************************************************
* Deformable DETR
* Copyright (c) 2020 SenseTime. All Rights Reserved.
* Licensed under the Apache License, Version 2.0 [see LICENSE for details]
**************************************************************************
* Modified from DCN (https://github.com/msracver/Deformable-ConvNets)
* Copyright (c) 2018 Microsoft
**************************************************************************
*/

#include <cstdio>
#include <algorithm>
#include <cstring>

#include <ATen/cuda/Atomic.cuh>

#define CUDA_KERNEL_LOOP(i, n) for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < (n); i += blockDim.x * gridDim.x)

const int CUDA_NUM_THREADS = 1024;
inline int GET_BLOCKS(const int N, const int num_threads) {
    return (N + num_threads - 1) / num_threads;
}

template <typename scalar_t>
struct BilinearSampleGeometry {
    int height = 0;
    int width = 0;
    int h_low = 0;
    int w_low = 0;
    int h_high = 0;
    int w_high = 0;
    int h_low_ptr_offset = 0;
    int h_high_ptr_offset = 0;
    int w_low_ptr_offset = 0;
    int w_high_ptr_offset = 0;
    int base_ptr = 0;
    scalar_t lh = 0;
    scalar_t lw = 0;
    scalar_t hh = 0;
    scalar_t hw = 0;
    scalar_t w1 = 0;
    scalar_t w2 = 0;
    scalar_t w3 = 0;
    scalar_t w4 = 0;
};

template <typename scalar_t>
__device__ BilinearSampleGeometry<scalar_t> make_bilinear_sample_geometry(const int height, const int width,
                                                                          const int nheads, const int channels,
                                                                          const scalar_t h, const scalar_t w,
                                                                          const int m, const int c) {
    BilinearSampleGeometry<scalar_t> geometry;
    geometry.height = height;
    geometry.width = width;
    geometry.h_low = floor(h);
    geometry.w_low = floor(w);
    geometry.h_high = geometry.h_low + 1;
    geometry.w_high = geometry.w_low + 1;
    geometry.lh = h - geometry.h_low;
    geometry.lw = w - geometry.w_low;
    geometry.hh = 1 - geometry.lh;
    geometry.hw = 1 - geometry.lw;

    const int w_stride = nheads * channels;
    const int h_stride = width * w_stride;
    geometry.h_low_ptr_offset = geometry.h_low * h_stride;
    geometry.h_high_ptr_offset = geometry.h_low_ptr_offset + h_stride;
    geometry.w_low_ptr_offset = geometry.w_low * w_stride;
    geometry.w_high_ptr_offset = geometry.w_low_ptr_offset + w_stride;
    geometry.base_ptr = m * channels + c;
    geometry.w1 = geometry.hh * geometry.hw;
    geometry.w2 = geometry.hh * geometry.lw;
    geometry.w3 = geometry.lh * geometry.hw;
    geometry.w4 = geometry.lh * geometry.lw;
    return geometry;
}

template <typename scalar_t, typename CornerPolicy>
__device__ scalar_t ms_deform_attn_apply_bilinear_corners(const scalar_t* bottom_data,
                                                          const BilinearSampleGeometry<scalar_t>& geometry,
                                                          CornerPolicy&& policy) {
    scalar_t value = 0;
    if (geometry.h_low >= 0 && geometry.w_low >= 0) {
        const int ptr = geometry.h_low_ptr_offset + geometry.w_low_ptr_offset + geometry.base_ptr;
        const scalar_t corner_value = bottom_data[ptr];
        value += geometry.w1 * corner_value;
        policy.corner(0, ptr, geometry.w1, corner_value, geometry);
    }
    if (geometry.h_low >= 0 && geometry.w_high <= geometry.width - 1) {
        const int ptr = geometry.h_low_ptr_offset + geometry.w_high_ptr_offset + geometry.base_ptr;
        const scalar_t corner_value = bottom_data[ptr];
        value += geometry.w2 * corner_value;
        policy.corner(1, ptr, geometry.w2, corner_value, geometry);
    }
    if (geometry.h_high <= geometry.height - 1 && geometry.w_low >= 0) {
        const int ptr = geometry.h_high_ptr_offset + geometry.w_low_ptr_offset + geometry.base_ptr;
        const scalar_t corner_value = bottom_data[ptr];
        value += geometry.w3 * corner_value;
        policy.corner(2, ptr, geometry.w3, corner_value, geometry);
    }
    if (geometry.h_high <= geometry.height - 1 && geometry.w_high <= geometry.width - 1) {
        const int ptr = geometry.h_high_ptr_offset + geometry.w_high_ptr_offset + geometry.base_ptr;
        const scalar_t corner_value = bottom_data[ptr];
        value += geometry.w4 * corner_value;
        policy.corner(3, ptr, geometry.w4, corner_value, geometry);
    }
    return policy.finish(value);
}

template <typename scalar_t>
struct BilinearForwardPolicy {
    __device__ void corner(int, int, scalar_t, scalar_t, const BilinearSampleGeometry<scalar_t>&) const {}

    __device__ scalar_t finish(const scalar_t value) const {
        return value;
    }
};

template <typename scalar_t, typename AccumulationPolicy>
struct BilinearBackwardPolicy {
    BilinearSampleGeometry<scalar_t> geometry{};
    scalar_t* grad_value = nullptr;
    scalar_t* grad_sampling_loc = nullptr;
    scalar_t* grad_attn_weight = nullptr;
    scalar_t top_grad = 0;
    scalar_t top_grad_value = 0;
    scalar_t grad_h_weight = 0;
    scalar_t grad_w_weight = 0;
    AccumulationPolicy accumulation_policy{};

    __device__ void corner(int corner_index, int ptr, scalar_t weight, scalar_t corner_value,
                           const BilinearSampleGeometry<scalar_t>&) {
        switch (corner_index) {
            case 0:
                grad_h_weight -= geometry.hw * corner_value;
                grad_w_weight -= geometry.hh * corner_value;
                break;
            case 1:
                grad_h_weight -= geometry.lw * corner_value;
                grad_w_weight += geometry.hh * corner_value;
                break;
            case 2:
                grad_h_weight += geometry.hw * corner_value;
                grad_w_weight -= geometry.lh * corner_value;
                break;
            default:
                grad_h_weight += geometry.lw * corner_value;
                grad_w_weight += geometry.lh * corner_value;
                break;
        }
        gpuAtomicAdd(grad_value + ptr, weight * top_grad_value);
    }

    __device__ scalar_t finish(const scalar_t value) {
        accumulation_policy.template store<scalar_t>(
            grad_sampling_loc, grad_attn_weight,
            {grad_h_weight, grad_w_weight, top_grad, top_grad_value, value, geometry.height, geometry.width});
        return value;
    }
};

template <typename ValueT>
struct BilinearDirectStoreOp {
    __device__ void operator()(ValueT* destination, const ValueT value) const {
        *destination = value;
    }
};

template <typename ValueT>
struct BilinearAtomicStoreOp {
    __device__ void operator()(ValueT* destination, const ValueT value) const {
        gpuAtomicAdd(destination, value);
    }
};

template <typename ValueT, typename StoreOp>
struct BilinearAccumulatorValues {
    ValueT grad_h_weight;
    ValueT grad_w_weight;
    ValueT top_grad;
    ValueT top_grad_value;
    ValueT value;
    int height_value;
    int width_value;
};

template <typename ValueT, typename StoreOp>
__device__ void store_bilinear_accumulator_values(const StoreOp& store_op, ValueT* grad_sampling_loc,
                                                  ValueT* grad_attn_weight,
                                                  const BilinearAccumulatorValues<ValueT, StoreOp>& values) {
    store_op(grad_attn_weight, values.top_grad * values.value);
    store_op(grad_sampling_loc, values.width_value * values.grad_w_weight * values.top_grad_value);
    store_op(grad_sampling_loc + 1, values.height_value * values.grad_h_weight * values.top_grad_value);
}

template <template <typename> class StoreOp>
struct BilinearAccumulatorPolicy {
    template <typename ValueT>
    __device__ void store(ValueT* grad_sampling_loc, ValueT* grad_attn_weight,
                          const BilinearAccumulatorValues<ValueT, StoreOp<ValueT>>& values) const {
        store_bilinear_accumulator_values(StoreOp<ValueT>{}, grad_sampling_loc, grad_attn_weight, values);
    }
};

using BilinearDirectAccumulatorPolicy = BilinearAccumulatorPolicy<BilinearDirectStoreOp>;
using BilinearAtomicAccumulatorPolicy = BilinearAccumulatorPolicy<BilinearAtomicStoreOp>;

template <typename scalar_t, typename AccumulationPolicy>
__device__ void ms_deform_attn_bilinear_backward(const scalar_t* bottom_data,
                                                 const BilinearSampleGeometry<scalar_t>& geometry,
                                                 const scalar_t top_grad, const scalar_t attn_weight,
                                                 scalar_t* grad_value, scalar_t* grad_sampling_loc,
                                                 scalar_t* grad_attn_weight,
                                                 const AccumulationPolicy& accumulation_policy) {
    const scalar_t top_grad_value = top_grad * attn_weight;
    BilinearBackwardPolicy<scalar_t, AccumulationPolicy> policy{
        geometry, grad_value, grad_sampling_loc, grad_attn_weight, top_grad, top_grad_value, 0, 0, accumulation_policy,
    };
    (void)ms_deform_attn_apply_bilinear_corners(bottom_data, geometry, policy);
}

template <typename scalar_t, typename AccumulationPolicy>
__device__ void ms_deform_attn_col2im_bilinear(const scalar_t* bottom_data, const int height, const int width,
                                               const int nheads, const int channels, const scalar_t h, const scalar_t w,
                                               const int m, const int c, const scalar_t top_grad,
                                               const scalar_t attn_weight, scalar_t* grad_value,
                                               scalar_t* grad_sampling_loc, scalar_t* grad_attn_weight,
                                               const AccumulationPolicy& accumulation_policy) {
    const BilinearSampleGeometry<scalar_t> geometry =
        make_bilinear_sample_geometry(height, width, nheads, channels, h, w, m, c);
    ms_deform_attn_bilinear_backward(bottom_data, geometry, top_grad, attn_weight, grad_value, grad_sampling_loc,
                                     grad_attn_weight, accumulation_policy);
}

template <typename scalar_t>
__device__ scalar_t ms_deform_attn_im2col_bilinear(const scalar_t*& bottom_data, const int& height, const int& width,
                                                   const int& nheads, const int& channels, const scalar_t& h,
                                                   const scalar_t& w, const int& m, const int& c) {
    const BilinearSampleGeometry<scalar_t> geometry =
        make_bilinear_sample_geometry(height, width, nheads, channels, h, w, m, c);
    return ms_deform_attn_apply_bilinear_corners(bottom_data, geometry, BilinearForwardPolicy<scalar_t>{});
}

inline void report_ms_deformable_cuda_error(const char* error_label) {
    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("error in %s: %s\n", error_label, cudaGetErrorString(err));
    }
}

template <typename LaunchFn>
inline void launch_ms_deformable_cuda_kernel(LaunchFn&& launch, const char* error_label) {
    launch();
    report_ms_deformable_cuda_error(error_label);
}

template <typename scalar_t, typename SizeT>
struct SharedLinearGradientReduction {
    SizeT thread_count = 0;

    __device__ void operator()(scalar_t* cache_grad_sampling_loc, scalar_t* cache_grad_attn_weight,
                               const unsigned int tid) const {
        if (tid != 0) {
            return;
        }

        scalar_t grad_w = cache_grad_sampling_loc[0];
        scalar_t grad_h = cache_grad_sampling_loc[1];
        scalar_t grad_a = cache_grad_attn_weight[0];
        int sid = 2;
        for (SizeT lane = 1; lane < thread_count; ++lane) {
            grad_w += cache_grad_sampling_loc[sid];
            grad_h += cache_grad_sampling_loc[sid + 1];
            grad_a += cache_grad_attn_weight[lane];
            sid += 2;
        }

        cache_grad_sampling_loc[0] = grad_w;
        cache_grad_sampling_loc[1] = grad_h;
        cache_grad_attn_weight[0] = grad_a;
    }
};

template <typename scalar_t, typename SizeT>
struct SharedTreeGradientReduction {
    SizeT thread_count = 0;

    __device__ void operator()(scalar_t* cache_grad_sampling_loc, scalar_t* cache_grad_attn_weight,
                               const unsigned int tid) const {
        for (SizeT s = thread_count / 2; s > 0; s >>= 1) {
            if (tid < s) {
                const unsigned int xid1 = tid << 1;
                const unsigned int xid2 = (tid + s) << 1;
                cache_grad_attn_weight[tid] += cache_grad_attn_weight[tid + s];
                cache_grad_sampling_loc[xid1] += cache_grad_sampling_loc[xid2];
                cache_grad_sampling_loc[xid1 + 1] += cache_grad_sampling_loc[xid2 + 1];
            }
            __syncthreads();
        }
    }
};

template <typename scalar_t, typename SizeT>
struct SharedTreeGradientReductionV2 {
    SizeT thread_count = 0;

    __device__ void operator()(scalar_t* cache_grad_sampling_loc, scalar_t* cache_grad_attn_weight,
                               const unsigned int tid) const {
        for (SizeT s = thread_count / 2, spre = thread_count; s > 0; s >>= 1, spre >>= 1) {
            if (tid < s) {
                const unsigned int xid1 = tid << 1;
                const unsigned int xid2 = (tid + s) << 1;
                cache_grad_attn_weight[tid] += cache_grad_attn_weight[tid + s];
                cache_grad_sampling_loc[xid1] += cache_grad_sampling_loc[xid2];
                cache_grad_sampling_loc[xid1 + 1] += cache_grad_sampling_loc[xid2 + 1];
                if (tid + (s << 1) < spre) {
                    cache_grad_attn_weight[tid] += cache_grad_attn_weight[tid + (s << 1)];
                    cache_grad_sampling_loc[xid1] += cache_grad_sampling_loc[xid2 + (s << 1)];
                    cache_grad_sampling_loc[xid1 + 1] += cache_grad_sampling_loc[xid2 + 1 + (s << 1)];
                }
            }
            __syncthreads();
        }
    }
};

template <typename scalar_t, typename StoreOp>
struct GradientStore {
    __device__ void operator()(scalar_t* grad_sampling_loc, scalar_t* grad_attn_weight,
                               const scalar_t* cache_grad_sampling_loc, const scalar_t* cache_grad_attn_weight) const {
        const StoreOp store_op{};
        store_op(grad_sampling_loc, cache_grad_sampling_loc[0]);
        store_op(grad_sampling_loc + 1, cache_grad_sampling_loc[1]);
        store_op(grad_attn_weight, cache_grad_attn_weight[0]);
    }
};

template <typename scalar_t>
using DirectGradientStore = GradientStore<scalar_t, BilinearDirectStoreOp<scalar_t>>;

template <typename scalar_t>
using AtomicGradientStore = GradientStore<scalar_t, BilinearAtomicStoreOp<scalar_t>>;

template <typename scalar_t>
struct MsDeformableCol2ImPointArgs {
    const scalar_t* data_value_ptr = nullptr;
    int spatial_h = 0;
    int spatial_w = 0;
    int num_heads = 0;
    int channels = 0;
    scalar_t loc_w = 0;
    scalar_t loc_h = 0;
    int m_col = 0;
    int c_col = 0;
    scalar_t top_grad = 0;
    scalar_t weight = 0;
    scalar_t* grad_value_ptr = nullptr;
    scalar_t* grad_sampling_loc = nullptr;
    scalar_t* grad_attn_weight = nullptr;
};

template <typename scalar_t, typename AccumulationPolicy>
__device__ inline void ms_deformable_col2im_apply_point(const MsDeformableCol2ImPointArgs<scalar_t>& point,
                                                        scalar_t* grad_sampling_loc, scalar_t* grad_attn_weight,
                                                        const AccumulationPolicy& accumulation_policy) {
    const scalar_t h_im = point.loc_h * point.spatial_h - 0.5;
    const scalar_t w_im = point.loc_w * point.spatial_w - 0.5;
    if (h_im > -1 && w_im > -1 && h_im < point.spatial_h && w_im < point.spatial_w) {
        ms_deform_attn_col2im_bilinear(point.data_value_ptr, point.spatial_h, point.spatial_w, point.num_heads,
                                       point.channels, h_im, w_im, point.m_col, point.c_col, point.top_grad,
                                       point.weight, point.grad_value_ptr, grad_sampling_loc, grad_attn_weight,
                                       accumulation_policy);
    }
}

template <typename scalar_t, typename ReductionFn, typename StoreFn>
__device__ inline void ms_deformable_col2im_reduce_point(const MsDeformableCol2ImPointArgs<scalar_t>& point,
                                                         scalar_t* cache_grad_sampling_loc,
                                                         scalar_t* cache_grad_attn_weight, const unsigned int tid,
                                                         const ReductionFn& reduction_fn, const StoreFn& store_fn) {
    *(cache_grad_sampling_loc + (tid << 1)) = 0;
    *(cache_grad_sampling_loc + ((tid << 1) + 1)) = 0;
    *(cache_grad_attn_weight + tid) = 0;
    ms_deformable_col2im_apply_point(point, cache_grad_sampling_loc + (tid << 1), cache_grad_attn_weight + tid,
                                     BilinearDirectAccumulatorPolicy{});

    __syncthreads();
    reduction_fn(cache_grad_sampling_loc, cache_grad_attn_weight, tid);
    __syncthreads();
    if (tid == 0) {
        store_fn(point.grad_sampling_loc, point.grad_attn_weight, cache_grad_sampling_loc, cache_grad_attn_weight);
    }
    __syncthreads();
}

template <typename scalar_t, typename ReductionFn, typename StoreFn>
struct SharedMemoryCol2ImPointReducer {
    scalar_t* cache_grad_sampling_loc = nullptr;
    scalar_t* cache_grad_attn_weight = nullptr;
    unsigned int tid = 0;
    ReductionFn reduction{};
    StoreFn store{};

    __device__ void operator()(const MsDeformableCol2ImPointArgs<scalar_t>& point) const {
        ms_deformable_col2im_reduce_point(point, cache_grad_sampling_loc, cache_grad_attn_weight, tid, reduction,
                                          store);
    }
};

template <typename scalar_t>
struct GlobalMemoryCol2ImPointReducer {
    __device__ void operator()(const MsDeformableCol2ImPointArgs<scalar_t>& point) const {
        ms_deformable_col2im_apply_point(point, point.grad_sampling_loc, point.grad_attn_weight,
                                         BilinearAtomicAccumulatorPolicy{});
    }
};

template <typename scalar_t, typename PointReducer>
__device__ inline void ms_deformable_col2im_kernel_body(
    const int index, const scalar_t* grad_col, const scalar_t* data_value, const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index, const scalar_t* data_sampling_loc, const scalar_t* data_attn_weight,
    const int spatial_size, const int num_heads, const int channels, const int num_levels, const int num_query,
    const int num_point, scalar_t* grad_value, scalar_t* grad_sampling_loc, scalar_t* grad_attn_weight,
    const PointReducer& point_reducer) {
    int temp = index;
    const int c_col = temp % channels;
    temp /= channels;
    const int sampling_index = temp;
    const int m_col = temp % num_heads;
    temp /= num_heads;
    temp /= num_query;
    const int b_col = temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    grad_sampling_loc += data_weight_ptr << 1;
    grad_attn_weight += data_weight_ptr;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col = 0; l_col < num_levels; ++l_col) {
        const int level_start_id = data_level_start_index[l_col];
        const int spatial_h_ptr = l_col << 1;
        const int spatial_h = data_spatial_shapes[spatial_h_ptr];
        const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
        const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
        const scalar_t* data_value_ptr = data_value + value_ptr_offset;
        scalar_t* grad_value_ptr = grad_value + value_ptr_offset;

        for (int p_col = 0; p_col < num_point; ++p_col) {
            const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
            const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
            const scalar_t weight = data_attn_weight[data_weight_ptr];
            point_reducer(MsDeformableCol2ImPointArgs<scalar_t>{data_value_ptr, spatial_h, spatial_w, num_heads,
                                                                channels, loc_w, loc_h, m_col, c_col, top_grad, weight,
                                                                grad_value_ptr, grad_sampling_loc, grad_attn_weight});
            ++data_weight_ptr;
            data_loc_w_ptr += 2;
            ++grad_attn_weight;
            grad_sampling_loc += 2;
        }
    }
}

#define MMLTK_RFDTR_DEFORM_COMMON_INPUT_PARAMS(scalar_t)                                                            \
    const scalar_t *data_value, const int64_t *data_spatial_shapes, const int64_t *data_level_start_index,          \
        const scalar_t *data_sampling_loc, const scalar_t *data_attn_weight, const int batch_size,                  \
        const int spatial_size, const int num_heads, const int channels, const int num_levels, const int num_query, \
        const int num_point

#define MMLTK_RFDTR_DEFORM_IM2COL_PARAMS(scalar_t) MMLTK_RFDTR_DEFORM_COMMON_INPUT_PARAMS(scalar_t), scalar_t* data_col

#define MMLTK_RFDTR_DEFORM_COL2IM_PARAMS(scalar_t)                                                    \
    const scalar_t *grad_col, MMLTK_RFDTR_DEFORM_COMMON_INPUT_PARAMS(scalar_t), scalar_t *grad_value, \
        scalar_t *grad_sampling_loc, scalar_t *grad_attn_weight

template <typename scalar_t>
__global__ void ms_deformable_im2col_gpu_kernel(const int n, MMLTK_RFDTR_DEFORM_IM2COL_PARAMS(scalar_t)) {
    CUDA_KERNEL_LOOP(index, n) {
        int _temp = index;
        const int c_col = _temp % channels;
        _temp /= channels;
        const int sampling_index = _temp;
        const int m_col = _temp % num_heads;
        _temp /= num_heads;
        _temp /= num_query;
        const int b_col = _temp;

        scalar_t* data_col_ptr = data_col + index;
        int data_weight_ptr = sampling_index * num_levels * num_point;
        int data_loc_w_ptr = data_weight_ptr << 1;
        const int qid_stride = num_heads * channels;
        const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;
        scalar_t col = 0;

        for (int l_col = 0; l_col < num_levels; ++l_col) {
            const int level_start_id = data_level_start_index[l_col];
            const int spatial_h_ptr = l_col << 1;
            const int spatial_h = data_spatial_shapes[spatial_h_ptr];
            const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
            const scalar_t* data_value_ptr = data_value + (data_value_ptr_init_offset + level_start_id * qid_stride);
            for (int p_col = 0; p_col < num_point; ++p_col) {
                const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
                const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
                const scalar_t weight = data_attn_weight[data_weight_ptr];

                const scalar_t h_im = loc_h * spatial_h - 0.5;
                const scalar_t w_im = loc_w * spatial_w - 0.5;

                if (h_im > -1 && w_im > -1 && h_im < spatial_h && w_im < spatial_w) {
                    col += ms_deform_attn_im2col_bilinear(data_value_ptr, spatial_h, spatial_w, num_heads, channels,
                                                          h_im, w_im, m_col, c_col) *
                           weight;
                }

                data_weight_ptr += 1;
                data_loc_w_ptr += 2;
            }
        }
        *data_col_ptr = col;
    }
}

#define MMLTK_RFDTR_SHARED_COL2IM_KERNEL_BODY(cache_grad_sampling_loc_expr, cache_grad_attn_weight_expr,               \
                                              reduction_count_expr, reduction_type_expr, store_type_expr)              \
    const unsigned int tid = threadIdx.x;                                                                              \
    ms_deformable_col2im_kernel_body(                                                                                  \
        index, grad_col, data_value, data_spatial_shapes, data_level_start_index, data_sampling_loc, data_attn_weight, \
        spatial_size, num_heads, channels, num_levels, num_query, num_point, grad_value, grad_sampling_loc,            \
        grad_attn_weight,                                                                                              \
        SharedMemoryCol2ImPointReducer<scalar_t, reduction_type_expr<scalar_t, unsigned int>,                          \
                                       store_type_expr<scalar_t>>{                                                     \
            cache_grad_sampling_loc_expr, cache_grad_attn_weight_expr, tid,                                            \
            reduction_type_expr<scalar_t, unsigned int>{reduction_count_expr}, store_type_expr<scalar_t>{}})

#define MMLTK_RFDTR_DEFINE_STATIC_SHM_COL2IM_KERNEL(kernel_name, reduction_type, store_type)                  \
    template <typename scalar_t, unsigned int blockSize>                                                      \
    __global__ void kernel_name(const int n, MMLTK_RFDTR_DEFORM_COL2IM_PARAMS(scalar_t)) {                    \
        CUDA_KERNEL_LOOP(index, n) {                                                                          \
            __shared__ scalar_t cache_grad_sampling_loc[blockSize * 2];                                       \
            __shared__ scalar_t cache_grad_attn_weight[blockSize];                                            \
            MMLTK_RFDTR_SHARED_COL2IM_KERNEL_BODY(cache_grad_sampling_loc, cache_grad_attn_weight, blockSize, \
                                                  reduction_type, store_type);                                \
        }                                                                                                     \
    }

#define MMLTK_RFDTR_DEFINE_DYNAMIC_SHM_COL2IM_KERNEL(kernel_name, reduction_type, store_type)                         \
    template <typename scalar_t>                                                                                      \
    __global__ void kernel_name(const int n, MMLTK_RFDTR_DEFORM_COL2IM_PARAMS(scalar_t)) {                            \
        CUDA_KERNEL_LOOP(index, n) {                                                                                  \
            extern __shared__ int shared_storage[];                                                                   \
            scalar_t* cache_grad_sampling_loc = reinterpret_cast<scalar_t*>(shared_storage);                          \
            scalar_t* cache_grad_attn_weight = cache_grad_sampling_loc + 2 * blockDim.x;                              \
            MMLTK_RFDTR_SHARED_COL2IM_KERNEL_BODY(cache_grad_sampling_loc, cache_grad_attn_weight,                    \
                                                  static_cast<unsigned int>(blockDim.x), reduction_type, store_type); \
        }                                                                                                             \
    }

MMLTK_RFDTR_DEFINE_STATIC_SHM_COL2IM_KERNEL(ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v1,
                                            SharedLinearGradientReduction, DirectGradientStore);
MMLTK_RFDTR_DEFINE_STATIC_SHM_COL2IM_KERNEL(ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v2,
                                            SharedTreeGradientReduction, DirectGradientStore);
MMLTK_RFDTR_DEFINE_DYNAMIC_SHM_COL2IM_KERNEL(ms_deformable_col2im_gpu_kernel_shm_reduce_v1,
                                             SharedLinearGradientReduction, DirectGradientStore);
MMLTK_RFDTR_DEFINE_DYNAMIC_SHM_COL2IM_KERNEL(ms_deformable_col2im_gpu_kernel_shm_reduce_v2,
                                             SharedTreeGradientReductionV2, DirectGradientStore);
MMLTK_RFDTR_DEFINE_DYNAMIC_SHM_COL2IM_KERNEL(ms_deformable_col2im_gpu_kernel_shm_reduce_v2_multi_blocks,
                                             SharedTreeGradientReductionV2, AtomicGradientStore);

#undef MMLTK_RFDTR_DEFINE_DYNAMIC_SHM_COL2IM_KERNEL
#undef MMLTK_RFDTR_DEFINE_STATIC_SHM_COL2IM_KERNEL
#undef MMLTK_RFDTR_SHARED_COL2IM_KERNEL_BODY

template <typename scalar_t>
__global__ void ms_deformable_col2im_gpu_kernel_gm(const int n, MMLTK_RFDTR_DEFORM_COL2IM_PARAMS(scalar_t)) {
    CUDA_KERNEL_LOOP(index, n) {
        ms_deformable_col2im_kernel_body(index, grad_col, data_value, data_spatial_shapes, data_level_start_index,
                                         data_sampling_loc, data_attn_weight, spatial_size, num_heads, channels,
                                         num_levels, num_query, num_point, grad_value, grad_sampling_loc,
                                         grad_attn_weight, GlobalMemoryCol2ImPointReducer<scalar_t>{});
    }
}

template <typename scalar_t>
struct MsDeformableCol2ImLaunchArgs {
    const scalar_t* grad_col = nullptr;
    const scalar_t* data_value = nullptr;
    const int64_t* data_spatial_shapes = nullptr;
    const int64_t* data_level_start_index = nullptr;
    const scalar_t* data_sampling_loc = nullptr;
    const scalar_t* data_attn_weight = nullptr;
    int batch_size = 0;
    int spatial_size = 0;
    int num_heads = 0;
    int channels = 0;
    int num_levels = 0;
    int num_query = 0;
    int num_point = 0;
    int num_threads = 0;
    scalar_t* grad_value = nullptr;
    scalar_t* grad_sampling_loc = nullptr;
    scalar_t* grad_attn_weight = nullptr;

    int num_kernels() const {
        return batch_size * num_query * num_heads * channels;
    }
};

#define MMLTK_RFDTR_COL2IM_KERNEL_ARGS(args)                                                                           \
    (args).num_kernels(), (args).grad_col, (args).data_value, (args).data_spatial_shapes,                              \
        (args).data_level_start_index, (args).data_sampling_loc, (args).data_attn_weight, (args).batch_size,           \
        (args).spatial_size, (args).num_heads, (args).channels, (args).num_levels, (args).num_query, (args).num_point, \
        (args).grad_value, (args).grad_sampling_loc, (args).grad_attn_weight

template <typename scalar_t>
inline int ms_deformable_col2im_dynamic_shared_bytes(const MsDeformableCol2ImLaunchArgs<scalar_t>& args) {
    return args.num_threads * 3 * sizeof(scalar_t);
}

#define MMLTK_RFDTR_COL2IM_LAUNCH_CONFIG(args, shared_bytes, stream) \
    GET_BLOCKS((args).num_kernels(), (args).num_threads), (args).num_threads, (shared_bytes), (stream)

#define MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_1(args, stream, shared_bytes, kernel)                  \
    launch_ms_deformable_cuda_kernel(                                                           \
        [&]() {                                                                                 \
            kernel<scalar_t><<<MMLTK_RFDTR_COL2IM_LAUNCH_CONFIG(args, shared_bytes, stream)>>>( \
                MMLTK_RFDTR_COL2IM_KERNEL_ARGS(args));                                          \
        },                                                                                      \
        "ms_deformable_col2im_cuda")

#define MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_2(args, stream, shared_bytes, kernel, block_size)                  \
    launch_ms_deformable_cuda_kernel(                                                                       \
        [&]() {                                                                                             \
            kernel<scalar_t, block_size><<<MMLTK_RFDTR_COL2IM_LAUNCH_CONFIG(args, shared_bytes, stream)>>>( \
                MMLTK_RFDTR_COL2IM_KERNEL_ARGS(args));                                                      \
        },                                                                                                  \
        "ms_deformable_col2im_cuda")

template <typename scalar_t>
void ms_deformable_im2col_cuda(cudaStream_t stream, MMLTK_RFDTR_DEFORM_IM2COL_PARAMS(scalar_t)) {
    const int num_kernels = batch_size * num_query * num_heads * channels;
    const int num_threads = CUDA_NUM_THREADS;
    launch_ms_deformable_cuda_kernel(
        [&]() {
            ms_deformable_im2col_gpu_kernel<scalar_t><<<GET_BLOCKS(num_kernels, num_threads), num_threads, 0, stream>>>(
                num_kernels, data_value, data_spatial_shapes, data_level_start_index, data_sampling_loc,
                data_attn_weight, batch_size, spatial_size, num_heads, channels, num_levels, num_query, num_point,
                data_col);
        },
        "ms_deformable_im2col_cuda");
}

#define MMLTK_RFDTR_DEFINE_COL2IM_BLOCKSIZE_AWARE_LAUNCHER(launcher_name, kernel_name)                   \
    template <typename scalar_t, unsigned int blockSize>                                                 \
    inline void launcher_name(const MsDeformableCol2ImLaunchArgs<scalar_t>& args, cudaStream_t stream) { \
        MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_2(args, stream, 0, kernel_name, blockSize);                     \
    }

#define MMLTK_RFDTR_DEFINE_COL2IM_DYNAMIC_LAUNCHER(launcher_name, kernel_name, shared_bytes_expr)        \
    template <typename scalar_t>                                                                         \
    inline void launcher_name(const MsDeformableCol2ImLaunchArgs<scalar_t>& args, cudaStream_t stream) { \
        MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_1(args, stream, shared_bytes_expr, kernel_name);                \
    }

MMLTK_RFDTR_DEFINE_COL2IM_BLOCKSIZE_AWARE_LAUNCHER(launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1,
                                                   ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v1);
MMLTK_RFDTR_DEFINE_COL2IM_BLOCKSIZE_AWARE_LAUNCHER(launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2,
                                                   ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v2);
MMLTK_RFDTR_DEFINE_COL2IM_DYNAMIC_LAUNCHER(launch_ms_deformable_col2im_cuda_shm_reduce_v1,
                                           ms_deformable_col2im_gpu_kernel_shm_reduce_v1,
                                           ms_deformable_col2im_dynamic_shared_bytes(args));
MMLTK_RFDTR_DEFINE_COL2IM_DYNAMIC_LAUNCHER(launch_ms_deformable_col2im_cuda_shm_reduce_v2,
                                           ms_deformable_col2im_gpu_kernel_shm_reduce_v2,
                                           ms_deformable_col2im_dynamic_shared_bytes(args));
MMLTK_RFDTR_DEFINE_COL2IM_DYNAMIC_LAUNCHER(launch_ms_deformable_col2im_cuda_shm_reduce_v2_multi_blocks,
                                           ms_deformable_col2im_gpu_kernel_shm_reduce_v2_multi_blocks,
                                           ms_deformable_col2im_dynamic_shared_bytes(args));

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_gm(const MsDeformableCol2ImLaunchArgs<scalar_t>& args,
                                                cudaStream_t stream) {
    MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_1(args, stream, 0, ms_deformable_col2im_gpu_kernel_gm);
}

#undef MMLTK_RFDTR_DEFINE_COL2IM_DYNAMIC_LAUNCHER
#undef MMLTK_RFDTR_DEFINE_COL2IM_BLOCKSIZE_AWARE_LAUNCHER

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_large_channels(const MsDeformableCol2ImLaunchArgs<scalar_t>& args,
                                                            cudaStream_t stream) {
    if ((args.channels & 1023) == 0) {
        launch_ms_deformable_col2im_cuda_shm_reduce_v2_multi_blocks(args, stream);
    } else {
        launch_ms_deformable_col2im_cuda_gm(args, stream);
    }
}

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_dynamic_reduce(const MsDeformableCol2ImLaunchArgs<scalar_t>& args,
                                                            cudaStream_t stream) {
    if (args.channels < 64) {
        launch_ms_deformable_col2im_cuda_shm_reduce_v1(args, stream);
    } else {
        launch_ms_deformable_col2im_cuda_shm_reduce_v2(args, stream);
    }
}

#define MMLTK_RFDTR_DISPATCH_COL2IM_CASE(block_size, launcher) \
    case block_size:                                           \
        launcher<scalar_t, block_size>(args, stream);          \
        break

#define MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V1()                                                \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(1, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1);  \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(2, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1);  \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(4, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1);  \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(8, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1);  \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(16, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1); \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(32, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1)

#define MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V2()                                                 \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(64, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2);  \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(128, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2); \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(256, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2); \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(512, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2); \
    MMLTK_RFDTR_DISPATCH_COL2IM_CASE(1024, launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2)

template <typename scalar_t>
void ms_deformable_col2im_cuda(cudaStream_t stream, MMLTK_RFDTR_DEFORM_COL2IM_PARAMS(scalar_t)) {
    const int num_threads = (channels > CUDA_NUM_THREADS) ? CUDA_NUM_THREADS : channels;
    const MsDeformableCol2ImLaunchArgs<scalar_t> args{
        grad_col,          data_value,       data_spatial_shapes, data_level_start_index,
        data_sampling_loc, data_attn_weight, batch_size,          spatial_size,
        num_heads,         channels,         num_levels,          num_query,
        num_point,         num_threads,      grad_value,          grad_sampling_loc,
        grad_attn_weight,
    };
    if (args.channels > 1024) {
        launch_ms_deformable_col2im_cuda_large_channels(args, stream);
        return;
    }

    switch (args.channels) {
        MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V1();
        MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V2();
        default:
            launch_ms_deformable_col2im_cuda_dynamic_reduce(args, stream);
            break;
    }
}

#undef MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V2
#undef MMLTK_RFDTR_DISPATCH_COL2IM_SPECIALIZED_CASES_V1
#undef MMLTK_RFDTR_DISPATCH_COL2IM_CASE
#undef MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_2
#undef MMLTK_RFDTR_LAUNCH_COL2IM_KERNEL_1
#undef MMLTK_RFDTR_COL2IM_LAUNCH_CONFIG
#undef MMLTK_RFDTR_COL2IM_KERNEL_ARGS
#undef MMLTK_RFDTR_DEFORM_COL2IM_PARAMS
#undef MMLTK_RFDTR_DEFORM_IM2COL_PARAMS
#undef MMLTK_RFDTR_DEFORM_COMMON_INPUT_PARAMS
