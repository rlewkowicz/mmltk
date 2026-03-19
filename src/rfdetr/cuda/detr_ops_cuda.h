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

} // namespace fastloader::rfdetr
