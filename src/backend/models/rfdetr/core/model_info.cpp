#include "model_info.h"
#include "keypoint_layout.h"
#include <algorithm>
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
 RfdetrOutputRoles roles;
 for (std::size_t index = 0; index < info.outputs.size(); ++index) {
  auto& output = info.outputs[index];
  auto role = output.role;
  const auto canonical = RfdetrOutputSchema::canonical_role(output.name);
  if (role == RfdetrOutputRole::Unspecified)
   role = canonical;
  else if (canonical != RfdetrOutputRole::Unspecified && canonical != role)
   throw std::invalid_argument("RF-DETR output role contradicts canonical tensor name");
  std::size_t rank = 0;
  RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
   if (role == Role) {
    if (roles.index<Role>()) throw std::invalid_argument("ambiguous RF-DETR output roles");
    roles.resolve<Role>(index);
    rank = facts.rank;
   }
  });
  if (rank == 0) throw std::invalid_argument("ambiguous RF-DETR output roles");
  output.role = role;
  if (output.shape.size() != rank || output.shape[0] == 0 || output.shape[0] < -1 || output.shape[1] <= 0 || output.shape[2] <= 0 || (role == RfdetrOutputRole::Boxes && output.shape[2] != 4) ||
      std::any_of(output.shape.begin() + 3, output.shape.end(), [](auto extent) { return extent <= 0; }))
   throw std::invalid_argument("invalid RF-DETR output shape");
 }
 RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
  if (!facts.task && !roles.index<Role>()) throw std::invalid_argument("RF-DETR requires declared logits and boxes outputs");
 });
 const auto logits = roles.index<RfdetrOutputRole::Logits>();
 const auto keypoints = roles.index<RfdetrOutputRole::Keypoints>();
 const auto& shape = info.outputs[*logits].shape;
 for (const auto& output : info.outputs)
  if (output.shape[1] != shape[1] || (output.shape[0] > 0 && shape[0] > 0 && output.shape[0] != shape[0])) throw std::invalid_argument("RF-DETR output batch/query axes disagree");
 std::optional<ModelTask> inferred_task;
 RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
  if (facts.task && roles.index<Role>()) {
   if (inferred_task && inferred_task != facts.task) throw std::invalid_argument("RF-DETR masks and keypoints are mutually exclusive");
   inferred_task = facts.task;
  }
 });
 if (!info.output_contract) {
  if (keypoints) throw std::invalid_argument("keypoint artifacts require explicit task and joint schema metadata");
  info.output_contract = RfdetrOutputContract{1U, inferred_task.value_or(ModelTask::Detection), {}};
 }
 const auto& contract = *info.output_contract;
 validate_rfdetr_output_contract(contract);
 RfdetrOutputSchema::visit([&]<RfdetrOutputRole Role>(RfdetrOutputPolicy facts) {
  if (facts.supports(contract.task) != roles.index<Role>().has_value()) throw std::invalid_argument("RF-DETR output roles disagree with task metadata");
 });
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
 info.has_masks = roles.index<RfdetrOutputRole::Masks>().has_value();
 return roles;
}
}
