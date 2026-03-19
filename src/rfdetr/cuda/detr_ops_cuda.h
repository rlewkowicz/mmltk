#pragma once

#include <torch/torch.h>

namespace fastloader::rfdetr {

// Computes pairwise Generalized IoU between [N, 4] and [M, 4] boxes.
// Returns [N, M] matrix.
// Boxes are in XYXY format.
torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

// Computes pairwise IoU between [N, 4] and [M, 4] boxes.
// Returns [N, M] matrix.
// Boxes are in XYXY format.
torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

// Fused Sigmoid Focal Loss
// inputs: [N, C]
// targets: [N, C]
torch::Tensor sigmoid_focal_loss_cuda(
    const torch::Tensor& inputs,
    const torch::Tensor& targets,
    double alpha,
    double gamma);

// Computes both Dice and Sigmoid CE loss in a fused way.
// returns {loss_ce, loss_dice}
std::pair<torch::Tensor, torch::Tensor> fused_dice_ce_loss_cuda(
    const torch::Tensor& inputs,  // [N, P]
    const torch::Tensor& targets, // [N, P]
    double num_masks);

} // namespace fastloader::rfdetr
