#pragma once
#include <torch/torch.h>
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>
namespace mmltk::backend::models::rfdetr::detail {
// Lane-local tensor execution. Each mode retains exactly one admitted signature.
// Ordinary owners retain all policy and registered parameter names.
class SelectiveTensorRegion final {
public:
 using Tensors = std::vector<torch::Tensor>;
 using Operation = std::function<Tensors(const Tensors&)>;
 explicit SelectiveTensorRegion(std::string name, bool dynamic_batch_queries = false);
 ~SelectiveTensorRegion();
 SelectiveTensorRegion(const SelectiveTensorRegion&) = delete;
 SelectiveTensorRegion& operator=(const SelectiveTensorRegion&) = delete;
 void prepare(bool training, bool enabled, std::int64_t batch_size);
 void invalidate();
 Tensors invoke(bool training, const Tensors& inputs, std::initializer_list<torch::nn::Module*> owners, const Operation& ordinary);
 [[nodiscard]] const void* identity(bool training) const noexcept;
private:
 struct State;
 struct Slot {
  bool enabled = false;
  bool armed = false;
  std::int64_t batch_size = 0;
  std::unique_ptr<State> state;
 };
 std::string name_;
 bool dynamic_batch_queries_;
 std::array<Slot, 2> slots_;
};
}
