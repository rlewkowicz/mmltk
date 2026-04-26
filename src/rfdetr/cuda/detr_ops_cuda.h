#pragma once

#include <torch/torch.h>

namespace mmltk::rfdetr {

torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2);

}  // namespace mmltk::rfdetr
