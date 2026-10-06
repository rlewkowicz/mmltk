#pragma once
#include "tensor_trace.h"
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <inplace_vector>
#include <string>
namespace mmltk::backend::models::rfdetr::detail {
// Lane-local tensor execution. Each mode retains exactly one admitted signature.
// Ordinary owners retain all policy and registered parameter names.
class SelectiveTensorRegion final {
public:
 using Tensors = std::inplace_vector<torch::Tensor, 2>;
 using Operation = std::function_ref<Tensors(const Tensors&)>;
 explicit SelectiveTensorRegion(std::string name, bool dynamic_batch_queries = false);
 ~SelectiveTensorRegion();
 SelectiveTensorRegion(const SelectiveTensorRegion&) = delete;
 SelectiveTensorRegion& operator=(const SelectiveTensorRegion&) = delete;
 void prepare(bool training, bool enabled, std::int64_t batch_size);
 void invalidate();
 Tensors invoke(bool training, const Tensors& inputs, std::initializer_list<torch::nn::Module*> owners, Operation ordinary);
 [[nodiscard]] const void* identity(bool training) const noexcept;

private:
 TensorTrace trace_;
};
}  // namespace mmltk::backend::models::rfdetr::detail
