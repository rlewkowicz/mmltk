#pragma once

#include <torch/torch.h>

namespace mmltk::rfdetr {

torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

void pairwise_detection_cost_cuda_out(const torch::Tensor& output, const torch::Tensor& pred_logits,
                                      const torch::Tensor& pred_boxes, const torch::Tensor& target_labels,
                                      const torch::Tensor& target_boxes, const torch::Tensor& target_offsets,
                                      const torch::Tensor& target_counts, double class_cost, double bbox_cost,
                                      double giou_cost);

void pairwise_mask_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& pred_mask_logits,
                                  const torch::Tensor& target_masks, const torch::Tensor& target_offsets,
                                  const torch::Tensor& target_counts, double ce_cost, double dice_cost);

}  
