#pragma once
#include <torch/types.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
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
 std::vector<std::string> initialized_names;
};
class ModelStateAdmissionError final : public std::runtime_error {
public:
 ModelStateAdmissionError(std::string message, ModelStateLoadSummary summary) : std::runtime_error(std::move(message)), summary_(std::move(summary)) {}
 [[nodiscard]] const ModelStateLoadSummary& summary() const noexcept { return summary_; }

private:
 ModelStateLoadSummary summary_;
};
namespace detail {
inline constexpr std::string_view kQueryFeatureWeight = "query_feat.weight", kReferencePointWeight = "refpoint_embed.weight";
struct NormalizedModelStateCandidate {
 ModelStateLoadSummary summary;
 std::vector<torch::Tensor> destinations;
 std::vector<torch::Tensor> tensors;
};
enum class NormalizedModelStateAdmission {
 Exact,
 // Complete inference state; native supervision is optional and not deployed.
 RequiredCore,
 PartialExact,
 FreshTransfer,
 PartialFreshTransfer,
};
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
