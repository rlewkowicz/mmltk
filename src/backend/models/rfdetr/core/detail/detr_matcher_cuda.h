#pragma once
#include <torch/torch.h>
namespace mmltk::backend::models::rfdetr {
void pairwise_detection_cost_cuda_out(const torch::Tensor& output, const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes, const torch::Tensor& target_labels,
 const torch::Tensor& target_boxes, const torch::Tensor& target_offsets, const torch::Tensor& target_counts, const torch::Tensor& output_offsets, int64_t padded_queries, int64_t max_targets,
 double class_cost, double bbox_cost, double giou_cost, double focal_alpha);
void pairwise_keypoint_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& predictions, const torch::Tensor& targets,
 const torch::Tensor& labels, const torch::Tensor& boxes, const torch::Tensor& schema_counts,
 const torch::Tensor& target_offsets, const torch::Tensor& target_counts, const torch::Tensor& output_offsets,
 int64_t padded_queries, int64_t max_targets, int64_t maximum_joints, double l1_cost, double findable_cost, double visible_cost, double nll_cost);
void pairwise_mask_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& pred_mask_logits, const torch::Tensor& target_masks, const torch::Tensor& target_offsets,
 const torch::Tensor& target_counts, const torch::Tensor& output_offsets, int64_t padded_queries, int64_t max_targets, double ce_cost, double dice_cost);
}  // namespace mmltk::backend::models::rfdetr
