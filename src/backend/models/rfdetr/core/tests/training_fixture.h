#pragma once
#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/detail/matcher_workspace.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/numa_topology.h"
namespace mmltk::backend::models::rfdetr::testsupport {
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
