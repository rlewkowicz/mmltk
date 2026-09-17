#pragma once
#include <torch/types.h>
#include <string>
#include <vector>
namespace mmltk::backend::models::rfdetr {
struct NormalizedModelStateEntry {
    std::string name;
    torch::Tensor tensor;
};
struct ModelStateLoadSummary {
    std::vector<std::string> loaded_names;
    std::vector<std::string> missing_names;
    std::vector<std::string> unexpected_names;
    std::vector<std::string> incompatible_names;
};
namespace detail {
struct NormalizedModelStateCandidate {
    ModelStateLoadSummary summary;
    std::vector<torch::Tensor> destinations;
    std::vector<torch::Tensor> tensors;
};
enum class NormalizedModelStateAdmission {
    Exact,
    PartialExact,
    FreshTransfer,
    PartialFreshTransfer,
};
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
