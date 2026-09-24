#pragma once
#include <torch/script.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <utility>
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
template <size_t Arity, typename TraceFn, typename... Tensors>
void ensure_loss_trace(TracedLossOp<Arity>& cache, const char* class_name, TraceFn&& fn, const Tensors&... tensors) {
 static_assert(sizeof...(Tensors) == Arity);
 if (cache.matches(tensors...)) { return; }
 auto cu = std::make_shared<torch::jit::CompilationUnit>();
 auto cls = torch::jit::ClassType::create(class_name, cu, true);
 TracedLossOp<Arity> candidate;
 candidate.module = torch::jit::Module(cu, cls);
 auto trace_res = torch::jit::tracer::trace(
  {tensors.detach().contiguous()...},
  [&](
   torch::jit::Stack args) -> torch::jit::Stack { return [&]<size_t... I>(std::index_sequence<I...>) -> torch::jit::Stack { return {fn(args[I].toTensor()...)}; }(std::make_index_sequence<Arity>{}); },
  [](const torch::autograd::Variable&) { return ""; }, false, false, &candidate.module);
 candidate.module.type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));
 candidate.record_signature(tensors...);
 cache = std::move(candidate);
}
template <typename TraceFn>
void ensure_parametric_binary_loss_trace(
 TracedParametricBinaryLossOp& cache, const char* class_name, const torch::Tensor& input, const torch::Tensor& target, double alpha, double gamma, TraceFn&& fn) {
 if (cache.matches(input, target) && cache.alpha == alpha && cache.gamma == gamma) return;
 TracedParametricBinaryLossOp candidate;
 ensure_loss_trace(candidate, class_name, std::forward<TraceFn>(fn), input, target);
 candidate.alpha = alpha;
 candidate.gamma = gamma;
 cache = std::move(candidate);
}
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
