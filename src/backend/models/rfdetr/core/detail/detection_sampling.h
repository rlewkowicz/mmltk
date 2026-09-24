#pragma once
#include <torch/types.h>
#include <span>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
namespace mmltk::backend::models::rfdetr {
// Optional per-layer draws for deterministic criterion evaluation. Layer order
// is main, auxiliary, encoder; undefined tensors use the production RNG.
struct LayerMaskSamples {
 torch::Tensor matcher;
 torch::Tensor uncertain_candidates;
 torch::Tensor uncertain_random;
};
TensorMap detection_loss_dict(
 const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode, const torch::Tensor& num_boxes, std::span<const LayerMaskSamples> samples);
std::vector<std::pair<torch::Tensor, torch::Tensor>> matcher_indices(
 const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode, const LayerMaskSamples& samples);
}  // namespace mmltk::backend::models::rfdetr
