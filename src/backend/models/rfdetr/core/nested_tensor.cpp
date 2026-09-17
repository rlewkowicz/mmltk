#include "src/backend/models/rfdetr/core/model.h"
namespace mmltk::backend::models::rfdetr {
NestedTensor NestedTensor::to(const torch::Device& device) const {
    NestedTensor result{tensors.to(device), {}};
    if (mask.defined()) result.mask = mask.to(device);
    return result;
}
NestedTensor NestedTensor::pin_memory() const {
    NestedTensor result{tensors.pin_memory(), {}};
    if (mask.defined()) result.mask = mask.pin_memory();
    return result;
}
std::pair<torch::Tensor, torch::Tensor> NestedTensor::decompose() const { return {tensors, mask}; }
NestedTensor nested_tensor_from_tensor_list(const std::vector<torch::Tensor>& tensor_list) {
    if (tensor_list.empty() || tensor_list.front().dim() != 3) throw std::runtime_error("nested_tensor_from_tensor_list requires nonempty CHW tensors");
    std::vector<std::int64_t> maximum(tensor_list.front().sizes().begin(), tensor_list.front().sizes().end());
    for (std::size_t index = 1U; index < tensor_list.size(); ++index) {
        const auto& tensor = tensor_list[index];
        if (tensor.dim() != 3) throw std::runtime_error("nested_tensor_from_tensor_list only supports CHW tensors");
        for (std::int64_t dimension = 0; dimension < tensor.dim(); ++dimension) {
            maximum[static_cast<std::size_t>(dimension)] = std::max(maximum[static_cast<std::size_t>(dimension)], tensor.size(dimension));
        }
    }
    const auto batch = static_cast<std::int64_t>(tensor_list.size());
    const auto options = tensor_list.front().options();
    auto padded = torch::zeros({batch, maximum[0], maximum[1], maximum[2]}, options);
    auto mask = torch::ones({batch, maximum[1], maximum[2]}, torch::TensorOptions().dtype(torch::kBool).device(options.device()));
    for (std::int64_t index = 0; index < batch; ++index) {
        const auto& tensor = tensor_list[static_cast<std::size_t>(index)];
        padded[index].slice(0, 0, tensor.size(0)).slice(1, 0, tensor.size(1)).slice(2, 0, tensor.size(2)).copy_(tensor);
        mask[index].slice(0, 0, tensor.size(1)).slice(1, 0, tensor.size(2)).fill_(false);
    }
    return {std::move(padded), std::move(mask)};
}
}  // namespace mmltk::backend::models::rfdetr
