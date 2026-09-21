#pragma once
#include <torch/script.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace mmltk::backend::models::rfdetr {
// Cached jit trace of a loss op taking Arity tensor arguments, keyed on each argument's device type, dtype,
// and rank. The per-slot signature fields are only read once initialized is true.
template <size_t Arity>
struct TracedLossOp {
 torch::jit::Module module;
 std::array<c10::DeviceType, Arity> device_types{};
 std::array<c10::ScalarType, Arity> dtypes{};
 std::array<int64_t, Arity> dims{};
 bool initialized = false;
 template <typename... Tensors>
 [[nodiscard]] bool matches(const Tensors&... tensors) const {
  static_assert(sizeof...(Tensors) == Arity);
  if (!initialized) { return false; }
  size_t index = 0;
  return (... && matches_slot(index++, tensors));
 }
 template <typename... Tensors>
 void record_signature(const Tensors&... tensors) {
  static_assert(sizeof...(Tensors) == Arity);
  size_t index = 0;
  (record_slot(index++, tensors), ...);
  initialized = true;
 }

private:
 [[nodiscard]] bool matches_slot(const size_t index, const torch::Tensor& tensor) const {
  return device_types[index] == tensor.device().type() && dtypes[index] == tensor.scalar_type() && dims[index] == tensor.dim();
 }
 void record_slot(const size_t index, const torch::Tensor& tensor) {
  device_types[index] = tensor.device().type();
  dtypes[index] = tensor.scalar_type();
  dims[index] = tensor.dim();
 }
};
using TracedBinaryLossOp = TracedLossOp<2>;
using TracedTernaryLossOp = TracedLossOp<3>;
struct TracedParametricBinaryLossOp : TracedBinaryLossOp {
 double alpha = std::numeric_limits<double>::quiet_NaN();
 double gamma = std::numeric_limits<double>::quiet_NaN();
};
struct TracedLossOpCache {
 TracedBinaryLossOp sigmoid_ce;
 TracedBinaryLossOp dice;
 TracedBinaryLossOp batch_dice;
 TracedBinaryLossOp batch_sigmoid_ce;
 TracedParametricBinaryLossOp sigmoid_focal;
 TracedParametricBinaryLossOp sigmoid_varifocal;
 TracedParametricBinaryLossOp position_supervised;
 TracedTernaryLossOp ia_bce;
};
}  // namespace mmltk::backend::models::rfdetr
