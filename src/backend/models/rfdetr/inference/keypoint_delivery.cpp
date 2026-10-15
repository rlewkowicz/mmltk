#include "keypoint_delivery.h"
#include <limits>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
void write_analysis_keypoints(const PostprocessedSelection& selected, mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotation) {
 namespace runtime = mmltk::backend::ml::runtime;
 annotation.keypoints_available = false;
 if (!selected.keypoints) return;
 const auto& source = *selected.keypoints;
 const auto count = source.size(1), joints = source.size(2);
 if (!annotation.keypoint_capacity) return;  // Semantic-only delivery retains the selected native packet.
 if (source.size(0) != 1 || annotation.keypoint_capacity != joints || annotation.value_capacity < static_cast<size_t>(count) || !annotation.keypoints.address || !annotation.keypoint_counts.address ||
     annotation.keypoints.element_type != runtime::AnalysisElementType::Float32 || annotation.keypoint_counts.element_type != runtime::AnalysisElementType::Int32 ||
     annotation.keypoints.shape.rank != 3 || annotation.keypoints.shape.extents[0] < count || annotation.keypoints.shape.extents[1] != joints ||
     annotation.keypoints.shape.extents[2] != runtime::kAnalysisKeypointChannels || annotation.keypoint_counts.shape.rank != 1 || annotation.keypoint_counts.shape.extents[0] < count ||
     annotation.keypoints.capacity_bytes < checked_prediction_extent(count, joints * sizeof(runtime::AnalysisKeypoint), kMaximumPredictionTensorBytes) ||
     annotation.keypoint_counts.capacity_bytes < static_cast<size_t>(count) * sizeof(int32_t))
  throw std::invalid_argument("analysis keypoint storage disagrees with admitted schema/capacity");
 using Packet = KeypointPredictionPacket;
 using Product = ReflectedTensorPacket<runtime::AnalysisKeypoint>;
 static_assert(Product::size == runtime::kAnalysisKeypointChannels);
 auto destination = torch::from_blob(reinterpret_cast<void*>(annotation.keypoints.address), {count, joints, static_cast<int64_t>(Product::size)}, source.options());
 const auto points = source[0];
 Product::select<^^runtime::AnalysisKeypoint::x>(destination, -1).copy_(Packet::select<^^KeypointPrediction::x>(points, -1) + annotation.source_region.x);
 Product::select<^^runtime::AnalysisKeypoint::y>(destination, -1).copy_(Packet::select<^^KeypointPrediction::y>(points, -1) + annotation.source_region.y);
 const auto counts = selected.keypoint_counts[0];
 const auto active = torch::arange(joints, counts.options()).unsqueeze(0).lt(counts.unsqueeze(-1));
 Product::select<^^runtime::AnalysisKeypoint::confidence>(destination, -1).copy_(Packet::select<^^KeypointPrediction::findable_logit>(points, -1).sigmoid().masked_fill(~active, 0));
 // Match pinned upstream's double-precision inversion before the float product.
 const auto a = Packet::select<^^KeypointPrediction::log_l11>(points, -1).to(torch::kFloat64).exp();
 const auto b = Packet::select<^^KeypointPrediction::l21>(points, -1).to(torch::kFloat64);
 const auto c = Packet::select<^^KeypointPrediction::log_l22>(points, -1).to(torch::kFloat64).exp();
 const auto inv_det = 1.0 / (a.square() * c.square());
 const auto xx = ((b.square() + c.square()) * inv_det);
 const auto xy = (-a * b * inv_det + 0.0);
 const auto yy = (a.square() * inv_det);
 const auto valid = active & torch::isfinite(a) & torch::isfinite(b) & torch::isfinite(c) & torch::isfinite(xx) & torch::isfinite(xy) & torch::isfinite(yy);
 const auto nan = std::numeric_limits<float>::quiet_NaN();
 Product::select<^^runtime::AnalysisKeypoint::covariance_xx>(destination, -1).copy_(xx.masked_fill(~valid, nan));
 Product::select<^^runtime::AnalysisKeypoint::covariance_xy>(destination, -1).copy_(xy.masked_fill(~valid, nan));
 Product::select<^^runtime::AnalysisKeypoint::covariance_yy>(destination, -1).copy_(yy.masked_fill(~valid, nan));
 torch::from_blob(reinterpret_cast<void*>(annotation.keypoint_counts.address), {count}, counts.options()).copy_(counts);
 annotation.keypoints_available = true;
}
}  // namespace mmltk::backend::models::rfdetr
