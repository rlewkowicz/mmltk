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
};
inline void apply_rfdetr_output_roles(ModelInfo& info, std::span<const RfdetrNamedOutputRole> roles) {
 if (roles.size() > 3) throw std::invalid_argument("too many RF-DETR output roles");
 for (const auto& declared : roles) {
  auto output = std::ranges::find(info.outputs, declared.name, &TensorInfo::name);
  if (output == info.outputs.end() || declared.role == RfdetrOutputRole::Unspecified ||
      (output->role != RfdetrOutputRole::Unspecified && output->role != declared.role))
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
 std::optional<std::size_t> masks;
};
[[nodiscard]] inline RfdetrOutputRoles validate_rfdetr_output_layout(ModelInfo& info) {
 std::optional<std::size_t> logits, boxes, masks;
 for (std::size_t index = 0; index < info.outputs.size(); ++index) {
  auto& output = info.outputs[index];
  auto role = output.role;
  const auto canonical = output.name == "pred_logits"   ? RfdetrOutputRole::Logits
                         : output.name == "pred_boxes"  ? RfdetrOutputRole::Boxes
                          : output.name == "pred_masks" ? RfdetrOutputRole::Masks
                                                        : RfdetrOutputRole::Unspecified;
  if (role == RfdetrOutputRole::Unspecified)
   role = canonical;
  else if (canonical != RfdetrOutputRole::Unspecified && canonical != role)
   throw std::invalid_argument("RF-DETR output role contradicts canonical tensor name");
  auto* selected = role == RfdetrOutputRole::Logits ? &logits : role == RfdetrOutputRole::Boxes ? &boxes : role == RfdetrOutputRole::Masks ? &masks : nullptr;
  if (!selected || *selected) throw std::invalid_argument("ambiguous RF-DETR output roles");
  *selected = index;
  output.role = role;
  const auto rank = role == RfdetrOutputRole::Masks ? 4U : 3U;
  if (output.shape.size() != rank || output.shape[1] <= 0 || output.shape[2] <= 0 || (role == RfdetrOutputRole::Boxes && output.shape[2] != 4) ||
      (role == RfdetrOutputRole::Masks && output.shape[3] <= 0))
   throw std::invalid_argument("invalid RF-DETR output shape");
 }
 if (!logits || !boxes || info.outputs.size() != (masks ? 3U : 2U)) throw std::invalid_argument("RF-DETR requires declared logits and boxes outputs");
 const auto& shape = info.outputs[*logits].shape;
 for (const auto& output : info.outputs)
  if (output.shape[1] != shape[1] || (output.shape[0] > 0 && shape[0] > 0 && output.shape[0] != shape[0]))
   throw std::invalid_argument("RF-DETR output batch/query axes disagree");
 info.num_queries = shape[1];
 info.num_classes = shape[2];
 info.has_masks = masks.has_value();
 return {*logits, *boxes, masks};
}
}  // namespace mmltk::backend::models::rfdetr
