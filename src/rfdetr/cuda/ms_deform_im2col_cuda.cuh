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

#define CUDA_KERNEL_LOOP(i, n)                          \
  for (int i = blockIdx.x * blockDim.x + threadIdx.x;   \
      i < (n);                                          \
      i += blockDim.x * gridDim.x)

const int CUDA_NUM_THREADS = 1024;
inline int GET_BLOCKS(const int N, const int num_threads)
{
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
__device__ BilinearSampleGeometry<scalar_t> make_bilinear_sample_geometry(const int height,
                                                                          const int width,
                                                                          const int nheads,
                                                                          const int channels,
                                                                          const scalar_t h,
                                                                          const scalar_t w,
                                                                          const int m,
                                                                          const int c) {
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
__device__ scalar_t ms_deform_attn_apply_bilinear_corners(
    const scalar_t* bottom_data,
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
  __device__ void corner(int,
                         int,
                         scalar_t,
                         scalar_t,
                         const BilinearSampleGeometry<scalar_t>&) const {}

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

  __device__ void corner(int corner_index,
                         int ptr,
                         scalar_t weight,
                         scalar_t corner_value,
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
        grad_sampling_loc,
        grad_attn_weight,
        grad_h_weight,
        grad_w_weight,
        top_grad,
        top_grad_value,
        value,
        geometry.height,
        geometry.width);
    return value;
  }
};

struct BilinearDirectAccumulatorPolicy {
  template <typename ValueT>
  __device__ void store(ValueT* grad_sampling_loc,
                        ValueT* grad_attn_weight,
                        const ValueT grad_h_weight,
                        const ValueT grad_w_weight,
                        const ValueT top_grad,
                        const ValueT top_grad_value,
                        const ValueT value,
                        const int height_value,
                        const int width_value) const {
    *grad_attn_weight = top_grad * value;
    *grad_sampling_loc = width_value * grad_w_weight * top_grad_value;
    *(grad_sampling_loc + 1) = height_value * grad_h_weight * top_grad_value;
  }
};

struct BilinearAtomicAccumulatorPolicy {
  template <typename ValueT>
  __device__ void store(ValueT* grad_sampling_loc,
                        ValueT* grad_attn_weight,
                        const ValueT grad_h_weight,
                        const ValueT grad_w_weight,
                        const ValueT top_grad,
                        const ValueT top_grad_value,
                        const ValueT value,
                        const int height_value,
                        const int width_value) const {
    gpuAtomicAdd(grad_attn_weight, top_grad * value);
    gpuAtomicAdd(grad_sampling_loc, width_value * grad_w_weight * top_grad_value);
    gpuAtomicAdd(grad_sampling_loc + 1, height_value * grad_h_weight * top_grad_value);
  }
};

template <typename scalar_t, typename AccumulationPolicy>
__device__ void ms_deform_attn_bilinear_backward(const scalar_t* bottom_data,
                                                 const BilinearSampleGeometry<scalar_t>& geometry,
                                                 const scalar_t top_grad,
                                                 const scalar_t attn_weight,
                                                 scalar_t* grad_value,
                                                 scalar_t* grad_sampling_loc,
                                                 scalar_t* grad_attn_weight,
                                                 const AccumulationPolicy& accumulation_policy) {
  const scalar_t top_grad_value = top_grad * attn_weight;
  BilinearBackwardPolicy<scalar_t, AccumulationPolicy> policy{
      geometry,
      grad_value,
      grad_sampling_loc,
      grad_attn_weight,
      top_grad,
      top_grad_value,
      0,
      0,
      accumulation_policy,
  };
  (void)ms_deform_attn_apply_bilinear_corners(bottom_data, geometry, policy);
}

template <typename scalar_t, typename AccumulationPolicy>
__device__ void ms_deform_attn_col2im_bilinear(const scalar_t* bottom_data,
                                               const int height,
                                               const int width,
                                               const int nheads,
                                               const int channels,
                                               const scalar_t h,
                                               const scalar_t w,
                                               const int m,
                                               const int c,
                                               const scalar_t top_grad,
                                               const scalar_t attn_weight,
                                               scalar_t* grad_value,
                                               scalar_t* grad_sampling_loc,
                                               scalar_t* grad_attn_weight,
                                               const AccumulationPolicy& accumulation_policy) {
  const BilinearSampleGeometry<scalar_t> geometry =
      make_bilinear_sample_geometry(height, width, nheads, channels, h, w, m, c);
  ms_deform_attn_bilinear_backward(bottom_data,
                                   geometry,
                                   top_grad,
                                   attn_weight,
                                   grad_value,
                                   grad_sampling_loc,
                                   grad_attn_weight,
                                   accumulation_policy);
}

template <typename scalar_t>
__device__ scalar_t ms_deform_attn_im2col_bilinear(const scalar_t* &bottom_data, 
                                                   const int &height, const int &width, const int &nheads, const int &channels,
                                                   const scalar_t &h, const scalar_t &w, const int &m, const int &c)
{
  const BilinearSampleGeometry<scalar_t> geometry =
      make_bilinear_sample_geometry(height, width, nheads, channels, h, w, m, c);
  return ms_deform_attn_apply_bilinear_corners(bottom_data, geometry, BilinearForwardPolicy<scalar_t>{});
}

inline void report_ms_deformable_cuda_error(const char* error_label) {
  const cudaError_t err = cudaGetLastError();
  if (err != cudaSuccess)
  {
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

  __device__ void operator()(scalar_t* cache_grad_sampling_loc,
                             scalar_t* cache_grad_attn_weight,
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

  __device__ void operator()(scalar_t* cache_grad_sampling_loc,
                             scalar_t* cache_grad_attn_weight,
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

  __device__ void operator()(scalar_t* cache_grad_sampling_loc,
                             scalar_t* cache_grad_attn_weight,
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

template <typename scalar_t>
struct DirectGradientStore {
  __device__ void operator()(scalar_t* grad_sampling_loc,
                             scalar_t* grad_attn_weight,
                             const scalar_t* cache_grad_sampling_loc,
                             const scalar_t* cache_grad_attn_weight) const {
    *grad_sampling_loc = cache_grad_sampling_loc[0];
    *(grad_sampling_loc + 1) = cache_grad_sampling_loc[1];
    *grad_attn_weight = cache_grad_attn_weight[0];
  }
};

template <typename scalar_t>
struct AtomicGradientStore {
  __device__ void operator()(scalar_t* grad_sampling_loc,
                             scalar_t* grad_attn_weight,
                             const scalar_t* cache_grad_sampling_loc,
                             const scalar_t* cache_grad_attn_weight) const {
    gpuAtomicAdd(grad_sampling_loc, cache_grad_sampling_loc[0]);
    gpuAtomicAdd(grad_sampling_loc + 1, cache_grad_sampling_loc[1]);
    gpuAtomicAdd(grad_attn_weight, cache_grad_attn_weight[0]);
  }
};

template <typename scalar_t, typename ReductionFn, typename StoreFn>
__device__ inline void ms_deformable_col2im_reduce_point(
    const scalar_t* data_value_ptr,
    const int spatial_h,
    const int spatial_w,
    const int num_heads,
    const int channels,
    const scalar_t loc_w,
    const scalar_t loc_h,
    const int m_col,
    const int c_col,
    const scalar_t top_grad,
    const scalar_t weight,
    scalar_t* grad_value_ptr,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    scalar_t* cache_grad_sampling_loc,
    scalar_t* cache_grad_attn_weight,
    const unsigned int tid,
    const ReductionFn& reduction_fn,
    const StoreFn& store_fn) {
  *(cache_grad_sampling_loc + (tid << 1)) = 0;
  *(cache_grad_sampling_loc + ((tid << 1) + 1)) = 0;
  *(cache_grad_attn_weight + tid) = 0;
  const scalar_t h_im = loc_h * spatial_h - 0.5;
  const scalar_t w_im = loc_w * spatial_w - 0.5;
  if (h_im > -1 && w_im > -1 && h_im < spatial_h && w_im < spatial_w) {
    ms_deform_attn_col2im_bilinear(
        data_value_ptr,
        spatial_h,
        spatial_w,
        num_heads,
        channels,
        h_im,
        w_im,
        m_col,
        c_col,
        top_grad,
        weight,
        grad_value_ptr,
        cache_grad_sampling_loc + (tid << 1),
        cache_grad_attn_weight + tid,
        BilinearDirectAccumulatorPolicy{});
  }

  __syncthreads();
  reduction_fn(cache_grad_sampling_loc, cache_grad_attn_weight, tid);
  __syncthreads();
  if (tid == 0) {
    store_fn(grad_sampling_loc, grad_attn_weight, cache_grad_sampling_loc, cache_grad_attn_weight);
  }
  __syncthreads();
}

template <typename scalar_t>
__global__ void ms_deformable_im2col_gpu_kernel(const int n,
                                                const scalar_t *data_value, 
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *data_col)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    scalar_t *data_col_ptr = data_col + index;
    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;
    scalar_t col = 0;
    
    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const scalar_t *data_value_ptr = data_value + (data_value_ptr_init_offset + level_start_id * qid_stride);
      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];

        const scalar_t h_im = loc_h * spatial_h - 0.5;
        const scalar_t w_im = loc_w * spatial_w - 0.5;

        if (h_im > -1 && w_im > -1 && h_im < spatial_h && w_im < spatial_w)
        {
          col += ms_deform_attn_im2col_bilinear(data_value_ptr, spatial_h, spatial_w, num_heads, channels, h_im, w_im, m_col, c_col) * weight;
        }

        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
      }
    }
    *data_col_ptr = col;
  }
}

template <typename scalar_t, unsigned int blockSize>
__global__ void ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v1(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    __shared__ scalar_t cache_grad_sampling_loc[blockSize * 2];
    __shared__ scalar_t cache_grad_attn_weight[blockSize];
    const unsigned int tid = threadIdx.x;
    const SharedLinearGradientReduction<scalar_t, unsigned int> reduction{blockSize};
    const DirectGradientStore<scalar_t> store{};
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];
        ms_deformable_col2im_reduce_point(
            data_value_ptr,
            spatial_h,
            spatial_w,
            num_heads,
            channels,
            loc_w,
            loc_h,
            m_col,
            c_col,
            top_grad,
            weight,
            grad_value_ptr,
            grad_sampling_loc,
            grad_attn_weight,
            cache_grad_sampling_loc,
            cache_grad_attn_weight,
            tid,
            reduction,
            store);
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}


