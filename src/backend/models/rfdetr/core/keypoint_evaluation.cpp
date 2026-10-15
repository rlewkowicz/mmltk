#include "evaluation.h"
#include <cmath>
#include <cstring>
#include <limits>
namespace mmltk::backend::models::rfdetr {
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
