#pragma once
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/keypoint_layout.h"
namespace mmltk::backend::models::rfdetr {
// Per-target losses, or Cartesian [batch, query, target] matching costs.
struct KeypointLossTerms final {
 torch::Tensor l1, findable, visible, nll;
};
[[nodiscard]] KeypointLossTerms keypoint_losses(const torch::Tensor& predictions, const torch::Tensor& targets,
 const torch::Tensor& classes, const torch::Tensor& areas, const KeypointTensorLayout& layout);
[[nodiscard]] KeypointLossTerms keypoint_matching_cost(const torch::Tensor& predictions, const torch::Tensor& targets,
 const torch::Tensor& classes, const torch::Tensor& areas, const KeypointTensorLayout& layout);
[[nodiscard]] torch::Tensor add_keypoint_matching_cost(const torch::Tensor&, const KeypointLossTerms&, const DetectionConfig&);
}  // namespace mmltk::backend::models::rfdetr