template <typename scalar_t, unsigned int blockSize>
__global__ void ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v2(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    __shared__ scalar_t cache_grad_sampling_loc[blockSize * 2];
    __shared__ scalar_t cache_grad_attn_weight[blockSize];
    const unsigned int tid = threadIdx.x;
    const SharedTreeGradientReduction<scalar_t, unsigned int> reduction{blockSize};
    const DirectGradientStore<scalar_t> store{};
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];
        ms_deformable_col2im_reduce_point(
            data_value_ptr,
            spatial_h,
            spatial_w,
            num_heads,
            channels,
            loc_w,
            loc_h,
            m_col,
            c_col,
            top_grad,
            weight,
            grad_value_ptr,
            grad_sampling_loc,
            grad_attn_weight,
            cache_grad_sampling_loc,
            cache_grad_attn_weight,
            tid,
            reduction,
            store);
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}


template <typename scalar_t>
__global__ void ms_deformable_col2im_gpu_kernel_shm_reduce_v1(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    extern __shared__ int _s[];
    scalar_t* cache_grad_sampling_loc = (scalar_t*)_s;
    scalar_t* cache_grad_attn_weight = cache_grad_sampling_loc + 2 * blockDim.x;
    const unsigned int tid = threadIdx.x;
    const SharedLinearGradientReduction<scalar_t, unsigned int> reduction{blockDim.x};
    const DirectGradientStore<scalar_t> store{};
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];
        ms_deformable_col2im_reduce_point(
            data_value_ptr,
            spatial_h,
            spatial_w,
            num_heads,
            channels,
            loc_w,
            loc_h,
            m_col,
            c_col,
            top_grad,
            weight,
            grad_value_ptr,
            grad_sampling_loc,
            grad_attn_weight,
            cache_grad_sampling_loc,
            cache_grad_attn_weight,
            tid,
            reduction,
            store);
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}

