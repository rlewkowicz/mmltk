#pragma once
#include "catch_support.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <ATen/Context.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAFunctions.h>
#include <inplace_vector>
#include <initializer_list>
#include <cstdint>
#include <utility>
namespace mmltk::backend::ml::testsupport {
class FullMatrixPrecision final {
public:
 FullMatrixPrecision() {
  at::globalContext().setAllowTF32CuDNN(false);
  at::globalContext().setAllowTF32CuBLAS(false);
 }
 ~FullMatrixPrecision() {
  at::globalContext().setAllowTF32CuDNN(cudnn_);
  at::globalContext().setAllowTF32CuBLAS(cublas_);
 }
 FullMatrixPrecision(const FullMatrixPrecision&) = delete;
 FullMatrixPrecision& operator=(const FullMatrixPrecision&) = delete;

private:
 bool cudnn_ = at::globalContext().allowTF32CuDNN();
 bool cublas_ = at::globalContext().allowTF32CuBLAS();
};
[[nodiscard]] inline std::inplace_vector<torch::Device, 2> available_devices(c10::DeviceIndex cuda_index = -1) {
 std::inplace_vector<torch::Device, 2> result{torch::Device(torch::kCPU)};
 if (mmltk::testsupport::checked_cuda_device_count()) result.emplace_back(torch::kCUDA, cuda_index < 0 ? c10::cuda::current_device() : cuda_index);
 return result;
}
[[nodiscard]] inline std::inplace_vector<std::pair<torch::Device, c10::ScalarType>, 4> available_amp_precisions() {
 std::inplace_vector<std::pair<torch::Device, c10::ScalarType>, 4> result;
 for (const auto device : available_devices()) {
  result.emplace_back(device, torch::kFloat32);
  if (device.is_cuda()) {
   result.emplace_back(device, torch::kFloat16);
   if (at::cuda::getCurrentDeviceProperties()->major >= 8) result.emplace_back(device, torch::kBFloat16);
  }
 }
 return result;
}
inline void require_zero_gradients(std::initializer_list<torch::Tensor> operands) {
 for (const auto& operand : operands) {
  REQUIRE(operand.grad().defined());
  REQUIRE(operand.grad().count_nonzero().item<int64_t>() == 0);
 }
}
}  // namespace mmltk::backend::ml::testsupport
