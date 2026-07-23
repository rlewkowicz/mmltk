#include "rfdetr/cuda/cuda_launch_common.h"

#include <torch/torch.h>
#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <cmath>
#include <limits>

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

__device__ __forceinline__ float stable_softplus(float value) {
    return fmaxf(value, 0.0f) + log1pf(expf(-fabsf(value)));
}

__device__ __forceinline__ float cxcywh_generalized_iou(const float* lhs, const float* rhs) {
    const float lhs_x1 = lhs[0] - lhs[2] * 0.5f;
    const float lhs_y1 = lhs[1] - lhs[3] * 0.5f;
    const float lhs_x2 = lhs[0] + lhs[2] * 0.5f;
    const float lhs_y2 = lhs[1] + lhs[3] * 0.5f;
    const float rhs_x1 = rhs[0] - rhs[2] * 0.5f;
    const float rhs_y1 = rhs[1] - rhs[3] * 0.5f;
    const float rhs_x2 = rhs[0] + rhs[2] * 0.5f;
    const float rhs_y2 = rhs[1] + rhs[3] * 0.5f;

    const float intersection_width = fmaxf(0.0f, fminf(lhs_x2, rhs_x2) - fmaxf(lhs_x1, rhs_x1));
    const float intersection_height = fmaxf(0.0f, fminf(lhs_y2, rhs_y2) - fmaxf(lhs_y1, rhs_y1));
    const float intersection = intersection_width * intersection_height;
    const float lhs_area = fmaxf(0.0f, lhs_x2 - lhs_x1) * fmaxf(0.0f, lhs_y2 - lhs_y1);
    const float rhs_area = fmaxf(0.0f, rhs_x2 - rhs_x1) * fmaxf(0.0f, rhs_y2 - rhs_y1);
    const float union_area = lhs_area + rhs_area - intersection;
    const float enclosing_width = fmaxf(lhs_x2, rhs_x2) - fminf(lhs_x1, rhs_x1);
    const float enclosing_height = fmaxf(lhs_y2, rhs_y2) - fminf(lhs_y1, rhs_y1);
    const float enclosing_area = fmaxf(0.0f, enclosing_width) * fmaxf(0.0f, enclosing_height);
    const float iou = intersection / union_area;
    return iou - (enclosing_area - union_area) / enclosing_area;
}

template <typename Logit, typename Box>
__global__ void matcher_cost_kernel(float* output, const Logit* pred_logits, const Box* pred_boxes,
                                    const int64_t* target_labels, const float* target_boxes,
                                    const int64_t* target_offsets, const int64_t* target_counts, int64_t batch_size,
                                    int64_t query_count, int64_t class_count, int64_t output_query_stride,
                                    int64_t max_targets, float class_cost, float bbox_cost, float giou_cost) {
    const int64_t index = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const int64_t total = batch_size * query_count * max_targets;
    if (index >= total) {
        return;
    }
    const int64_t target_slot = index % max_targets;
    const int64_t query_index = (index / max_targets) % query_count;
    const int64_t batch_index = index / (query_count * max_targets);
    if (target_slot >= target_counts[batch_index]) {
        return;
    }

    const int64_t target_index = target_offsets[batch_index] + target_slot;
    const int64_t class_index = target_labels[target_index];
    const float logit =
        static_cast<float>(pred_logits[(batch_index * query_count + query_index) * class_count + class_index]);
    const float probability = 1.0f / (1.0f + expf(-logit));
    const float one_minus_probability = 1.0f - probability;
    const float positive_class_cost = 0.25f * one_minus_probability * one_minus_probability * stable_softplus(-logit);
    const float negative_class_cost = 0.75f * probability * probability * stable_softplus(logit);

    const Box* prediction = pred_boxes + (batch_index * query_count + query_index) * 4;
    const float* target = target_boxes + target_index * 4;
    const float prediction_float[4] = {static_cast<float>(prediction[0]), static_cast<float>(prediction[1]),
                                       static_cast<float>(prediction[2]), static_cast<float>(prediction[3])};
    const float l1 = fabsf(prediction_float[0] - target[0]) + fabsf(prediction_float[1] - target[1]) +
                     fabsf(prediction_float[2] - target[2]) + fabsf(prediction_float[3] - target[3]);
    const float giou = cxcywh_generalized_iou(prediction_float, target);
    output[(batch_index * output_query_stride + query_index) * max_targets + target_slot] =
        class_cost * (positive_class_cost - negative_class_cost) + bbox_cost * l1 - giou_cost * giou;
}