template <typename scalar_t>
__global__ void ms_deformable_col2im_gpu_kernel_shm_reduce_v2(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    extern __shared__ int _s[];
    scalar_t* cache_grad_sampling_loc = (scalar_t*)_s;
    scalar_t* cache_grad_attn_weight = cache_grad_sampling_loc + 2 * blockDim.x;
    const unsigned int tid = threadIdx.x;
    const SharedTreeGradientReductionV2<scalar_t, unsigned int> reduction{blockDim.x};
    const DirectGradientStore<scalar_t> store{};
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];
        ms_deformable_col2im_reduce_point(
            data_value_ptr,
            spatial_h,
            spatial_w,
            num_heads,
            channels,
            loc_w,
            loc_h,
            m_col,
            c_col,
            top_grad,
            weight,
            grad_value_ptr,
            grad_sampling_loc,
            grad_attn_weight,
            cache_grad_sampling_loc,
            cache_grad_attn_weight,
            tid,
            reduction,
            store);
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}

template <typename scalar_t>
__global__ void ms_deformable_col2im_gpu_kernel_shm_reduce_v2_multi_blocks(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    extern __shared__ int _s[];
    scalar_t* cache_grad_sampling_loc = (scalar_t*)_s;
    scalar_t* cache_grad_attn_weight = cache_grad_sampling_loc + 2 * blockDim.x;
    const unsigned int tid = threadIdx.x;
    const SharedTreeGradientReductionV2<scalar_t, unsigned int> reduction{blockDim.x};
    const AtomicGradientStore<scalar_t> store{};
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];
        ms_deformable_col2im_reduce_point(
            data_value_ptr,
            spatial_h,
            spatial_w,
            num_heads,
            channels,
            loc_w,
            loc_h,
            m_col,
            c_col,
            top_grad,
            weight,
            grad_value_ptr,
            grad_sampling_loc,
            grad_attn_weight,
            cache_grad_sampling_loc,
            cache_grad_attn_weight,
            tid,
            reduction,
            store);
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}


