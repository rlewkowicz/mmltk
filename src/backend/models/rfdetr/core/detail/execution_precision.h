#pragma once
#include <ATen/autocast_mode.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <torch/torch.h>
#include "src/backend/ml/cuda/torch_autocast_scope.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] inline at::ScalarType resolve_cuda_autocast_dtype() {
    const auto* properties = at::cuda::getCurrentDeviceProperties();
    TORCH_CHECK(properties != nullptr, "CUDA autocast requires a valid CUDA device");
    TORCH_CHECK(properties->major >= 7, "RF-DETR CUDA autocast requires compute capability 7.0 or newer");
    return properties->major >= 8 ? torch::kBFloat16 : torch::kFloat16;
}
[[nodiscard]] inline at::ScalarType resolve_cuda_autocast_dtype(const int device_id) {
    c10::cuda::CUDAGuard device_guard(static_cast<c10::DeviceIndex>(device_id));
    return resolve_cuda_autocast_dtype();
}
}  // namespace mmltk::backend::models::rfdetr
