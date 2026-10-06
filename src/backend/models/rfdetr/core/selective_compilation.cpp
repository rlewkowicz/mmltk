#include "detail/selective_compilation.h"
#include <utility>
#include <vector>
namespace mmltk::backend::models::rfdetr::detail {
SelectiveTensorRegion::SelectiveTensorRegion(std::string name, bool dynamic) : trace_(std::move(name), dynamic) {}
SelectiveTensorRegion::~SelectiveTensorRegion() = default;
void SelectiveTensorRegion::prepare(bool training, bool enabled, std::int64_t batch_size) { trace_.prepare(training, enabled, batch_size); }
void SelectiveTensorRegion::invalidate() { trace_.invalidate(); }
const void* SelectiveTensorRegion::identity(bool training) const noexcept { return trace_.identity(training); }
SelectiveTensorRegion::Tensors SelectiveTensorRegion::invoke(bool training, const Tensors& inputs, std::initializer_list<torch::nn::Module*> owners, Operation ordinary) {
 if (!trace_.enabled(training)) return ordinary(inputs);
 const auto output = trace_.invoke(training, {inputs.data(), inputs.size()}, owners, [&](std::span<const torch::Tensor> arguments) {
  Tensors tensors;
  for (const auto& argument : arguments) tensors.push_back(argument);
  std::vector<c10::IValue> values;
  for (auto& value : ordinary(tensors)) values.emplace_back(std::move(value));
  return c10::IValue(c10::ivalue::Tuple::create(std::move(values)));
 });
 Tensors result;
 const auto& elements = output.output.toTupleRef().elements();
 TORCH_CHECK(!elements.empty() && elements.size() <= Tensors::capacity(), "selective tensor region requires one or two outputs");
 for (const auto& item : elements) result.push_back(item.toTensor());
 return result;
}
}  // namespace mmltk::backend::models::rfdetr::detail