template <typename scalar_t>
__global__ void ms_deformable_col2im_gpu_kernel_gm(const int n,
                                                const scalar_t *grad_col,
                                                const scalar_t *data_value,
                                                const int64_t *data_spatial_shapes,
                                                const int64_t *data_level_start_index, 
                                                const scalar_t *data_sampling_loc,
                                                const scalar_t *data_attn_weight,
                                                const int batch_size, 
                                                const int spatial_size, 
                                                const int num_heads,
                                                const int channels, 
                                                const int num_levels,
                                                const int num_query,
                                                const int num_point,
                                                scalar_t *grad_value,
                                                scalar_t *grad_sampling_loc,
                                                scalar_t *grad_attn_weight)
{
  CUDA_KERNEL_LOOP(index, n)
  {
    int _temp = index;
    const int c_col = _temp % channels;
    _temp /= channels;
    const int sampling_index = _temp; 
    const int m_col = _temp % num_heads;
    _temp /= num_heads;
    _temp /= num_query;
    const int b_col = _temp;

    const scalar_t top_grad = grad_col[index];

    int data_weight_ptr = sampling_index * num_levels * num_point;
    int data_loc_w_ptr = data_weight_ptr << 1;
    const int grad_sampling_ptr = data_weight_ptr;
    grad_sampling_loc += grad_sampling_ptr << 1;
    grad_attn_weight += grad_sampling_ptr;
    const int grad_weight_stride = 1;
    const int grad_loc_stride = 2;
    const int qid_stride = num_heads * channels;
    const int data_value_ptr_init_offset = b_col * spatial_size * qid_stride;

    for (int l_col=0; l_col < num_levels; ++l_col)
    {
      const int level_start_id = data_level_start_index[l_col];
      const int spatial_h_ptr = l_col << 1;
      const int spatial_h = data_spatial_shapes[spatial_h_ptr];
      const int spatial_w = data_spatial_shapes[spatial_h_ptr + 1];
      const int value_ptr_offset = data_value_ptr_init_offset + level_start_id * qid_stride;
      const scalar_t *data_value_ptr = data_value + value_ptr_offset;
      scalar_t *grad_value_ptr = grad_value + value_ptr_offset;

      for (int p_col=0; p_col < num_point; ++p_col)
      {
        const scalar_t loc_w = data_sampling_loc[data_loc_w_ptr];
        const scalar_t loc_h = data_sampling_loc[data_loc_w_ptr + 1];
        const scalar_t weight = data_attn_weight[data_weight_ptr];

        const scalar_t h_im = loc_h * spatial_h - 0.5;
        const scalar_t w_im = loc_w * spatial_w - 0.5;
        if (h_im > -1 && w_im > -1 && h_im < spatial_h && w_im < spatial_w)
        {
          ms_deform_attn_col2im_bilinear(
            data_value_ptr, spatial_h, spatial_w, num_heads, channels, h_im, w_im, m_col, c_col,
            top_grad, weight, grad_value_ptr, 
            grad_sampling_loc, grad_attn_weight,
            BilinearAtomicAccumulatorPolicy{});
        }
        data_weight_ptr += 1;
        data_loc_w_ptr += 2;
        grad_attn_weight += grad_weight_stride;
        grad_sampling_loc += grad_loc_stride;
      }
    }
  }
}


