
#include "src/backend/models/rfdetr/core/detection_types.h"
namespace mmltk::backend::models::rfdetr {
void assert_inference_output_dtype(const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes, const at::ScalarType expected_dtype, const char* context) {
 if (!pred_logits.defined() || !pred_boxes.defined()) { throw std::runtime_error(std::string(context) + " returned undefined detection outputs"); }
 if (pred_logits.scalar_type() != expected_dtype) { throw std::runtime_error(std::string(context) + " returned logits with the wrong element type"); }
 const bool valid_box_dtype = pred_boxes.scalar_type() == expected_dtype || (expected_dtype != at::kFloat && pred_boxes.scalar_type() == at::kFloat);
 if (!valid_box_dtype) { throw std::runtime_error(std::string(context) + " returned boxes with the wrong element type"); }
}
void populate_default_detection_weights(DetectionConfig& config) {
 config.weights.clear();
 CriterionSchema::visit_coefficients<DetectionConfig>([&]<CriterionTerm Term, std::meta::info Coefficient>(CriterionDomain domain) {
  if (domain == CriterionDomain::Masks && !config.include_masks) return;
  if (domain == CriterionDomain::Keypoints && config.keypoints_per_class.empty()) return;
  config.weights.push_back({{Term}, config.[:Coefficient:]});
 });
 const auto base_weights = config.weights;
 for (std::int64_t layer_index = 0; config.aux_loss && layer_index < config.dec_layers - 1; ++layer_index) {
  for (const auto& item : base_weights) { config.weights.push_back({{item.key.term, CriterionLayer::auxiliary(static_cast<std::size_t>(layer_index))}, item.coefficient}); }
 }
 if (config.two_stage && config.dec_layers > 0) {
  for (const auto& item : base_weights) { config.weights.push_back({{item.key.term, CriterionLayer::encoder()}, item.coefficient}); }
 }
}
}  // namespace mmltk::backend::models::rfdetr
