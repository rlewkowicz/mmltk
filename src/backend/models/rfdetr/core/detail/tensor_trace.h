#pragma once
#include <torch/torch.h>
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
namespace torch::jit { struct Graph; }
namespace mmltk::backend::models::rfdetr::detail {
// Shared lane-local capture mechanics. Callers own output schemas and storage;
// each mode retains one admitted graph over live registered owner tensors.
class TensorTrace final {
public:
 using Operation = std::function_ref<c10::IValue(std::span<const torch::Tensor>)>;
 explicit TensorTrace(std::string name, bool dynamic_batch_queries = false);
 ~TensorTrace();
 TensorTrace(const TensorTrace&) = delete;
 TensorTrace& operator=(const TensorTrace&) = delete;
 void prepare(bool training, bool enabled, std::int64_t batch_size);
 void invalidate();
 struct Result { c10::IValue output; bool recorded = false; };
 Result invoke(bool training, std::span<const torch::Tensor> inputs, std::initializer_list<torch::nn::Module*> owners, Operation ordinary, std::uint64_t specialization = 0);
 [[nodiscard]] bool enabled(bool training) const noexcept;
 [[nodiscard]] const void* identity(bool training) const noexcept;
 [[nodiscard]] std::shared_ptr<torch::jit::Graph> graph(bool training) const;
private:
 struct State;
 struct Slot {
  bool enabled = false;
  bool armed = false;
  std::int64_t batch_size = 0;
  // Pinned Torch 4fdf77b940 DifferentiableGraphBackward owns its executor by value, with
  // shared implementation custody; native autograd nodes own saved tensors.
  // Retiring this forward slot therefore preserves outstanding backwards.
  std::unique_ptr<State> state;
 };
 std::string name_;
 bool dynamic_batch_queries_;
 std::array<Slot, 2> slots_;
};
}  // namespace mmltk::backend::models::rfdetr::detail
