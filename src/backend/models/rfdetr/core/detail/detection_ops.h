#pragma once
#include <torch/torch.h>

#include <functional>
#include <utility>
#include <vector>

#include "detection_types.h"

namespace mmltk::backend::models::rfdetr {

using AllReduceTensorFn = std::function<void(torch::Tensor&)>;

torch::Tensor sample_target_masks(const PackedTargetMasks& masks, const torch::Tensor& mask_indices, const torch::Tensor& point_coords,
                                  const char* context);

std::vector<std::pair<torch::Tensor, torch::Tensor>> matcher_indices(const ModelOutputs& outputs, const PreparedTargets& targets,
                                                                     const DetectionConfig& config, bool training_mode);
TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config,
                              bool training_mode, bool distributed_enabled, const AllReduceTensorFn& distributed_all_reduce = {});
TensorMap detection_loss_dict(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config,
                              bool training_mode, double num_boxes_value);
torch::Tensor weighted_detection_loss(const TensorMap& loss_dict, const DetectionConfig& config, const torch::Device& device);

}  // namespace mmltk::backend::models::rfdetr
