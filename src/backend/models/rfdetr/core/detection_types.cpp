
#include "src/backend/models/rfdetr/core/detection_types.h"
namespace mmltk::backend::models::rfdetr {
void assert_inference_output_dtype(const OutputLayer& output, const at::ScalarType expected_dtype, const char* context) {
 const auto& pred_logits = output.pred_logits;
 const auto& pred_boxes = output.pred_boxes;
 if (!pred_logits.defined() || !pred_boxes.defined()) { throw std::runtime_error(std::string(context) + " returned undefined detection outputs"); }
 // GroupPose adds its refined keypoint class contribution to the classifier.
 // FP32 reference coordinates promote the joint packet and this sum under AMP.
 const bool promoted_keypoints = expected_dtype != at::kFloat && output.pred_keypoints && output.pred_keypoints->defined() && output.pred_keypoints->scalar_type() == at::kFloat;
 if (pred_logits.scalar_type() != expected_dtype && !(promoted_keypoints && pred_logits.scalar_type() == at::kFloat)) {
  throw std::runtime_error(
   std::string(context) + " returned logits of type " + c10::toString(pred_logits.scalar_type()) + "; expected " + c10::toString(expected_dtype) + " or FP32 keypoint promotion");
 }
 const bool valid_box_dtype = pred_boxes.scalar_type() == expected_dtype || (expected_dtype != at::kFloat && pred_boxes.scalar_type() == at::kFloat);
 if (!valid_box_dtype) {
  throw std::runtime_error(std::string(context) + " returned boxes of type " + c10::toString(pred_boxes.scalar_type()) + "; expected " + c10::toString(expected_dtype) + " or FP32 refinement");
 }
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