template <typename Logit>
__global__ void matcher_mask_cost_kernel(float* output, const Logit* pred_logits, const float* target_masks,
                                         const int64_t* target_offsets, const int64_t* target_counts,
                                         int64_t batch_size, int64_t query_count, int64_t output_query_stride,
                                         int64_t max_targets, int64_t point_count, float ce_cost, float dice_cost) {
    constexpr int kWarpSize = 32;
    const int lane = threadIdx.x & (kWarpSize - 1);
    const int64_t warp_index =
        (static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x) / static_cast<int64_t>(kWarpSize);
    const int64_t pair_count = batch_size * query_count * max_targets;
    if (warp_index >= pair_count) {
        return;
    }
    const int64_t target_slot = warp_index % max_targets;
    const int64_t query_index = (warp_index / max_targets) % query_count;
    const int64_t batch_index = warp_index / (query_count * max_targets);
    if (target_slot >= target_counts[batch_index]) {
        return;
    }

    const int64_t target_index = target_offsets[batch_index] + target_slot;
    const Logit* prediction = pred_logits + (batch_index * query_count + query_index) * point_count;
    const float* target = target_masks + target_index * point_count;
    float ce_sum = 0.0f;
    float intersection = 0.0f;
    float probability_sum = 0.0f;
    float target_sum = 0.0f;
    for (int64_t point = lane; point < point_count; point += kWarpSize) {
        const float logit = static_cast<float>(prediction[point]);
        const float target_value = target[point];
        const float probability = 1.0f / (1.0f + expf(-logit));
        ce_sum += stable_softplus(-logit) * target_value + stable_softplus(logit) * (1.0f - target_value);
        intersection += probability * target_value;
        probability_sum += probability;
        target_sum += target_value;
    }
    for (int offset = kWarpSize / 2; offset > 0; offset /= 2) {
        ce_sum += __shfl_down_sync(0xffffffffU, ce_sum, offset);
        intersection += __shfl_down_sync(0xffffffffU, intersection, offset);
        probability_sum += __shfl_down_sync(0xffffffffU, probability_sum, offset);
        target_sum += __shfl_down_sync(0xffffffffU, target_sum, offset);
    }
    if (lane == 0) {
        const float ce = ce_sum / static_cast<float>(point_count);
        const float dice = 1.0f - (2.0f * intersection + 1.0f) / (probability_sum + target_sum + 1.0f);
        output[(batch_index * output_query_stride + query_index) * max_targets + target_slot] +=
            ce_cost * ce + dice_cost * dice;
    }
}

void check_matcher_tensor(const torch::Tensor& tensor, c10::ScalarType dtype, int64_t dimensions, const char* name) {
    TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
    TORCH_CHECK(tensor.scalar_type() == dtype, name, " has an unexpected dtype");
    TORCH_CHECK(tensor.dim() == dimensions, name, " has an unexpected rank");
    TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
}

