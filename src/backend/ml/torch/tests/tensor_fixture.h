#pragma once
#include "catch_support.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <ATen/Context.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAFunctions.h>
#include <inplace_vector>
#include <initializer_list>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <string>
#include <unordered_map>
#include <torch/nn/module.h>
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
// Reference operands own storage and a fresh autograd leaf independently of
// the implementation under test, while preserving the exact input values.
[[nodiscard]] inline torch::Tensor independent_leaf(const torch::Tensor& source, bool requires_grad = true) { return source.detach().clone().set_requires_grad(requires_grad); }
[[nodiscard]] inline std::unordered_map<std::string, torch::Tensor> independent_parameters(const torch::nn::Module& module) {
 std::unordered_map<std::string, torch::Tensor> result;
 for (const auto& parameter : module.named_parameters()) result.emplace(parameter.key(), independent_leaf(parameter.value()));
 return result;
}
inline void affine_parameters(torch::nn::Module& module, double scale, double shift) {
 torch::NoGradGuard guard;
 for (const auto& parameter : module.named_parameters()) parameter.value().copy_((torch::arange(parameter.value().numel(), parameter.value().options()) * scale + shift).reshape_as(parameter.value()));
}
inline void backward_squared(std::initializer_list<torch::Tensor> outputs) {
 for (const auto& output : outputs) output.square().sum().backward();
}
inline void require_gradients_close(std::initializer_list<std::pair<torch::Tensor, torch::Tensor>> operands, double relative, double absolute) {
 std::size_t index = 0;
 for (const auto& [actual, expected] : operands) {
  CAPTURE(index);
  REQUIRE(actual.grad().defined());
  REQUIRE(expected.grad().defined());
  REQUIRE(torch::allclose(actual.grad(), expected.grad(), relative, absolute));
  ++index;
 }
}
inline void require_nonfinite_pattern(const torch::Tensor& actual, const torch::Tensor& expected) {
 REQUIRE(torch::equal(torch::isfinite(actual), torch::isfinite(expected)));
 REQUIRE(torch::equal(torch::isnan(actual), torch::isnan(expected)));
}
}  // namespace mmltk::backend::ml::testsupport
