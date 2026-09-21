#include "detail/detection_geometry.h"
#include <ATen/TensorIndexing.h>
#include "src/backend/ml/ops/box_iou.h"
namespace mmltk::backend::models::rfdetr {
namespace {
using namespace torch::indexing;
enum class BoxSpan : std::uint8_t {
 Intersection,
 Enclosure,
};
void require_box_tensor(const torch::Tensor& boxes, const char* what) {
 if (!boxes.defined() || boxes.dim() < 2 || boxes.size(-1) != 4) { throw std::runtime_error(std::string(what) + " expects a [...,N,4] box tensor"); }
}
torch::Tensor corner_span_area(const torch::Tensor& lower, const torch::Tensor& upper) {
 const auto extent = (upper - lower).clamp_min(0.0);
 return extent.select(-1, 0) * extent.select(-1, 1);
}
torch::Tensor box_area(const torch::Tensor& boxes) { return corner_span_area(boxes.slice(-1, 0, 2), boxes.slice(-1, 2, 4)); }
torch::Tensor span_area(const torch::Tensor& lhs_lower, const torch::Tensor& rhs_lower, const torch::Tensor& lhs_upper, const torch::Tensor& rhs_upper,
                        const BoxSpan span) {
 const auto lower = span == BoxSpan::Intersection ? torch::maximum(lhs_lower, rhs_lower) : torch::minimum(lhs_lower, rhs_lower);
 const auto upper = span == BoxSpan::Intersection ? torch::minimum(lhs_upper, rhs_upper) : torch::maximum(lhs_upper, rhs_upper);
 return corner_span_area(lower, upper);
}
std::pair<torch::Tensor, torch::Tensor> generic_pairwise_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 const auto lhs_lower = boxes1.slice(-1, 0, 2).unsqueeze(-2);
 const auto rhs_lower = boxes2.slice(-1, 0, 2).unsqueeze(-3);
 const auto lhs_upper = boxes1.slice(-1, 2, 4).unsqueeze(-2);
 const auto rhs_upper = boxes2.slice(-1, 2, 4).unsqueeze(-3);
 const auto intersection = span_area(lhs_lower, rhs_lower, lhs_upper, rhs_upper, BoxSpan::Intersection);
 const auto lhs_area = box_area(boxes1).unsqueeze(-1);
 const auto rhs_area = box_area(boxes2).unsqueeze(-2);
 const auto union_area = lhs_area + rhs_area - intersection;
 return {intersection / union_area, union_area};
}
void require_pairwise_prefix(const torch::Tensor& boxes1, const torch::Tensor& boxes2, const char* what) {
 require_box_tensor(boxes1, what);
 require_box_tensor(boxes2, what);
 if (boxes1.dim() != boxes2.dim() || boxes1.sizes().slice(0, boxes1.dim() - 2).vec() != boxes2.sizes().slice(0, boxes2.dim() - 2).vec()) {
  throw std::runtime_error(std::string(what) + " requires identical prefix dimensions");
 }
}
void require_aligned_pair(const torch::Tensor& boxes1, const torch::Tensor& boxes2, const char* what) {
 require_pairwise_prefix(boxes1, boxes2, what);
 if (boxes1.sizes() != boxes2.sizes()) { throw std::runtime_error(std::string(what) + " requires aligned box shapes"); }
}
}  // namespace
torch::Tensor box_cxcywh_to_xyxy(const torch::Tensor& boxes, const BoxExtentPolicy extent_policy) {
 require_box_tensor(boxes, "box_cxcywh_to_xyxy");
 const auto center = boxes.slice(-1, 0, 2);
 auto extent = boxes.slice(-1, 2, 4);
 if (extent_policy == BoxExtentPolicy::ClampNonnegative) { extent = extent.clamp_min(0.0); }
 const auto half_extent = extent * 0.5;
 return torch::cat({center - half_extent, center + half_extent}, -1);
}
torch::Tensor pairwise_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_pairwise_prefix(boxes1, boxes2, "pairwise_box_iou");
 if (boxes1.dim() == 2 && boxes1.is_cuda() && boxes2.is_cuda() && boxes1.scalar_type() == torch::kFloat32 && boxes2.scalar_type() == torch::kFloat32) {
  return mmltk::backend::ml::ops::box_iou_cuda(boxes1, boxes2);
 }
 return generic_pairwise_iou(boxes1, boxes2).first;
}
torch::Tensor pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_pairwise_prefix(boxes1, boxes2, "pairwise_generalized_box_iou");
 if (boxes1.dim() == 2 && boxes1.is_cuda() && boxes2.is_cuda() && boxes1.scalar_type() == torch::kFloat32 && boxes2.scalar_type() == torch::kFloat32) {
  return mmltk::backend::ml::ops::generalized_box_iou_cuda(boxes1, boxes2);
 }
 const auto [iou, union_area] = generic_pairwise_iou(boxes1, boxes2);
 const auto enclosure = span_area(boxes1.slice(-1, 0, 2).unsqueeze(-2), boxes2.slice(-1, 0, 2).unsqueeze(-3), boxes1.slice(-1, 2, 4).unsqueeze(-2),
                                  boxes2.slice(-1, 2, 4).unsqueeze(-3), BoxSpan::Enclosure);
 return iou - (enclosure - union_area) / enclosure;
}
torch::Tensor aligned_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_aligned_pair(boxes1, boxes2, "aligned_box_iou");
 const auto intersection = span_area(boxes1.slice(-1, 0, 2), boxes2.slice(-1, 0, 2), boxes1.slice(-1, 2, 4), boxes2.slice(-1, 2, 4), BoxSpan::Intersection);
 return intersection / (box_area(boxes1) + box_area(boxes2) - intersection);
}
torch::Tensor aligned_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_aligned_pair(boxes1, boxes2, "aligned_generalized_box_iou");
 const auto intersection = span_area(boxes1.slice(-1, 0, 2), boxes2.slice(-1, 0, 2), boxes1.slice(-1, 2, 4), boxes2.slice(-1, 2, 4), BoxSpan::Intersection);
 const auto union_area = box_area(boxes1) + box_area(boxes2) - intersection;
 const auto enclosure = span_area(boxes1.slice(-1, 0, 2), boxes2.slice(-1, 0, 2), boxes1.slice(-1, 2, 4), boxes2.slice(-1, 2, 4), BoxSpan::Enclosure);
 return intersection / union_area - (enclosure - union_area) / enclosure;
}
torch::Tensor batched_pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 if (boxes1.dim() < 3 || boxes2.dim() < 3) { throw std::runtime_error("batched_pairwise_generalized_box_iou expects [...,M,4] and [...,N,4]"); }
 return pairwise_generalized_box_iou(boxes1, boxes2);
}
}  // namespace mmltk::backend::models::rfdetr