void check_matcher_floating_tensor(const torch::Tensor& tensor, int64_t dimensions, const char* name) {
    TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
    TORCH_CHECK(at::isFloatingType(tensor.scalar_type()), name, " must use a floating dtype");
    TORCH_CHECK(tensor.dim() == dimensions, name, " has an unexpected rank");
    TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
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

void pairwise_detection_cost_cuda_out(const torch::Tensor& output, const torch::Tensor& pred_logits,
                                      const torch::Tensor& pred_boxes, const torch::Tensor& target_labels,
                                      const torch::Tensor& target_boxes, const torch::Tensor& target_offsets,
                                      const torch::Tensor& target_counts, double class_cost, double bbox_cost,
                                      double giou_cost) {
    check_matcher_tensor(output, torch::kFloat32, 3, "matcher cost output");
    check_matcher_floating_tensor(pred_logits, 3, "matcher logits");
    check_matcher_floating_tensor(pred_boxes, 3, "matcher boxes");
    check_matcher_tensor(target_labels, torch::kInt64, 1, "matcher target labels");
    check_matcher_tensor(target_boxes, torch::kFloat32, 2, "matcher target boxes");
    check_matcher_tensor(target_offsets, torch::kInt64, 1, "matcher target offsets");
    check_matcher_tensor(target_counts, torch::kInt64, 1, "matcher target counts");
    TORCH_CHECK(output.device() == pred_logits.device() && output.device() == pred_boxes.device() &&
                    output.device() == target_labels.device() && output.device() == target_boxes.device() &&
                    output.device() == target_offsets.device() && output.device() == target_counts.device(),
                "matcher CUDA tensors must share one device");
    TORCH_CHECK(pred_boxes.size(0) == pred_logits.size(0) && pred_boxes.size(1) == pred_logits.size(1) &&
                    pred_boxes.size(2) == 4,
                "matcher prediction boxes must be [batch, queries, 4]");
    TORCH_CHECK(output.size(0) >= pred_logits.size(0) && output.size(1) >= pred_logits.size(1),
                "matcher output shape does not cover predictions");
    TORCH_CHECK(target_boxes.size(0) == target_labels.size(0) && target_boxes.size(1) == 4,
                "matcher target boxes and labels disagree");
    TORCH_CHECK(target_offsets.size(0) == pred_logits.size(0) && target_counts.size(0) == pred_logits.size(0),
                "matcher target metadata does not match batch size");

    c10::cuda::CUDAGuard device_guard(output.device());
    const int64_t total = pred_logits.size(0) * pred_logits.size(1) * output.size(2);
    if (total == 0) {
        return;
    }
    const int threads = cuda_launch::kDefaultLinearThreads;
    const int blocks = cuda_launch::linear_blocks_for(static_cast<int>(total), threads);
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Half, at::ScalarType::BFloat16, pred_logits.scalar_type(), "pairwise_detection_cost_logits",
        [&] {
            using Logit = scalar_t;
            AT_DISPATCH_FLOATING_TYPES_AND2(
                at::ScalarType::Half, at::ScalarType::BFloat16, pred_boxes.scalar_type(),
                "pairwise_detection_cost_boxes", [&] {
                    using Box = scalar_t;
                    matcher_cost_kernel<Logit, Box><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
                        output.data_ptr<float>(), pred_logits.data_ptr<Logit>(), pred_boxes.data_ptr<Box>(),
                        target_labels.data_ptr<int64_t>(), target_boxes.data_ptr<float>(),
                        target_offsets.data_ptr<int64_t>(), target_counts.data_ptr<int64_t>(), pred_logits.size(0),
                        pred_logits.size(1), pred_logits.size(2), output.size(1), output.size(2),
                        static_cast<float>(class_cost), static_cast<float>(bbox_cost), static_cast<float>(giou_cost));
                });
        });
    TORCH_CHECK(cudaGetLastError() == cudaSuccess, "matcher cost CUDA kernel launch failed");
}

void pairwise_mask_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& pred_mask_logits,
                                  const torch::Tensor& target_masks, const torch::Tensor& target_offsets,
                                  const torch::Tensor& target_counts, double ce_cost, double dice_cost) {
    check_matcher_tensor(output, torch::kFloat32, 3, "matcher mask cost output");
    check_matcher_floating_tensor(pred_mask_logits, 3, "matcher sampled mask logits");
    check_matcher_tensor(target_masks, torch::kFloat32, 2, "matcher sampled target masks");
    check_matcher_tensor(target_offsets, torch::kInt64, 1, "matcher target offsets");
    check_matcher_tensor(target_counts, torch::kInt64, 1, "matcher target counts");
    TORCH_CHECK(output.device() == pred_mask_logits.device() && output.device() == target_masks.device() &&
                    output.device() == target_offsets.device() && output.device() == target_counts.device(),
                "matcher mask CUDA tensors must share one device");
    TORCH_CHECK(output.size(0) >= pred_mask_logits.size(0) && output.size(1) >= pred_mask_logits.size(1),
                "matcher mask output shape does not cover predictions");
    TORCH_CHECK(target_masks.size(1) == pred_mask_logits.size(2),
                "matcher sampled prediction and target masks disagree");

    c10::cuda::CUDAGuard device_guard(output.device());
    constexpr int threads = 256;
    constexpr int warps_per_block = threads / 32;
    const int64_t pair_count = pred_mask_logits.size(0) * pred_mask_logits.size(1) * output.size(2);
    if (pair_count == 0) {
        return;
    }
    if (pred_mask_logits.size(2) == 0) {
        output.fill_(std::numeric_limits<float>::quiet_NaN());
        return;
    }
    const int blocks = static_cast<int>((pair_count + warps_per_block - 1) / warps_per_block);
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Half, at::ScalarType::BFloat16, pred_mask_logits.scalar_type(), "pairwise_mask_cost", [&] {
            matcher_mask_cost_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(
                output.data_ptr<float>(), pred_mask_logits.data_ptr<scalar_t>(), target_masks.data_ptr<float>(),
                target_offsets.data_ptr<int64_t>(), target_counts.data_ptr<int64_t>(), pred_mask_logits.size(0),
                pred_mask_logits.size(1), output.size(1), output.size(2), pred_mask_logits.size(2),
                static_cast<float>(ce_cost), static_cast<float>(dice_cost));
        });
    TORCH_CHECK(cudaGetLastError() == cudaSuccess, "matcher mask cost CUDA kernel launch failed");
}

}  // namespace mmltk::rfdetr
