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
struct BoxPairGeometry {
    const T* b1 = nullptr;
    const T* b2 = nullptr;
    T area1 = 0;
    T area2 = 0;
    T intersection = 0;
    T union_area = 0;
};

template <typename T>
__device__ inline BoxPairGeometry<T> make_box_pair_geometry(const T* boxes1, const T* boxes2, const int idx,
                                                            const int M) {
    const int n = idx / M;
    const int m = idx % M;
    const T* b1 = boxes1 + n * 4;
    const T* b2 = boxes2 + m * 4;
    const T left = max(b1[0], b2[0]);
    const T top = max(b1[1], b2[1]);
    const T right = min(b1[2], b2[2]);
    const T bottom = min(b1[3], b2[3]);
    const T intersection = max((T)0, right - left) * max((T)0, bottom - top);
    const T area1 = box_area(b1);
    const T area2 = box_area(b2);
    return BoxPairGeometry<T>{b1, b2, area1, area2, intersection, area1 + area2 - intersection};
}

template <typename T>
__global__ void box_iou_kernel(const T* boxes1,  // [N, 4]
                               const T* boxes2,  // [M, 4]
                               T* iou,           // [N, M]
                               int N, int M) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N * M)
        return;

    const BoxPairGeometry<T> geometry = make_box_pair_geometry(boxes1, boxes2, idx, M);
    iou[idx] = geometry.intersection / geometry.union_area;
}

template <typename T>
__global__ void generalized_box_iou_kernel(const T* boxes1,  // [N, 4]
                                           const T* boxes2,  // [M, 4]
                                           T* giou,          // [N, M]
                                           int N, int M) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N * M)
        return;

    const BoxPairGeometry<T> geometry = make_box_pair_geometry(boxes1, boxes2, idx, M);
    const T* b1 = geometry.b1;
    const T* b2 = geometry.b2;
    T iou = geometry.intersection / geometry.union_area;

    T c_left = min(b1[0], b2[0]);
    T c_top = min(b1[1], b2[1]);
    T c_right = max(b1[2], b2[2]);
    T c_bottom = max(b1[3], b2[3]);

    T c_w = c_right - c_left;
    T c_h = c_bottom - c_top;
    T c_area = (c_w > 0 && c_h > 0) ? c_w * c_h : (T)0;

    giou[idx] = iou - (c_area - geometry.union_area) / c_area;
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
                                       boxes1.data_ptr<scalar_t>(), boxes2.data_ptr<scalar_t>(),
                                       iou.data_ptr<scalar_t>(), N, M);
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

    AT_DISPATCH_FLOATING_TYPES(
        boxes1.scalar_type(), "generalized_box_iou_cuda", ([&] {
            generalized_box_iou_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
                boxes1.data_ptr<scalar_t>(), boxes2.data_ptr<scalar_t>(), giou.data_ptr<scalar_t>(), N, M);
        }));

    return giou;
}

}  // namespace mmltk::rfdetr
