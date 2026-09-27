#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <cmath>
#include <cstdint>
#include "src/frameworks/gpu/cuda/cuda_launch.cuh"
#include <limits>
#include "box_iou.h"
#include "box_geometry.cuh"
namespace mmltk::backend::ml::ops {
namespace {
template <typename T>
__global__ void box_iou_kernel(const T* boxes1, const T* boxes2, T* iou, const int pair_count, const int second_count) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 if (index >= pair_count) return;
 iou[index] = box_geometry::pair(boxes1 + (index / second_count) * 4, boxes2 + (index % second_count) * 4).iou();
}
template <typename T>
__global__ void generalized_box_iou_kernel(const T* boxes1, const T* boxes2, T* giou, const int pair_count, const int second_count) {
 const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
 if (index >= pair_count) return;
 giou[index] = box_geometry::pair(boxes1 + (index / second_count) * 4, boxes2 + (index % second_count) * 4).giou();
}
enum class BoxIouKind : std::uint8_t {
 Standard,
 Generalized,
};
void validate_box_tensor(const torch::Tensor& tensor, const char* name) {
 TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
 TORCH_CHECK(tensor.scalar_type() == torch::kFloat32 || tensor.scalar_type() == torch::kFloat64, name, " must use float32 or float64");
 TORCH_CHECK(tensor.dim() == 2 && tensor.size(1) == 4, name, " must have shape [count, 4]");
 TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
 TORCH_CHECK(tensor.size(0) <= std::numeric_limits<int>::max(), name, " count exceeds CUDA kernel limit");
}
torch::Tensor pairwise_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2, const BoxIouKind kind) {
 validate_box_tensor(boxes1, "boxes1");
 validate_box_tensor(boxes2, "boxes2");
 TORCH_CHECK(boxes1.device() == boxes2.device(), "box IoU tensors must share one CUDA device");
 TORCH_CHECK(boxes1.scalar_type() == boxes2.scalar_type(), "box IoU tensors must share one dtype");
 TORCH_CHECK(boxes2.size(0) == 0 || boxes1.size(0) <= std::numeric_limits<int>::max() / boxes2.size(0), "box pair count exceeds CUDA kernel limit");
 const std::int64_t pair_count = boxes1.size(0) * boxes2.size(0);
 TORCH_CHECK(pair_count <= std::numeric_limits<int>::max(), "box pair count exceeds CUDA kernel limit");
 c10::cuda::CUDAGuard device_guard(boxes1.device());
 auto result = torch::empty({boxes1.size(0), boxes2.size(0)}, boxes1.options());
 if (pair_count == 0) { return result; }
 const int second_count = static_cast<int>(boxes2.size(0));
 const int threads = mmltk::frameworks::gpu::launch::kDefaultLinearThreads;
 const int blocks = mmltk::frameworks::gpu::launch::linear_blocks_for(static_cast<int>(pair_count), threads);
 const cudaStream_t stream = at::cuda::getCurrentCUDAStream();
 AT_DISPATCH_FLOATING_TYPES(boxes1.scalar_type(), "pairwise_box_iou_cuda", [&] {
  // NOLINTNEXTLINE(bugprone-branch-clone): each branch launches a different CUDA kernel.
  if (kind == BoxIouKind::Generalized) {
   generalized_box_iou_kernel<scalar_t>
    <<<blocks, threads, 0, stream>>>(boxes1.data_ptr<scalar_t>(), boxes2.data_ptr<scalar_t>(), result.data_ptr<scalar_t>(), static_cast<int>(pair_count), second_count);
  } else {
   box_iou_kernel<scalar_t><<<blocks, threads, 0, stream>>>(boxes1.data_ptr<scalar_t>(), boxes2.data_ptr<scalar_t>(), result.data_ptr<scalar_t>(), static_cast<int>(pair_count), second_count);
  }
 });
 TORCH_CHECK(cudaGetLastError() == cudaSuccess, "box IoU CUDA kernel launch failed");
 return result;
}
}  // namespace
torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) { return pairwise_box_iou_cuda(boxes1, boxes2, BoxIouKind::Standard); }
torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) { return pairwise_box_iou_cuda(boxes1, boxes2, BoxIouKind::Generalized); }
}  // namespace mmltk::backend::ml::ops
