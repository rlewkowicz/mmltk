#include "src/backend/models/rfdetr/core/detail/class_tensor_axes.h"
#include <torch/nn/module.h>
namespace mmltk::backend::models::rfdetr::detail {
void ClassTensorFamily::AppendParameters(std::vector<ClassTensorAxis>& axes, const torch::nn::Module& module) const {
 const auto owner_prefix = prefix();
 for (const auto& parameter : module.named_parameters(true)) {
  const auto name = owner_prefix + parameter.key();
  if (const auto axis = Match(name)) axes.push_back({name, axis->dimension, axis->coordinates});
 }
}
}  // namespace mmltk::backend::models::rfdetr::detail
