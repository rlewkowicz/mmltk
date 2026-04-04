#include "rfdetr/cuda/cuda_launch_common.h"

#include <torch/torch.h>
#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <cuda.h>
#include <cuda_runtime.h>

namespace mmltk::rfdetr {

template <typename T>
__device__ inline T box_area(const T* box) {
    T w = box[2] - box[0];
    T h = box[3] - box[1];
    return (w > 0 && h > 0) ? w * h : (T)0;
}

template <typename T>
__global__ void box_iou_kernel(
    const T* boxes1, // [N, 4]
    const T* boxes2, // [M, 4]
    T* iou,          // [N, M]
    int N, int M) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N * M) return;

    int n = idx / M;
    int m = idx % M;

    const T* b1 = boxes1 + n * 4;
    const T* b2 = boxes2 + m * 4;

    T area1 = box_area(b1);
    T area2 = box_area(b2);

    T left = max(b1[0], b2[0]);
    T top = max(b1[1], b2[1]);
    T right = min(b1[2], b2[2]);
    T bottom = min(b1[3], b2[3]);

    T w = max((T)0, right - left);
    T h = max((T)0, bottom - top);
    T inter = w * h;
    T uni = area1 + area2 - inter;

    iou[idx] = inter / uni;
}

template <typename T>
__global__ void generalized_box_iou_kernel(
    const T* boxes1, // [N, 4]
    const T* boxes2, // [M, 4]
    T* giou,         // [N, M]
    int N, int M) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N * M) return;

    int n = idx / M;
    int m = idx % M;

    const T* b1 = boxes1 + n * 4;
    const T* b2 = boxes2 + m * 4;

    T area1 = box_area(b1);
    T area2 = box_area(b2);

    // Intersection
    T left = max(b1[0], b2[0]);
    T top = max(b1[1], b2[1]);
    T right = min(b1[2], b2[2]);
    T bottom = min(b1[3], b2[3]);
    T inter = max((T)0, right - left) * max((T)0, bottom - top);

    // Union
    T uni = area1 + area2 - inter;
    T iou = inter / uni;

    // Smallest enclosing box (Convex Hull)
    T c_left = min(b1[0], b2[0]);
    T c_top = min(b1[1], b2[1]);
    T c_right = max(b1[2], b2[2]);
    T c_bottom = max(b1[3], b2[3]);

    T c_w = c_right - c_left;
    T c_h = c_bottom - c_top;
    T c_area = (c_w > 0 && c_h > 0) ? c_w * c_h : (T)0;

    giou[idx] = iou - (c_area - uni) / c_area;
}

torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
    c10::cuda::CUDAGuard device_guard(boxes1.device());
    int N = boxes1.size(0);
    int M = boxes2.size(0);
    auto iou = torch::empty({N, M}, boxes1.options());

    const int threads = cuda_launch::kDefaultLinearThreads;
    const int blocks = cuda_launch::linear_blocks_for(N * M, threads);

    AT_DISPATCH_FLOATING_TYPES(boxes1.scalar_type(), "box_iou_cuda", ([&] {
        box_iou_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
            boxes1.data_ptr<scalar_t>(),
            boxes2.data_ptr<scalar_t>(),
            iou.data_ptr<scalar_t>(),
            N, M);
    }));

    return iou;
}

torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
    c10::cuda::CUDAGuard device_guard(boxes1.device());
    int N = boxes1.size(0);
    int M = boxes2.size(0);
    auto giou = torch::empty({N, M}, boxes1.options());

    const int threads = cuda_launch::kDefaultLinearThreads;
    const int blocks = cuda_launch::linear_blocks_for(N * M, threads);

    AT_DISPATCH_FLOATING_TYPES(boxes1.scalar_type(), "generalized_box_iou_cuda", ([&] {
        generalized_box_iou_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
            boxes1.data_ptr<scalar_t>(),
            boxes2.data_ptr<scalar_t>(),
            giou.data_ptr<scalar_t>(),
            N, M);
    }));

    return giou;
}

} // namespace mmltk::rfdetr
