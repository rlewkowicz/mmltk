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
torch::Tensor box_area(const torch::Tensor& boxes) {
 const auto promoted = boxes.scalar_type() == torch::kFloat16 || boxes.scalar_type() == torch::kBFloat16 ? boxes.to(torch::kFloat32) : boxes;
 return (promoted.select(-1, 2) - promoted.select(-1, 0)) * (promoted.select(-1, 3) - promoted.select(-1, 1));
}
torch::Tensor span_area(const torch::Tensor& boxes1, const torch::Tensor& boxes2, const BoxSpan span, bool pairwise) {
 const auto coordinate = [&](const torch::Tensor& boxes, int64_t start, int64_t axis) {
  const auto value = boxes.slice(-1, start, start + 2);
  return pairwise ? value.unsqueeze(axis) : value;
 };
 const auto lhs_lower = coordinate(boxes1, 0, -2), rhs_lower = coordinate(boxes2, 0, -3);
 const auto lower = span == BoxSpan::Intersection ? torch::maximum(lhs_lower, rhs_lower) : torch::minimum(lhs_lower, rhs_lower);
 const auto lhs_upper = coordinate(boxes1, 2, -2), rhs_upper = coordinate(boxes2, 2, -3);
 const auto upper = span == BoxSpan::Intersection ? torch::minimum(lhs_upper, rhs_upper) : torch::maximum(lhs_upper, rhs_upper);
 return corner_span_area(lower, upper);
}
std::pair<torch::Tensor, torch::Tensor> generic_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2, bool pairwise) {
 // Preserve the upstream operation order as well as its formula. Half VJPs
 // round at branch accumulation, especially near coincident box boundaries.
 const auto area1 = box_area(boxes1), area2 = box_area(boxes2);
 const auto intersection = span_area(boxes1, boxes2, BoxSpan::Intersection, pairwise);
 const auto lhs_area = pairwise ? area1.unsqueeze(-1) : area1;
 const auto rhs_area = pairwise ? area2.unsqueeze(-2) : area2;
 const auto union_area = lhs_area + rhs_area - intersection;
 return {intersection / union_area.clamp_min(1e-7), union_area};
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
 const auto coordinates = boxes.unbind(-1);
 const auto extent = [&](const torch::Tensor& value) { return extent_policy == BoxExtentPolicy::ClampNonnegative ? value.clamp_min(0.0) : value; };
 // Each corner has its own clamp/multiply branch in the pinned upstream.
 return torch::stack(
  {coordinates[0] - 0.5 * extent(coordinates[2]), coordinates[1] - 0.5 * extent(coordinates[3]), coordinates[0] + 0.5 * extent(coordinates[2]), coordinates[1] + 0.5 * extent(coordinates[3])}, -1);
}
torch::Tensor pairwise_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_pairwise_prefix(boxes1, boxes2, "pairwise_box_iou");
 if (boxes1.dim() == 2 && boxes1.is_cuda() && boxes2.is_cuda() && boxes1.scalar_type() == torch::kFloat32 && boxes2.scalar_type() == torch::kFloat32 && !boxes1.requires_grad() &&
     !boxes2.requires_grad()) {
  return mmltk::backend::ml::ops::box_iou_cuda(boxes1, boxes2);
 }
 return generic_iou(boxes1, boxes2, true).first;
}
torch::Tensor pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_pairwise_prefix(boxes1, boxes2, "pairwise_generalized_box_iou");
 if (boxes1.dim() == 2 && boxes1.is_cuda() && boxes2.is_cuda() && boxes1.scalar_type() == torch::kFloat32 && boxes2.scalar_type() == torch::kFloat32 && !boxes1.requires_grad() &&
     !boxes2.requires_grad()) {
  return mmltk::backend::ml::ops::generalized_box_iou_cuda(boxes1, boxes2);
 }
 const auto [iou, union_area] = generic_iou(boxes1, boxes2, true);
 const auto enclosure = span_area(boxes1, boxes2, BoxSpan::Enclosure, true);
 return iou - (enclosure - union_area) / enclosure.clamp_min(1e-7);
}
torch::Tensor aligned_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_aligned_pair(boxes1, boxes2, "aligned_box_iou");
 return generic_iou(boxes1, boxes2, false).first;
}
torch::Tensor aligned_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 require_aligned_pair(boxes1, boxes2, "aligned_generalized_box_iou");
 const auto [iou, union_area] = generic_iou(boxes1, boxes2, false);
 const auto enclosure = span_area(boxes1, boxes2, BoxSpan::Enclosure, false);
 return iou - (enclosure - union_area) / enclosure.clamp_min(1e-7);
}
torch::Tensor batched_pairwise_generalized_box_iou(const torch::Tensor& boxes1, const torch::Tensor& boxes2) {
 if (boxes1.dim() < 3 || boxes2.dim() < 3) { throw std::runtime_error("batched_pairwise_generalized_box_iou expects [...,M,4] and [...,N,4]"); }
 return pairwise_generalized_box_iou(boxes1, boxes2);
}
}  // namespace mmltk::backend::models::rfdetr
