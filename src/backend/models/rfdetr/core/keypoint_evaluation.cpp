#include "evaluation.h"
#include <cmath>
#include <cstring>
#include <limits>
namespace mmltk::backend::models::rfdetr {
PredictedKeypoint decode_prediction_keypoint(const KeypointPrediction& point) {
 return {point.x, point.y, 1.0F / (1.0F + std::exp(-point.findable_logit)), {point.log_l11, point.l21, point.log_l22}};
}
PredictedKeypoint map_prediction_keypoint(PredictedKeypoint point, double sx, double sy, double ox, double oy) {
 if (!(sx > 0) || !(sy > 0) || !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(ox) || !std::isfinite(oy)) throw std::invalid_argument("invalid keypoint coordinate Jacobian");
 point.x = point.x * sx + ox;
 point.y = point.y * sy + oy;
 point.precision_cholesky[0] -= std::log(sx);
 point.precision_cholesky[1] /= sy;
 point.precision_cholesky[2] -= std::log(sy);
 return point;
}
std::array<float, 3> keypoint_covariance(const PredictedKeypoint& point) {
 const auto& precision = point.precision_cholesky;
 const double a = std::exp(static_cast<double>(precision[0])), b = precision[1], c = std::exp(static_cast<double>(precision[2]));
 const double inv_det = 1.0 / (a * a * c * c);
 const std::array<double, 3> covariance{(b * b + c * c) * inv_det, -a * b * inv_det + 0.0, a * a * inv_det};
 if (!std::ranges::all_of(precision, [](float value) { return std::isfinite(value); }) || !std::ranges::all_of(covariance, [](double value) { return std::isfinite(value); })) {
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  return {nan,nan,nan};
 }
 // Upstream validates in double before narrowing its pixel product to float.
 return {static_cast<float>(covariance[0]),static_cast<float>(covariance[1]),static_cast<float>(covariance[2])};
}
double keypoint_oks(const KeypointPredictionView& predictions, std::size_t prediction,
 std::span<const mmltk::backend::data::PackedKeypoint> target, std::span<const double> sigmas,
 const std::array<float, 4>& box, double area, const std::array<double, 2>& scale) {
 if (prediction >= kMaximumPredictionCandidates || target.size() != sigmas.size() || target.size() > predictions.joint_capacity || predictions.joint_capacity > mmltk::backend::data::catalog::kKeypointsPerClassCapacity ||
     predictions.prediction_stride < static_cast<std::ptrdiff_t>(predictions.joint_capacity * sizeof(KeypointPrediction) / sizeof(float)) || !predictions.data) throw std::invalid_argument("OKS joint schema/extent differs");
 static_cast<void>(checked_prediction_extent(prediction + 1, predictions.prediction_stride, kMaximumPredictionTensorBytes / sizeof(float)));
 const bool annotated = std::ranges::any_of(target, [](const auto& joint) { return joint.visibility != mmltk::backend::data::catalog::KeypointVisibility::Unannotated; });
 const double width = box[2] - box[0], height = box[3] - box[1];
 double total = 0;
 std::size_t count = 0;
 for (std::size_t joint = 0; joint < target.size(); ++joint) {
  if (annotated && target[joint].visibility == mmltk::backend::data::catalog::KeypointVisibility::Unannotated) continue;
  KeypointPrediction point;
  std::memcpy(&point, predictions.data + prediction * predictions.prediction_stride + joint * sizeof(KeypointPrediction) / sizeof(float), sizeof(point));
  const double dx = annotated ? point.x - target[joint].x : std::max(0.0, box[0] - width - point.x) + std::max(0.0, point.x - box[2] - width);
  const double dy = annotated ? point.y - target[joint].y : std::max(0.0, box[1] - height - point.y) + std::max(0.0, point.y - box[3] - height);
  const double variance = 4 * sigmas[joint] * sigmas[joint];
  const double error = ((dx * scale[0]) * (dx * scale[0]) + (dy * scale[1]) * (dy * scale[1])) / variance / (area + std::numeric_limits<double>::epsilon()) / 2;
  total += std::exp(-error);
  ++count;
 }
 return count ? total / static_cast<double>(count) : 0;
}
}  // namespace mmltk::backend::models::rfdetr