template <typename scalar_t>
void ms_deformable_im2col_cuda(cudaStream_t stream,
                              const scalar_t* data_value,
                              const int64_t* data_spatial_shapes, 
                              const int64_t* data_level_start_index, 
                              const scalar_t* data_sampling_loc,
                              const scalar_t* data_attn_weight,
                              const int batch_size,
                              const int spatial_size, 
                              const int num_heads, 
                              const int channels, 
                              const int num_levels, 
                              const int num_query,
                              const int num_point,
                              scalar_t* data_col)
{
  const int num_kernels = batch_size * num_query * num_heads * channels;
  const int num_actual_kernels = batch_size * num_query * num_heads * channels;
  const int num_threads = CUDA_NUM_THREADS;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_im2col_gpu_kernel<scalar_t>
        <<<GET_BLOCKS(num_actual_kernels, num_threads), num_threads,
            0, stream>>>(
        num_kernels, data_value, data_spatial_shapes, data_level_start_index, data_sampling_loc, data_attn_weight, 
        batch_size, spatial_size, num_heads, channels, num_levels, num_query, num_point, data_col);
  },
                                   "ms_deformable_im2col_cuda");

}

template <typename scalar_t, unsigned int blockSize>
inline void launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v1<scalar_t, blockSize>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads, 0, stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t, unsigned int blockSize>
inline void launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_shm_blocksize_aware_reduce_v2<scalar_t, blockSize>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads, 0, stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_shm_reduce_v1(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_shm_reduce_v1<scalar_t>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads,
           num_threads * 3 * sizeof(scalar_t), stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_shm_reduce_v2(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_shm_reduce_v2<scalar_t>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads,
           num_threads * 3 * sizeof(scalar_t), stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_shm_reduce_v2_multi_blocks(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_shm_reduce_v2_multi_blocks<scalar_t>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads,
           num_threads * 3 * sizeof(scalar_t), stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t>
inline void launch_ms_deformable_col2im_cuda_gm(
    const scalar_t* grad_col,
    const scalar_t* data_value,
    const int64_t* data_spatial_shapes,
    const int64_t* data_level_start_index,
    const scalar_t* data_sampling_loc,
    const scalar_t* data_attn_weight,
    const int batch_size,
    const int spatial_size,
    const int num_heads,
    const int channels,
    const int num_levels,
    const int num_query,
    const int num_point,
    const int num_threads,
    scalar_t* grad_value,
    scalar_t* grad_sampling_loc,
    scalar_t* grad_attn_weight,
    cudaStream_t stream) {
  const int num_kernels = batch_size * num_query * num_heads * channels;
  launch_ms_deformable_cuda_kernel([&]() {
    ms_deformable_col2im_gpu_kernel_gm<scalar_t>
        <<<GET_BLOCKS(num_kernels, num_threads), num_threads, 0, stream>>>(
            num_kernels,
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight);
  },
                                   "ms_deformable_col2im_cuda");
}

template <typename scalar_t>
void ms_deformable_col2im_cuda(cudaStream_t stream,
                              const scalar_t* grad_col,
                              const scalar_t* data_value,
                              const int64_t * data_spatial_shapes,
                              const int64_t * data_level_start_index,
                              const scalar_t * data_sampling_loc,
                              const scalar_t * data_attn_weight,
                              const int batch_size, 
                              const int spatial_size, 
                              const int num_heads,
                              const int channels, 
                              const int num_levels,
                              const int num_query,
                              const int num_point, 
                              scalar_t* grad_value,
                              scalar_t* grad_sampling_loc,
                              scalar_t* grad_attn_weight)
{
  const int num_threads = (channels > CUDA_NUM_THREADS)?CUDA_NUM_THREADS:channels;
  if (channels > 1024)
  {
    if ((channels & 1023) == 0)
    {
      launch_ms_deformable_col2im_cuda_shm_reduce_v2_multi_blocks(
          grad_col,
          data_value,
          data_spatial_shapes,
          data_level_start_index,
          data_sampling_loc,
          data_attn_weight,
          batch_size,
          spatial_size,
          num_heads,
          channels,
          num_levels,
          num_query,
          num_point,
          num_threads,
          grad_value,
          grad_sampling_loc,
          grad_attn_weight,
          stream);
    }
    else
    {
      launch_ms_deformable_col2im_cuda_gm(
          grad_col,
          data_value,
          data_spatial_shapes,
          data_level_start_index,
          data_sampling_loc,
          data_attn_weight,
          batch_size,
          spatial_size,
          num_heads,
          channels,
          num_levels,
          num_query,
          num_point,
          num_threads,
          grad_value,
          grad_sampling_loc,
          grad_attn_weight,
          stream);
    }
  }
  else{
    switch(channels)
    {
      case 1:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 1>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 2:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 2>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 4:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 4>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 8:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 8>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 16:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 16>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 32:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v1<scalar_t, 32>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 64:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2<scalar_t, 64>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 128:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2<scalar_t, 128>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 256:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2<scalar_t, 256>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 512:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2<scalar_t, 512>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      case 1024:
        launch_ms_deformable_col2im_cuda_shm_blocksize_aware_reduce_v2<scalar_t, 1024>(
            grad_col,
            data_value,
            data_spatial_shapes,
            data_level_start_index,
            data_sampling_loc,
            data_attn_weight,
            batch_size,
            spatial_size,
            num_heads,
            channels,
            num_levels,
            num_query,
            num_point,
            num_threads,
            grad_value,
            grad_sampling_loc,
            grad_attn_weight,
            stream);
        break;
      default:
        if (channels < 64)
        {
          launch_ms_deformable_col2im_cuda_shm_reduce_v1(
              grad_col,
              data_value,
              data_spatial_shapes,
              data_level_start_index,
              data_sampling_loc,
              data_attn_weight,
              batch_size,
              spatial_size,
              num_heads,
              channels,
              num_levels,
              num_query,
              num_point,
              num_threads,
              grad_value,
              grad_sampling_loc,
              grad_attn_weight,
              stream);
        }
        else
        {
          launch_ms_deformable_col2im_cuda_shm_reduce_v2(
              grad_col,
              data_value,
              data_spatial_shapes,
              data_level_start_index,
              data_sampling_loc,
              data_attn_weight,
              batch_size,
              spatial_size,
              num_heads,
              channels,
              num_levels,
              num_query,
              num_point,
              num_threads,
              grad_value,
              grad_sampling_loc,
              grad_attn_weight,
              stream);
        }
    }
  }
}
