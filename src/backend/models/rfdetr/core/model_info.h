#pragma once
#include <cstddef>
#include <algorithm>
#include <span>
#include <cstdint>
#include <sstream>
#include <optional>
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include <string>
#include <vector>
namespace mmltk::backend::models::rfdetr {
struct TensorInfo {
 std::string name;
 std::vector<int64_t> shape;
 std::string dtype;
 RfdetrOutputRole role = RfdetrOutputRole::Unspecified;
};
struct ModelInfo {
 std::string backend;
 std::string model_path;
 TensorInfo input;
 std::vector<TensorInfo> outputs;
 int64_t num_queries = 0;
 int64_t automatic_num_queries_cap = 0;
 int64_t num_classes = 0;
 bool has_masks = false;
 std::optional<ModelClassLayout> class_layout;
 std::optional<RfdetrOutputContract> output_contract;
};
inline void apply_rfdetr_output_roles(ModelInfo& info, std::span<const RfdetrNamedOutputRole> roles) {
 if (roles.size() > kMaximumNamedOutputRoles) throw std::invalid_argument("too many RF-DETR output roles");
 for (const auto& declared : roles) {
  auto output = std::ranges::find(info.outputs, declared.name, &TensorInfo::name);
  if (output == info.outputs.end() || declared.role == RfdetrOutputRole::Unspecified || (output->role != RfdetrOutputRole::Unspecified && output->role != declared.role))
   throw std::invalid_argument("RF-DETR output role metadata disagrees with artifact");
  output->role = declared.role;
 }
}
[[nodiscard]] inline std::vector<RfdetrNamedOutputRole> rfdetr_output_roles(const ModelInfo& info) {
 std::vector<RfdetrNamedOutputRole> result;
 result.reserve(info.outputs.size());
 for (const auto& output : info.outputs) result.push_back({output.name, output.role});
 return result;
}
[[nodiscard]] inline std::string format_shape(const std::vector<int64_t>& shape) {
 std::ostringstream stream;
 stream << "[";
 for (std::size_t index = 0; index < shape.size(); ++index) {
  if (index > 0) { stream << ", "; }
  stream << shape[index];
 }
 stream << "]";
 return stream.str();
}
struct RfdetrOutputRoles final {
 std::size_t logits{}, boxes{};
 std::optional<std::size_t> masks, keypoints;
};
class ResolvedClassLayout;
void validate_rfdetr_output_contract(const RfdetrOutputContract&, const ResolvedClassLayout* layout = nullptr);
[[nodiscard]] RfdetrOutputRoles validate_rfdetr_output_layout(ModelInfo& info);
}  // namespace mmltk::backend::models::rfdetr
