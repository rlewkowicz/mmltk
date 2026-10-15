#pragma once
#include "src/backend/models/rfdetr/core/criterion_terms.h"
#include <torch/types.h>
#include <functional>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/detection_statistics.h"
namespace mmltk::backend::models::rfdetr {
using AllReduceTensorFn = std::function<void(torch::Tensor&)>;
[[nodiscard]] torch::Tensor cardinality_error(const torch::Tensor& logits, const torch::Tensor& target_counts);
std::vector<std::pair<torch::Tensor, torch::Tensor>> matcher_indices(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode);
CriterionResult detection_loss_terms(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode, bool distributed_enabled,
 const AllReduceTensorFn& distributed_all_reduce = {}, DetectionStatisticsPacket::Tensors* statistics = nullptr);
CriterionResult detection_loss_terms(
 const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode, double num_boxes_value, DetectionStatisticsPacket::Tensors* statistics = nullptr);
CriterionResult detection_loss_terms(const ModelOutputs& outputs, const PreparedTargets& targets, const DetectionConfig& config, bool training_mode, const torch::Tensor& num_boxes_value,
 DetectionStatisticsPacket::Tensors* statistics = nullptr);
torch::Tensor weighted_detection_loss(const CriterionResult& losses, const DetectionConfig& config, const torch::Device& device, torch::Tensor* auxiliary_weighted = nullptr);
}  // namespace mmltk::backend::models::rfdetr
