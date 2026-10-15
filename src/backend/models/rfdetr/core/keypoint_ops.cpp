// Keypoint equations derived from RF-DETR e2b5f096, Apache-2.0, Roboflow.
#include "detail/keypoint_ops.h"
#include "detail/matcher_workspace.h"
#include "runtime.h"
#include "src/backend/data/catalog/keypoint_visibility.h"
#include <torch/nn/functional.h>
#include <limits>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
namespace {
namespace F = torch::nn::functional;
using Visibility = mmltk::backend::data::catalog::KeypointVisibility;
constexpr auto unannotated = static_cast<int>(Visibility::Unannotated);
constexpr auto occluded = static_cast<int>(Visibility::Occluded);
using Packet = KeypointPredictionPacket;
constexpr double area_epsilon = std::numeric_limits<float>::epsilon();
void require_inputs(const torch::Tensor& predictions, const torch::Tensor& targets, const torch::Tensor& classes, const torch::Tensor& areas, const KeypointTensorLayout& layout, int rank) {
 if (layout.slot_count() == 0 || predictions.dim() != rank || predictions.size(-2) != layout.padded_count() || predictions.size(-1) != Packet::size || targets.dim() != 3 || targets.size(2) != 3 ||
     targets.size(1) > layout.maximum_joints() || classes.dim() != 1 || areas.dim() != 1 || classes.numel() != targets.size(0) || areas.numel() != targets.size(0) ||
     classes.scalar_type() != torch::kInt64 || !predictions.is_floating_point() || !targets.is_floating_point() || !areas.is_floating_point() || classes.device() != predictions.device() ||
     targets.device() != predictions.device() || areas.device() != predictions.device())
  throw std::invalid_argument("keypoint supervision tensors disagree with the admitted schema");
 // Schema slots may omit classifier-only trailing slots. Such labels have zero
 // joints and use the masked final schema row.
 if (rank == 3 && predictions.size(0) != targets.size(0)) throw std::invalid_argument("keypoint matched target extent differs");
}
torch::Tensor padded_targets(const torch::Tensor& targets, const KeypointTensorLayout& layout) { return F::pad(targets, F::PadFuncOptions({0, 0, 0, layout.maximum_joints() - targets.size(1)})); }
torch::Tensor xy(const torch::Tensor& prediction) { return prediction.narrow(-1, Packet::index<^^KeypointPrediction::x>(), 2); }
torch::Tensor bce(const torch::Tensor& logits, const torch::Tensor& targets) {
 return F::binary_cross_entropy_with_logits(logits, targets.to(logits.scalar_type()), F::BinaryCrossEntropyWithLogitsFuncOptions().reduction(torch::kNone));
}
torch::Tensor safe(const torch::Tensor& values, const torch::Tensor& mask) { return torch::where(mask, values, torch::zeros_like(values)); }
}  // namespace
KeypointLossTerms keypoint_losses(const torch::Tensor& predictions, const torch::Tensor& targets, const torch::Tensor& classes, const torch::Tensor& areas, const KeypointTensorLayout& layout) {
 require_inputs(predictions, targets, classes, areas, layout, 3);
 const auto n = predictions.size(0), k = layout.maximum_joints();
 const auto by_class = predictions.reshape({n, layout.slot_count(), k, Packet::size});
 const auto labels = classes.clamp_max(layout.slot_count() - 1);
 const auto selected = by_class.index({torch::arange(n, classes.options()), labels});
 const auto counts = active_matcher_workspace() ? active_matcher_workspace()->keypoint_counts(layout.counts(), predictions.device())
                                                : torch::tensor(std::vector<std::int64_t>(layout.counts().begin(), layout.counts().end()), classes.options());
 const auto mask = torch::arange(k, classes.options()).unsqueeze(0).lt(counts.index_select(0, labels).unsqueeze(1)) & classes.lt(layout.slot_count()).unsqueeze(-1);
 const auto target = padded_targets(targets, layout);
 const auto visibility = target.select(-1, 2);
 const auto pred_xy = xy(selected), target_xy = target.narrow(-1, 0, 2);
 const auto area = areas.to(torch::kFloat32);
 const auto valid_area = torch::isfinite(area) & area.gt(area_epsilon);
 const auto safe_area = torch::where(valid_area, area, torch::ones_like(area));
 const auto location = mask & torch::isfinite(visibility) & visibility.gt(unannotated) & torch::isfinite(pred_xy).all(-1) & torch::isfinite(target_xy).all(-1) & valid_area.unsqueeze(1);
 const auto location_count = location.sum(-1).to(predictions.scalar_type()).clamp_min(1);
 const auto difference = safe(pred_xy, location.unsqueeze(-1)) - safe(target_xy, location.unsqueeze(-1));
 KeypointLossTerms result;
 result.l1 = (difference.abs().sum(-1).masked_fill(~location, 0) / safe_area.clamp_min(area_epsilon).sqrt().unsqueeze(1)).sum(-1) / location_count;
 const auto visibility_loss = [&](const torch::Tensor& logits, const torch::Tensor& wanted) {
  const auto active = mask & torch::isfinite(logits);
  return bce(safe(logits, active), wanted).masked_fill(~active, 0).sum(-1) / active.sum(-1).clamp_min(1).to(predictions.scalar_type());
 };
 result.findable = visibility_loss(Packet::select<^^KeypointPrediction::findable_logit>(selected, -1), visibility.gt(unannotated));
 result.visible = visibility_loss(Packet::select<^^KeypointPrediction::visible_logit>(selected, -1), visibility.gt(occluded));
 auto a = Packet::select<^^KeypointPrediction::log_l11>(selected, -1).to(torch::kFloat32);
 auto b = Packet::select<^^KeypointPrediction::l21>(selected, -1).to(torch::kFloat32);
 auto c = Packet::select<^^KeypointPrediction::log_l22>(selected, -1).to(torch::kFloat32);
 auto gaussian = location & torch::isfinite(a) & torch::isfinite(b) & torch::isfinite(c);
 auto delta = (safe(pred_xy, gaussian.unsqueeze(-1)) - safe(target_xy, gaussian.unsqueeze(-1))).to(torch::kFloat32);
 auto dx = delta.select(-1, 0), dy = delta.select(-1, 1);
 a = safe(a, gaussian);
 b = safe(b, gaussian);
 c = safe(c, gaussian);
 auto u = a.exp() * dx + b * dy, v = c.exp() * dy;
 auto maha = u * u + v * v;
 gaussian = gaussian & torch::isfinite(u) & torch::isfinite(v) & torch::isfinite(maha);
 // Sanitize before recomputing: masking an overflowing result alone leaves
 // nonfinite local derivatives in the autograd graph.
 a = safe(a, gaussian);
 b = safe(b, gaussian);
 c = safe(c, gaussian);
 dx = safe(dx, gaussian);
 dy = safe(dy, gaussian);
 u = a.exp() * dx + b * dy;
 v = c.exp() * dy;
 maha = u * u + v * v;
 auto nll = 0.5 * maha / safe_area.clamp_min(area_epsilon).unsqueeze(1) - (a + c);
 nll = torch::nan_to_num(nll, 0.0, std::numeric_limits<float>::max() / 2.0, std::numeric_limits<float>::lowest());
 const auto count = gaussian.sum(-1).to(predictions.scalar_type());
 result.nll = torch::where(count.gt(0), nll.masked_fill(~gaussian, 0).sum(-1) / count.clamp_min(1), torch::zeros_like(count));
 return result;
}
KeypointLossTerms keypoint_matching_cost(const torch::Tensor& predictions, const torch::Tensor& targets, const torch::Tensor& classes, const torch::Tensor& areas, const KeypointTensorLayout& layout) {
 require_inputs(predictions, targets, classes, areas, layout, 4);
 const auto batch = predictions.size(0), queries = predictions.size(1), n = targets.size(0), flat = batch * queries;
 const auto shape = std::vector<int64_t>{batch, queries, n};
 KeypointLossTerms result{
  torch::zeros(shape, predictions.options()), torch::zeros(shape, predictions.options()), torch::zeros(shape, predictions.options()), torch::zeros(shape, predictions.options())
 };
 const auto target = padded_targets(targets, layout);
 // Preserve pinned class-local contraction order; only admitted nonempty
 // schemas allocate pairwise joint scratch.
 for (std::size_t slot = 0; slot < layout.counts().size(); ++slot) {
  const auto k = layout.counts()[slot];
  if (k == 0) continue;
  const auto indices = torch::nonzero(classes.eq(static_cast<int64_t>(slot))).squeeze(1);
  if (indices.numel() == 0) continue;
  const auto pred = predictions.narrow(2, layout.padded_offset(slot), k).reshape({flat, k, Packet::size});
  const auto truth = target.index_select(0, indices).narrow(1, 0, k);
  const auto area = areas.index_select(0, indices).to(torch::kFloat32);
  const auto target_xy = truth.narrow(-1, 0, 2).to(torch::kFloat32);
  const auto visibility = truth.select(-1, 2);
  const auto visible = torch::isfinite(visibility) & visibility.gt(unannotated) & torch::isfinite(target_xy).all(-1) & (torch::isfinite(area) & area.gt(area_epsilon)).unsqueeze(1);
  const auto denominator = visible.sum(-1).to(torch::kFloat32).clamp_min(1);
  const auto pred_xy = xy(pred).to(torch::kFloat32);
  const auto delta = pred_xy.unsqueeze(1) - target_xy.unsqueeze(0);
  auto loc = delta.abs().sum(-1).masked_fill(~visible.unsqueeze(0), 0).sum(-1) / denominator.unsqueeze(0) / area.clamp_min(area_epsilon).sqrt().unsqueeze(0);
  loc = torch::nan_to_num(loc, 0.0, 0.0, 0.0);
  const auto a = Packet::select<^^KeypointPrediction::log_l11>(pred, -1).to(torch::kFloat32);
  const auto b = Packet::select<^^KeypointPrediction::l21>(pred, -1).to(torch::kFloat32);
  const auto c = Packet::select<^^KeypointPrediction::log_l22>(pred, -1).to(torch::kFloat32);
  const auto dx = delta.select(-1, 0), dy = delta.select(-1, 1);
  const auto u = a.exp().unsqueeze(1) * dx + b.unsqueeze(1) * dy, v = c.exp().unsqueeze(1) * dy;
  const auto maha = u * u + v * v;
  const auto finite = torch::isfinite(pred_xy).all(-1) & torch::isfinite(a) & torch::isfinite(b) & torch::isfinite(c);
  const auto active = visible.unsqueeze(0) & finite.unsqueeze(1) & torch::isfinite(u) & torch::isfinite(v) & torch::isfinite(maha);
  auto nll = 0.5 * maha / area.clamp_min(area_epsilon).view({1, -1, 1}) - (a + c).unsqueeze(1);
  nll = torch::nan_to_num(nll, 0.0, std::numeric_limits<float>::max() / 2.0, std::numeric_limits<float>::lowest()).masked_fill(~active, 0).sum(-1) / denominator.unsqueeze(0);
  const auto pairwise_bce = [&](const torch::Tensor& logits, const torch::Tensor& labels) {
   return (F::softplus(logits).sum(1, true) - torch::matmul(logits, labels.to(logits.scalar_type()).transpose(0, 1))) / static_cast<double>(k);
  };
  const auto assign = [&](torch::Tensor& destination, const torch::Tensor& values) {
   destination.index_copy_(2, indices, values.reshape({batch, queries, indices.numel()}).to(predictions.scalar_type()));
  };
  assign(result.l1, loc);
  assign(result.nll, nll);
  assign(result.findable, pairwise_bce(Packet::select<^^KeypointPrediction::findable_logit>(pred, -1), visibility.gt(unannotated)));
  assign(result.visible, pairwise_bce(Packet::select<^^KeypointPrediction::visible_logit>(pred, -1), visibility.gt(occluded)));
 }
 return result;
}
torch::Tensor add_keypoint_matching_cost(const torch::Tensor& cost, const KeypointLossTerms& terms, const DetectionConfig& config) {
 return cost + config.keypoint_l1_loss_coef * terms.l1 + config.keypoint_findable_loss_coef * terms.findable + config.keypoint_visible_loss_coef * terms.visible +
        config.keypoint_nll_loss_coef * terms.nll;
}
}  // namespace mmltk::backend::models::rfdetr
