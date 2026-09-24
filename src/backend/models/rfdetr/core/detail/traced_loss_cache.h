#pragma once
#include <ATen/autocast_mode.h>
#include <torch/script.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
namespace mmltk::backend::models::rfdetr {
// One lane-owned graph per operator. Tensor extents remain dynamic; coefficients
// and the effective CUDA dispatch policy belong to the recorded graph.
template <size_t Arity, size_t Coefficients = 0>
class TracedLossOp final {
public:
 template <typename TraceFn, typename... Tensors>
 torch::Tensor invoke(const char* class_name, TraceFn&& fn, const Tensors&... tensors) requires (Coefficients == 0 && sizeof...(Tensors) == Arity) {
  return invoke(class_name, std::array<double, 0>{}, std::forward<TraceFn>(fn), tensors...);
 }
 template <typename TraceFn, typename... Tensors>
 torch::Tensor invoke(const char* class_name, const std::array<double, Coefficients>& coefficients, TraceFn&& fn, const Tensors&... tensors) {
  static_assert(sizeof...(Tensors) == Arity);
  const Signature signature{{tensors.device().type()...}, {tensors.scalar_type()...}, {tensors.dim()...}, coefficients,
                            (... || tensors.is_cuda())};
  if (!state_ || state_->signature != signature) {
   auto cu = std::make_shared<torch::jit::CompilationUnit>();
   auto cls = torch::jit::ClassType::create(class_name, cu, true);
   auto candidate = std::make_unique<State>(torch::jit::Module(cu, cls), signature);
   auto trace_res = torch::jit::tracer::trace(
    {tensors.detach().contiguous()...},
    [&](torch::jit::Stack args) -> torch::jit::Stack {
     return [&]<size_t... I>(std::index_sequence<I...>) -> torch::jit::Stack { return {fn(args[I].toTensor()...)}; }(std::make_index_sequence<Arity>{});
    },
    [](const torch::autograd::Variable&) { return ""; }, false, false, &candidate->module);
   candidate->module.type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));
   state_ = std::move(candidate);
  }
  return state_->module.forward({tensors...}).toTensor();
 }
 // Opaque observation only: callers cannot mutate or independently execute a module.
 [[nodiscard]] const void* identity() const noexcept { return state_.get(); }

private:
 struct Signature {
  std::array<c10::DeviceType, Arity> device_types;
  std::array<c10::ScalarType, Arity> dtypes;
  std::array<int64_t, Arity> dims;
  std::array<double, Coefficients> coefficients;
  bool cuda_autocast;
  c10::ScalarType autocast_dtype;
  Signature(std::array<c10::DeviceType, Arity> devices, std::array<c10::ScalarType, Arity> types,
            std::array<int64_t, Arity> ranks, std::array<double, Coefficients> parameters, bool uses_cuda)
      : device_types(devices), dtypes(types), dims(ranks), coefficients(parameters),
        cuda_autocast(uses_cuda && at::autocast::is_autocast_enabled(at::kCUDA)),
        autocast_dtype(cuda_autocast ? at::autocast::get_autocast_dtype(at::kCUDA) : c10::ScalarType::Undefined) {}
  bool operator==(const Signature&) const = default;
 };
 struct State {
  torch::jit::Module module;
  Signature signature;
 };
 std::unique_ptr<State> state_;
};
using TracedBinaryLossOp = TracedLossOp<2>;
using TracedTernaryLossOp = TracedLossOp<3>;
using TracedParametricBinaryLossOp = TracedLossOp<2, 2>;
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
