#pragma once
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/numa_topology.h"
namespace mmltk::backend::models::rfdetr::testsupport {
inline void move_required_output_tensors(OutputLayer& layer, const torch::Device& device) {
 layer.for_each_required_tensor([&](torch::Tensor& tensor) { tensor = tensor.to(device); });
}
inline void configure_cuda_training_model(NativeRfDetrModel& model) {
 model.to(torch::Device(torch::kCUDA));
 model.train(true);
 model.set_force_pytorch_deformable_attn(true);
}
class MatcherExecutionFixture final {
private:
 static mmltk::common::system::ExecutionPlacement Placement() {
  const auto topology = mmltk::common::system::NumaTopology::Capture();
  return mmltk::common::system::resolve_placement(topology, topology.permitted_nodes.front());
 }
 const mmltk::common::system::ExecutionPlacement placement_ = Placement();
 mmltk::common::system::ScopedExecutionPolicy policy_{{placement_.cpus, {}, 0, placement_.numa_node, -10, false}};

public:
 MatcherWorkspace workspace{placement_.numa_node, true};
};
inline void populate_target_views(PreparedTargets& result) {
 result.targets.resize(result.counts.size());
 for (std::size_t image = 0; image < result.counts.size(); ++image) {
  auto& target = result.targets[image];
  target.boxes = result.all_boxes.narrow(0, result.offsets[image], result.counts[image]);
  target.labels = result.all_labels.narrow(0, result.offsets[image], result.counts[image]);
 }
}
[[nodiscard]] inline PreparedTargets one_image_targets(const torch::Tensor& boxes, const torch::Tensor& labels) {
 PreparedTargets result;
 auto& target = result.targets.emplace_back();
 target.boxes = boxes;
 target.labels = labels;
 result.all_boxes = boxes;
 result.all_labels = labels;
 result.offsets = {0};
 result.counts = {labels.numel()};
 return result;
}
[[nodiscard]] inline DenoisingOutputs denoising_output_fixture(const DenoisingQueryBatch& prepared) {
 DenoisingOutputs result;
 // CLEANUP-OFF: Independent fixture projection tests the production model's DN metadata handoff.
 result.original_labels = prepared.original_labels;
 result.original_boxes = prepared.original_boxes;
 result.valid_slots = prepared.valid_slots;
 result.target_indices = prepared.target_indices;
 result.mask_sampling_seed = prepared.mask_sampling_seed;
 result.groups = prepared.layout.denoising_groups;
 result.queries_per_group = prepared.layout.denoising_queries_per_group;
 // CLEANUP-ON
 return result;
}
// Literal reference arithmetic shared only by test oracles, never production.
[[nodiscard]] inline torch::Tensor reference_focal_elements(const torch::Tensor& logits, const torch::Tensor& labels, double alpha, int gamma) {
 const auto probability = logits.sigmoid();
 return (labels * torch::softplus(-logits) + (1 - labels) * torch::softplus(logits)) * torch::pow(1 - (probability * labels + (1 - probability) * (1 - labels)), gamma) *
        (alpha * labels + (1 - alpha) * (1 - labels));
}
[[nodiscard]] inline torch::Tensor reference_reparam_boxes(const torch::Tensor& base, const torch::Tensor& delta) {
 return torch::cat({delta.slice(-1, 0, 2) * base.slice(-1, 2, 4) + base.slice(-1, 0, 2), delta.slice(-1, 2, 4).exp() * base.slice(-1, 2, 4)}, -1);
}
// Cases retain their own class-loss policy, coefficients and mask settings.
[[nodiscard]] inline DetectionConfig detection_fixture_base(const NativeRfDetrConfig& model) {
 DetectionConfig result;
 result.num_classes = model.num_classes;
 result.group_detr = model.group_detr;
 result.dec_layers = model.dec_layers;
 result.num_select = model.num_select;
 result.two_stage = model.two_stage;
 result.aux_loss = model.aux_loss;
 result.ia_bce_loss = model.ia_bce_loss;
 result.focal_alpha = model.focal_alpha;
 result.set_cost_class = model.set_cost_class;
 result.set_cost_bbox = model.set_cost_bbox;
 result.set_cost_giou = model.set_cost_giou;
 return result;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
