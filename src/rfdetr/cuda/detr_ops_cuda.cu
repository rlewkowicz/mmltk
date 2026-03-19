#include <torch/torch.h>
#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <cuda.h>
#include <cuda_runtime.h>

namespace fastloader::rfdetr {

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

template <typename T>
__global__ void sigmoid_focal_loss_kernel(
    const T* inputs,  // [N, C]
    const T* targets, // [N, C]
    T* output,        // [N, C]
    int size,
    T alpha,
    T gamma) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= size) return;

    T x = inputs[idx];
    T y = targets[idx];

    // Sigmoid
    T p = (T)1 / ((T)1 + exp(-x));

    // Binary Cross Entropy with logits
    // loss = max(x, 0) - x * y + log(1 + exp(-abs(x)))
    T bce = max(x, (T)0) - x * y + log((T)1 + exp(-abs(x)));

    // Focal weight
    // p_t = p * y + (1 - p) * (1 - y)
    T p_t = p * y + ((T)1 - p) * ((T)1 - y);
    T focal_weight = pow((T)1 - p_t, gamma);

    T loss = bce * focal_weight;

    // Alpha weighting
    if (alpha >= (T)0) {
        T alpha_t = alpha * y + ((T)1 - alpha) * ((T)1 - y);
        loss *= alpha_t;
    }

    output[idx] = loss;
}

torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
    c10::cuda::CUDAGuard device_guard(boxes1.device());
    int N = boxes1.size(0);
    int M = boxes2.size(0);
    auto iou = torch::empty({N, M}, boxes1.options());

    int threads = 256;
    int blocks = (N * M + threads - 1) / threads;

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

    int threads = 256;
    int blocks = (N * M + threads - 1) / threads;

    AT_DISPATCH_FLOATING_TYPES(boxes1.scalar_type(), "generalized_box_iou_cuda", ([&] {
        generalized_box_iou_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
            boxes1.data_ptr<scalar_t>(),
            boxes2.data_ptr<scalar_t>(),
            giou.data_ptr<scalar_t>(),
            N, M);
    }));

    return giou;
}

torch::Tensor sigmoid_focal_loss_cuda(
    const torch::Tensor& inputs,
    const torch::Tensor& targets,
    double alpha,
    double gamma) {
    c10::cuda::CUDAGuard device_guard(inputs.device());
    int size = inputs.numel();
    auto output = torch::empty_like(inputs);

    int threads = 256;
    int blocks = (size + threads - 1) / threads;

    AT_DISPATCH_FLOATING_TYPES(inputs.scalar_type(), "sigmoid_focal_loss_cuda", ([&] {
        sigmoid_focal_loss_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
            inputs.data_ptr<scalar_t>(),
            targets.data_ptr<scalar_t>(),
            output.data_ptr<scalar_t>(),
            size,
            (scalar_t)alpha,
            (scalar_t)gamma);
    }));

    return output;
}

template <typename T>
__global__ void fused_dice_ce_loss_kernel(
    const T* inputs,         // [N, P]
    const T* targets,        // [N, P]
    T* out_ce_per_mask,      // [N]
    T* out_dice_num_per_mask,// [N]
    T* out_dice_den_per_mask,// [N]
    int N, int P) {
    int n = blockIdx.x;
    if (n >= N) return;

    const T* in = inputs + n * P;
    const T* tgt = targets + n * P;

    float sum_ce = 0.0f;
    float sum_num = 0.0f;
    float sum_den = 0.0f;

    for (int p = threadIdx.x; p < P; p += blockDim.x) {
        float x = static_cast<float>(in[p]);
        float y = static_cast<float>(tgt[p]);

        // Sigmoid Binary Cross Entropy
        sum_ce += fmaxf(x, 0.0f) - x * y + log1pf(expf(-fabsf(x)));

        // Dice
        float prob = 1.0f / (1.0f + expf(-x));
        sum_num += prob * y;
        sum_den += prob + y;
    }

    // Parallel reduction in shared memory within the block
    extern __shared__ char shared_mem[];
    float* sdata = reinterpret_cast<float*>(shared_mem);
    int tid = threadIdx.x;

    sdata[tid] = sum_ce;
    sdata[tid + blockDim.x] = sum_num;
    sdata[tid + 2 * blockDim.x] = sum_den;
    __syncthreads();

    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sdata[tid] += sdata[tid + s];
            sdata[tid + blockDim.x] += sdata[tid + blockDim.x + s];
            sdata[tid + 2 * blockDim.x] += sdata[tid + 2 * blockDim.x + s];
        }
        __syncthreads();
    }

    if (tid == 0) {
        out_ce_per_mask[n] = static_cast<T>(sdata[0] / static_cast<float>(P));
        out_dice_num_per_mask[n] = static_cast<T>(sdata[blockDim.x]);
        out_dice_den_per_mask[n] = static_cast<T>(sdata[2 * blockDim.x]);
    }
}

std::pair<torch::Tensor, torch::Tensor> fused_dice_ce_loss_cuda(
    const torch::Tensor& inputs,
    const torch::Tensor& targets,
    double num_masks) {
    c10::cuda::CUDAGuard device_guard(inputs.device());
    int N = inputs.size(0);
    int P = inputs.size(1);
    
    auto ce_per_mask = torch::empty({N}, inputs.options());
    auto dice_num = torch::empty({N}, inputs.options());
    auto dice_den = torch::empty({N}, inputs.options());

    int threads = 256;
    size_t shared_size = 3 * threads * torch::elementSize(inputs.scalar_type());

    AT_DISPATCH_FLOATING_TYPES(inputs.scalar_type(), "fused_dice_ce_loss_cuda", ([&] {
        fused_dice_ce_loss_kernel<scalar_t><<<N, threads, shared_size, at::cuda::getCurrentCUDAStream()>>>(
            inputs.data_ptr<scalar_t>(),
            targets.data_ptr<scalar_t>(),
            ce_per_mask.data_ptr<scalar_t>(),
            dice_num.data_ptr<scalar_t>(),
            dice_den.data_ptr<scalar_t>(),
            N, P);
    }));

    auto loss_ce = ce_per_mask.sum() / num_masks;
    auto loss_dice = (1.0 - (2.0 * dice_num + 1.0) / (dice_den + 1.0)).sum() / num_masks;

    return {loss_ce, loss_dice};
}

} // namespace fastloader::rfdetr
