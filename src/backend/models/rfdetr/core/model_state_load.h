#pragma once
#include <torch/types.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <functional>
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
 // Optional source custody check, invoked before mutation and inside rollback
 // protection after all copies. Its closure retains any admitted source owner.
 std::function<void()> validate_source;
};
inline bool stage_model_state_tensor(NormalizedModelStateCandidate& candidate, const std::string& name, const torch::Tensor& source, const torch::Tensor& destination) {
 if (!source.defined() || source.sizes() != destination.sizes()) {
  candidate.summary.incompatible_names.push_back(name);
  return false;
 }
 candidate.destinations.push_back(destination);
 candidate.tensors.push_back(source.to(destination.device(), destination.scalar_type()));
 candidate.summary.loaded_names.push_back(name);
 return true;
}
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
