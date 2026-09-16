#include "detail/box_iou.h"
#include <torch/torch.h>
#include "detail/box_iou_cuda.h"
namespace mmltk::backend::ml::ops {
torch::Tensor box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) { return detail::box_iou_cuda(boxes1, boxes2); }
torch::Tensor generalized_box_iou_cuda(const torch::Tensor& boxes1, const torch::Tensor& boxes2) { return detail::generalized_box_iou_cuda(boxes1, boxes2); }
}  // namespace mmltk::backend::ml::ops
