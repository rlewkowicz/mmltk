#include "model_info.h"
#include "keypoint_layout.h"
namespace mmltk::backend::models::rfdetr {
void validate_rfdetr_output_contract(const RfdetrOutputContract& contract, const ResolvedClassLayout* layout) {
 if (contract.version != 1U || !model_task_name(contract.task)) throw std::invalid_argument("invalid RF-DETR output contract version/task");
 const KeypointTensorLayout joints(contract.keypoints_per_class);
 if (contract.task == ModelTask::Keypoints) {
  if (!joints.compact_count()) throw std::invalid_argument("keypoint artifact has no active joint schema");
  if (layout) {
   if (!layout->semantic()) throw std::invalid_argument("keypoint artifact requires named class and joint schema metadata");
   joints.require_class_layout(*layout);
  }
 } else if (!contract.keypoints_per_class.empty()) throw std::invalid_argument("non-keypoint artifact declares joint slots");
}
RfdetrOutputRoles validate_rfdetr_output_layout(ModelInfo& info) {
 std::optional<std::size_t> logits, boxes, masks, keypoints;
 for (std::size_t index = 0; index < info.outputs.size(); ++index) {
  auto& output = info.outputs[index];
  auto role = output.role;
  const auto canonical = output.name == "pred_logits"   ? RfdetrOutputRole::Logits
                         : output.name == "pred_boxes"  ? RfdetrOutputRole::Boxes
                          : output.name == "pred_masks" ? RfdetrOutputRole::Masks
                          : output.name == "pred_keypoints" ? RfdetrOutputRole::Keypoints
                                                        : RfdetrOutputRole::Unspecified;
  if (role == RfdetrOutputRole::Unspecified)
   role = canonical;
  else if (canonical != RfdetrOutputRole::Unspecified && canonical != role)
   throw std::invalid_argument("RF-DETR output role contradicts canonical tensor name");
  auto* selected = role == RfdetrOutputRole::Logits ? &logits : role == RfdetrOutputRole::Boxes ? &boxes : role == RfdetrOutputRole::Masks ? &masks : role == RfdetrOutputRole::Keypoints ? &keypoints : nullptr;
  if (!selected || *selected) throw std::invalid_argument("ambiguous RF-DETR output roles");
  *selected = index;
  output.role = role;
  const auto rank = (role == RfdetrOutputRole::Masks || role == RfdetrOutputRole::Keypoints) ? 4U : 3U;
  if (output.shape.size() != rank || output.shape[0] == 0 || output.shape[0] < -1 || output.shape[1] <= 0 || output.shape[2] <= 0 || (role == RfdetrOutputRole::Boxes && output.shape[2] != 4) ||
      ((role == RfdetrOutputRole::Masks || role == RfdetrOutputRole::Keypoints) && output.shape[3] <= 0))
   throw std::invalid_argument("invalid RF-DETR output shape");
 }
 if (!logits || !boxes || info.outputs.size() != 2U + masks.has_value() + keypoints.has_value()) throw std::invalid_argument("RF-DETR requires declared logits and boxes outputs");
 const auto& shape = info.outputs[*logits].shape;
 for (const auto& output : info.outputs)
  if (output.shape[1] != shape[1] || (output.shape[0] > 0 && shape[0] > 0 && output.shape[0] != shape[0])) throw std::invalid_argument("RF-DETR output batch/query axes disagree");
 if (masks && keypoints) throw std::invalid_argument("RF-DETR masks and keypoints are mutually exclusive");
 if (!info.output_contract) {
  if (keypoints) throw std::invalid_argument("keypoint artifacts require explicit task and joint schema metadata");
  info.output_contract = RfdetrOutputContract{1U, masks ? ModelTask::Segmentation : ModelTask::Detection, {}};
 }
 const auto& contract = *info.output_contract;
 validate_rfdetr_output_contract(contract);
 if (contract.version != 1U || !model_task_name(contract.task) ||
     (contract.task == ModelTask::Segmentation) != masks.has_value() || (contract.task == ModelTask::Keypoints) != keypoints.has_value())
  throw std::invalid_argument("RF-DETR output roles disagree with task metadata");
 if (keypoints) {
  const KeypointTensorLayout joints(contract.keypoints_per_class);
  const auto& joint_shape = info.outputs[*keypoints].shape;
  if (joints.compact_count() == 0 || joints.slot_count() > shape[2] || joint_shape[2] != joints.padded_count() || joint_shape[3] != KeypointPredictionPacket::size)
   throw std::invalid_argument("RF-DETR keypoint output shape disagrees with explicit schema");
  if (info.class_layout) {
   const ResolvedClassLayout resolved(*info.class_layout);
   validate_rfdetr_output_contract(contract, &resolved);
  }
 } else if (!contract.keypoints_per_class.empty()) throw std::invalid_argument("non-keypoint artifact declares joint slots");
 for (const auto& output : info.outputs)
  if (output.dtype != info.outputs[*logits].dtype || (output.dtype != "float32" && output.dtype != "float16" && output.dtype != "bfloat16" && output.dtype != "float64"))
   throw std::invalid_argument("RF-DETR output dtypes disagree or are not floating point");
 info.num_queries = shape[1];
 info.num_classes = shape[2];
 info.has_masks = masks.has_value();
 return {*logits, *boxes, masks, keypoints};
}
}
