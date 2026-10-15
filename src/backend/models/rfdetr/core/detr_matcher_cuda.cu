#include "detail/detr_matcher_cuda.h"
#include "keypoint_prediction.h"
#include "src/backend/data/catalog/keypoint_visibility.h"
#include <cstddef>
#include "src/backend/ml/ops/box_geometry.cuh"
#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <torch/torch.h>
#include <cmath>
#include "src/frameworks/gpu/cuda/cuda_launch.cuh"
#include <initializer_list>
#include <limits>
namespace mmltk::backend::models::rfdetr {
__device__ __forceinline__ float stable_softplus(float value) { return fmaxf(value, 0.0f) + log1pf(expf(-fabsf(value))); }
__device__ __forceinline__ float cxcywh_generalized_iou(const float* lhs, const float* rhs) {
 float a[4], b[4];
 mmltk::backend::ml::ops::box_geometry::corners(lhs, a);
 mmltk::backend::ml::ops::box_geometry::corners(rhs, b);
 return mmltk::backend::ml::ops::box_geometry::pair(a, b).giou();
}
namespace {
constexpr auto kMatcherFloatNaN = std::numeric_limits<float>::quiet_NaN();
constexpr auto kMatcherFloatEpsilon = std::numeric_limits<float>::epsilon();
constexpr auto kMatcherFloatMaximum = std::numeric_limits<float>::max();
constexpr auto kMatcherFloatLowest = std::numeric_limits<float>::lowest();
struct MatcherPair final {
 int64_t batch_index, query_index, target_index, output_index;
};
__device__ __forceinline__ bool matcher_pair(int64_t index, const int64_t* target_offsets, const int64_t* target_counts, const int64_t* output_offsets, int64_t output_targets, int64_t batch_size,
 int64_t query_count, int64_t max_targets, int64_t total_targets, MatcherPair& pair) {
 const int64_t total = batch_size * query_count * max_targets;
 if (index >= total) { return false; }
 const int64_t target_slot = index % max_targets;
 const int64_t query_index = (index / max_targets) % query_count;
 const int64_t batch_index = index / (query_count * max_targets);
 const int64_t output_begin = output_offsets[batch_index];
 const int64_t output_end = batch_index + 1 < batch_size ? output_offsets[batch_index + 1] : output_targets;
 if (output_begin < 0 || output_end < output_begin || output_end > output_targets || target_slot >= output_end - output_begin) { return false; }
 const int64_t output_count = output_end - output_begin;
 if (target_slot >= target_counts[batch_index]) { return false; }
 const int64_t target_offset = target_offsets[batch_index];
 if (target_offset < 0 || target_slot >= total_targets || target_offset > total_targets - target_slot - 1) { return false; }
 const int64_t target_index = target_offset + target_slot;
 pair = {batch_index, query_index, target_index, query_count * output_begin + query_index * output_count + target_slot};
 return true;
}
}  // namespace
template <typename Logit, typename Box>
__global__ void matcher_cost_kernel(float* output, const Logit* pred_logits, const Box* pred_boxes, const int64_t* target_labels, const float* target_boxes, const int64_t* target_offsets,
 const int64_t* target_counts, const int64_t* output_offsets, int64_t output_targets, int64_t batch_size, int64_t query_count, int64_t class_count, int64_t max_targets, int64_t total_targets,
 float class_cost, float bbox_cost, float giou_cost, float focal_alpha) {
 const int64_t index = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
 MatcherPair pair;
 if (!matcher_pair(index, target_offsets, target_counts, output_offsets, output_targets, batch_size, query_count, max_targets, total_targets, pair)) { return; }
 const auto [batch_index, query_index, target_index, output_index] = pair;
 const int64_t class_index = target_labels[target_index];
 if (class_index < 0 || class_index >= class_count) { return; }
 const auto logit = static_cast<float>(pred_logits[(batch_index * query_count + query_index) * class_count + class_index]);
 const float probability = 1.0f / (1.0f + expf(-logit));
 const float one_minus_probability = 1.0f - probability;
 const float positive_class_cost = focal_alpha * one_minus_probability * one_minus_probability * stable_softplus(-logit);
 const float negative_class_cost = (1.0f - focal_alpha) * probability * probability * stable_softplus(logit);
 const Box* prediction = pred_boxes + (batch_index * query_count + query_index) * 4;
 const float* target = target_boxes + target_index * 4;
 const float prediction_float[4] = {static_cast<float>(prediction[0]), static_cast<float>(prediction[1]), static_cast<float>(prediction[2]), static_cast<float>(prediction[3])};
 const float l1 = fabsf(prediction_float[0] - target[0]) + fabsf(prediction_float[1] - target[1]) + fabsf(prediction_float[2] - target[2]) + fabsf(prediction_float[3] - target[3]);
 const float giou = cxcywh_generalized_iou(prediction_float, target);
 const float cost = class_cost * (positive_class_cost - negative_class_cost) + bbox_cost * l1 - giou_cost * giou;
 output[output_index] = cost;
}
// RF-DETR e2b5f096 keypoint matcher equations (Apache-2.0, Roboflow).
// One compact pair owns its active class row; no padded Cartesian joint tensor
// or device-to-host class inventory is needed.
template <typename Scalar>
__global__ void matcher_keypoint_cost_kernel(float* output, const Scalar* predictions, const float* targets, const int64_t* labels, const float* boxes, const int64_t* schema_counts,
 int64_t schema_slots, int64_t maximum_joints, int64_t target_joints, const int64_t* target_offsets, const int64_t* target_counts, const int64_t* output_offsets, int64_t output_targets, int64_t batch,
 int64_t queries, int64_t max_targets, int64_t total_targets, float l1_cost, float findable_cost, float visible_cost, float nll_cost) {
 const int64_t index = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
 MatcherPair pair;
 if (!matcher_pair(index, target_offsets, target_counts, output_offsets, output_targets, batch, queries, max_targets, total_targets, pair)) return;
 const auto label = labels[pair.target_index];
 if (label < 0 || label >= schema_slots || !schema_counts[label]) return;
 const auto joint_count = schema_counts[label];
 if (joint_count < 0 || joint_count > maximum_joints) {
  output[pair.output_index] = kMatcherFloatNaN;
  return;
 }
 constexpr auto width = sizeof(KeypointPrediction) / sizeof(float);
 const auto* prediction = predictions + ((pair.batch_index * queries + pair.query_index) * schema_slots + label) * maximum_joints * width;
 const auto* target = targets + pair.target_index * target_joints * 3;
 const auto area = boxes[pair.target_index * 4 + 2] * boxes[pair.target_index * 4 + 3];
 const float safe_area = isnan(area) ? area : fmaxf(area, kMatcherFloatEpsilon);
 float l1 = 0, nll = 0, findable_softplus = 0, findable_product = 0, visible_softplus = 0, visible_product = 0;
 int visible_count = 0;
 for (int64_t joint = 0; joint < joint_count; ++joint) {
  const auto* row = prediction + joint * width;
  const auto field = [row](std::size_t offset) { return static_cast<float>(row[offset / sizeof(float)]); };
  const float tx = joint < target_joints ? target[joint * 3] : 0, ty = joint < target_joints ? target[joint * 3 + 1] : 0;
  const float visibility = joint < target_joints ? target[joint * 3 + 2] : 0;
  const bool findable = visibility > static_cast<int>(mmltk::backend::data::catalog::KeypointVisibility::Unannotated);
  const bool visible = visibility > static_cast<int>(mmltk::backend::data::catalog::KeypointVisibility::Occluded);
  const float fl = field(offsetof(KeypointPrediction, findable_logit)), vl = field(offsetof(KeypointPrediction, visible_logit));
  // The reference performs softplus reduction minus a separate dot product.
  // Keep 0*infinity nonfinite so the existing matcher sentinel policy sees it.
  findable_softplus += fl > 20 ? fl : stable_softplus(fl);
  visible_softplus += vl > 20 ? vl : stable_softplus(vl);
  findable_product += fl * static_cast<float>(findable);
  visible_product += vl * static_cast<float>(visible);
  if (!(isfinite(visibility) && findable && isfinite(tx) && isfinite(ty) && isfinite(area) && area > kMatcherFloatEpsilon)) continue;
  ++visible_count;
  const float x = field(offsetof(KeypointPrediction, x)), y = field(offsetof(KeypointPrediction, y));
  const float dx = x - tx, dy = y - ty;
  l1 += fabsf(dx) + fabsf(dy);
  const float a = field(offsetof(KeypointPrediction, log_l11)), b = field(offsetof(KeypointPrediction, l21)), c = field(offsetof(KeypointPrediction, log_l22));
  const float u = expf(a) * dx + b * dy, v = expf(c) * dy, maha = u * u + v * v;
  if (!(isfinite(x) && isfinite(y) && isfinite(a) && isfinite(b) && isfinite(c) && isfinite(u) && isfinite(v) && isfinite(maha))) continue;
  const float value = .5f * maha / safe_area - (a + c);
  nll += isnan(value) ? 0 : isinf(value) ? (value > 0 ? kMatcherFloatMaximum / 2 : kMatcherFloatLowest) : value;
 }
 const auto denominator = static_cast<float>(visible_count > 0 ? visible_count : 1);
 l1 = l1 / denominator / sqrtf(safe_area);
 if (!isfinite(l1)) l1 = 0;
 const auto count = static_cast<float>(joint_count);
 const float findable = (findable_softplus - findable_product) / count, visible = (visible_softplus - visible_product) / count;
 output[pair.output_index] = output[pair.output_index] + l1_cost * l1 + findable_cost * findable + visible_cost * visible + nll_cost * (nll / denominator);
}
template <typename Logit>
__global__ void matcher_mask_cost_kernel(float* output, const Logit* pred_logits, const float* target_masks, const int64_t* target_offsets, const int64_t* target_counts, const int64_t* output_offsets,
 int64_t output_targets, int64_t batch_size, int64_t query_count, int64_t max_targets, int64_t total_targets, int64_t point_count, float ce_cost, float dice_cost) {
 constexpr int kWarpSize = 32;
 const int lane = static_cast<int>(threadIdx.x) & (kWarpSize - 1);
 const int64_t warp_index = (static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x) / static_cast<int64_t>(kWarpSize);
 MatcherPair pair;
 if (!matcher_pair(warp_index, target_offsets, target_counts, output_offsets, output_targets, batch_size, query_count, max_targets, total_targets, pair)) { return; }
 const auto [batch_index, query_index, target_index, output_index] = pair;
 const Logit* prediction = pred_logits + (batch_index * query_count + query_index) * point_count;
 const float* target = target_masks + target_index * point_count;
 float ce_sum = 0.0f;
 float intersection = 0.0f;
 float probability_sum = 0.0f;
 float target_sum = 0.0f;
 for (int64_t point = lane; point < point_count; point += kWarpSize) {
  const auto logit = static_cast<float>(prediction[point]);
  const float target_value = target[point];
  const float probability = 1.0f / (1.0f + expf(-logit));
  ce_sum += stable_softplus(-logit) * target_value + stable_softplus(logit) * (1.0f - target_value);
  intersection += probability * target_value;
  probability_sum += probability;
  target_sum += target_value;
 }
 for (int offset = kWarpSize / 2; offset > 0; offset /= 2) {
  ce_sum += __shfl_down_sync(0xffffffffU, ce_sum, offset);
  intersection += __shfl_down_sync(0xffffffffU, intersection, offset);
  probability_sum += __shfl_down_sync(0xffffffffU, probability_sum, offset);
  target_sum += __shfl_down_sync(0xffffffffU, target_sum, offset);
 }
 if (lane == 0) {
  const float ce = ce_sum / static_cast<float>(point_count);
  const float dice = 1.0f - (2.0f * intersection + 1.0f) / (probability_sum + target_sum + 1.0f);
  const float current = output[output_index];
  const float updated = current + ce_cost * ce + dice_cost * dice;
  output[output_index] = updated;
 }
}
void check_matcher_tensor(const torch::Tensor& tensor, c10::ScalarType dtype, int64_t dimensions, const char* name) {
 TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
 TORCH_CHECK(tensor.scalar_type() == dtype, name, " has an unexpected dtype");
 TORCH_CHECK(tensor.dim() == dimensions, name, " has an unexpected rank");
 TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
}
void check_matcher_floating_tensor(const torch::Tensor& tensor, int64_t dimensions, const char* name) {
 TORCH_CHECK(tensor.is_cuda(), name, " must be a CUDA tensor");
 TORCH_CHECK(at::isFloatingType(tensor.scalar_type()), name, " must use a floating dtype");
 TORCH_CHECK(tensor.dim() == dimensions, name, " has an unexpected rank");
 TORCH_CHECK(tensor.is_contiguous(), name, " must be contiguous");
}
int64_t checked_extent(const std::initializer_list<int64_t> factors, const int64_t limit, const char* context) {
 int64_t product = 1;
 for (const int64_t factor : factors) {
  TORCH_CHECK(factor >= 0 && (product == 0 || factor <= limit / product), context, " dimensions exceed the checked pointer/launch range");
  product *= factor;
 }
 return product;
}
namespace {
int64_t checked_output_targets(const torch::Tensor& output, const torch::Tensor& output_offsets, int64_t batch, int64_t queries, int64_t padded_queries, int64_t max_targets, const char* context) {
 (void)checked_extent({batch, padded_queries, max_targets}, std::numeric_limits<int64_t>::max() / sizeof(float), context);
 TORCH_CHECK(output_offsets.numel() == batch && (queries ? output.numel() % queries == 0 : output.numel() == 0), "matcher compact output layout does not cover predictions");
 const int64_t output_targets = queries ? output.numel() / queries : 0;
 TORCH_CHECK(output_targets <= checked_extent({batch, max_targets}, std::numeric_limits<int64_t>::max(), "matcher target extent"), "matcher compact target extent exceeds padded layout");
 return output_targets;
}
void check_matcher_offsets(const torch::Tensor& output, const torch::Tensor& target_offsets, const torch::Tensor& target_counts, const torch::Tensor& output_offsets) {
 check_matcher_tensor(target_offsets, torch::kInt64, 1, "matcher target offsets");
 check_matcher_tensor(target_counts, torch::kInt64, 1, "matcher target counts");
 check_matcher_tensor(output_offsets, torch::kInt64, 1, "matcher output offsets");
 TORCH_CHECK(output_offsets.device() == output.device(), "matcher output offsets must share the output device");
}
}  // namespace
float checked_cost_coefficient(const double value, const char* name) {
 TORCH_CHECK(std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max()), name, " must be finite and representable as float32");
 return static_cast<float>(value);
}
void pairwise_detection_cost_cuda_out(const torch::Tensor& output, const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes, const torch::Tensor& target_labels,
 const torch::Tensor& target_boxes, const torch::Tensor& target_offsets, const torch::Tensor& target_counts, const torch::Tensor& output_offsets, int64_t padded_queries, int64_t max_targets,
 double class_cost, double bbox_cost, double giou_cost, double focal_alpha) {
 check_matcher_tensor(output, torch::kFloat32, 1, "matcher cost output");
 check_matcher_floating_tensor(pred_logits, 3, "matcher logits");
 check_matcher_floating_tensor(pred_boxes, 3, "matcher boxes");
 check_matcher_tensor(target_labels, torch::kInt64, 1, "matcher target labels");
 check_matcher_tensor(target_boxes, torch::kFloat32, 2, "matcher target boxes");
 check_matcher_offsets(output, target_offsets, target_counts, output_offsets);
 TORCH_CHECK(output.device() == pred_logits.device() && output.device() == pred_boxes.device() && output.device() == target_labels.device() && output.device() == target_boxes.device() &&
              output.device() == target_offsets.device() && output.device() == target_counts.device(),
  "matcher CUDA tensors must share one device");
 TORCH_CHECK(pred_boxes.size(0) == pred_logits.size(0) && pred_boxes.size(1) == pred_logits.size(1) && pred_boxes.size(2) == 4, "matcher prediction boxes must be [batch, queries, 4]");
 TORCH_CHECK(padded_queries >= pred_logits.size(1), "matcher output shape does not cover predictions");
 TORCH_CHECK(target_boxes.size(0) == target_labels.size(0) && target_boxes.size(1) == 4, "matcher target boxes and labels disagree");
 TORCH_CHECK(target_offsets.size(0) == pred_logits.size(0) && target_counts.size(0) == pred_logits.size(0), "matcher target metadata does not match batch size");
 const int64_t batch = pred_logits.size(0);
 const int64_t queries = pred_logits.size(1);
 const int64_t classes = pred_logits.size(2);
 const int64_t total_targets = target_labels.size(0);
 TORCH_CHECK(checked_extent({batch, queries, classes}, std::numeric_limits<int64_t>::max(), "matcher logits pointer extent") == pred_logits.numel(), "matcher logits storage does not match its shape");
 TORCH_CHECK(
  checked_extent({batch, queries, 4}, std::numeric_limits<int64_t>::max(), "matcher prediction-box pointer extent") == pred_boxes.numel(), "matcher prediction-box storage does not match its shape");
 TORCH_CHECK(
  checked_extent({total_targets, 4}, std::numeric_limits<int64_t>::max(), "matcher target-box pointer extent") == target_boxes.numel(), "matcher target-box storage does not match its shape");
 const int64_t output_targets = checked_output_targets(output, output_offsets, batch, queries, padded_queries, max_targets, "matcher output pointer extent");
 const float checked_alpha = checked_cost_coefficient(focal_alpha, "matcher focal alpha");
 const float checked_class_cost = checked_cost_coefficient(class_cost, "matcher class cost");
 const float checked_bbox_cost = checked_cost_coefficient(bbox_cost, "matcher box cost");
 const float checked_giou_cost = checked_cost_coefficient(giou_cost, "matcher generalized-IoU cost");
 c10::cuda::CUDAGuard device_guard(output.device());
 const int64_t total = checked_extent({batch, queries, max_targets}, std::numeric_limits<int>::max(), "matcher detection cost");
 if (total == 0) { return; }
 const int threads = mmltk::frameworks::gpu::launch::kDefaultLinearThreads;
 const int blocks = mmltk::frameworks::gpu::launch::linear_blocks_for(static_cast<int>(total), threads);
 AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, pred_logits.scalar_type(), "pairwise_detection_cost_logits", [&] {
  using Logit = scalar_t;
  AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, pred_boxes.scalar_type(), "pairwise_detection_cost_boxes", [&] {
   using Box = scalar_t;
   matcher_cost_kernel<Logit, Box><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(output.data_ptr<float>(), pred_logits.data_ptr<Logit>(), pred_boxes.data_ptr<Box>(),
    target_labels.data_ptr<int64_t>(), target_boxes.data_ptr<float>(), target_offsets.data_ptr<int64_t>(), target_counts.data_ptr<int64_t>(), output_offsets.data_ptr<int64_t>(), output_targets, batch,
    queries, classes, max_targets, total_targets, checked_class_cost, checked_bbox_cost, checked_giou_cost, checked_alpha);
  });
 });
 TORCH_CHECK(cudaGetLastError() == cudaSuccess, "matcher cost CUDA kernel launch failed");
}
void pairwise_keypoint_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& predictions, const torch::Tensor& targets, const torch::Tensor& labels, const torch::Tensor& boxes,
 const torch::Tensor& schema_counts, const torch::Tensor& target_offsets, const torch::Tensor& target_counts, const torch::Tensor& output_offsets, int64_t padded_queries, int64_t max_targets,
 int64_t maximum_joints, double l1_cost, double findable_cost, double visible_cost, double nll_cost) {
 check_matcher_tensor(output, torch::kFloat32, 1, "matcher keypoint cost output");
 check_matcher_floating_tensor(predictions, 4, "matcher keypoint predictions");
 check_matcher_tensor(targets, torch::kFloat32, 3, "matcher keypoint targets");
 check_matcher_tensor(labels, torch::kInt64, 1, "matcher keypoint labels");
 check_matcher_tensor(boxes, torch::kFloat32, 2, "matcher keypoint boxes");
 check_matcher_tensor(schema_counts, torch::kInt64, 1, "matcher keypoint schema");
 check_matcher_offsets(output, target_offsets, target_counts, output_offsets);
 for (const auto& tensor : {predictions, targets, labels, boxes, schema_counts}) TORCH_CHECK(tensor.device() == output.device(), "matcher keypoint tensors must share one device");
 const auto batch = predictions.size(0), queries = predictions.size(1), total_targets = targets.size(0);
 TORCH_CHECK(maximum_joints > 0 && schema_counts.numel() > 0 && predictions.size(2) % maximum_joints == 0 && predictions.size(2) / maximum_joints == schema_counts.numel() &&
              predictions.size(3) == sizeof(KeypointPrediction) / sizeof(float) && targets.size(1) <= maximum_joints && targets.size(2) == 3 && labels.numel() == total_targets &&
              boxes.size(0) == total_targets && boxes.size(1) == 4 && target_offsets.numel() == batch && target_counts.numel() == batch && padded_queries >= queries,
  "matcher keypoint tensor layout differs");
 const auto output_targets = checked_output_targets(output, output_offsets, batch, queries, padded_queries, max_targets, "matcher keypoint output");
 const float l1 = checked_cost_coefficient(l1_cost, "matcher keypoint L1"), findable = checked_cost_coefficient(findable_cost, "matcher keypoint findability"),
             visible = checked_cost_coefficient(visible_cost, "matcher keypoint visibility"), nll = checked_cost_coefficient(nll_cost, "matcher keypoint NLL");
 c10::cuda::CUDAGuard guard(output.device());
 constexpr int threads = 256;
 const auto pairs = checked_extent({batch, queries, max_targets}, static_cast<int64_t>(std::numeric_limits<int>::max()) * threads, "matcher keypoint launch");
 if (!pairs) return;
 const auto blocks = static_cast<int>((pairs + threads - 1) / threads);
 AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, predictions.scalar_type(), "pairwise_keypoint_cost", [&] {
  matcher_keypoint_cost_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(output.data_ptr<float>(), predictions.data_ptr<scalar_t>(), targets.data_ptr<float>(),
   labels.data_ptr<int64_t>(), boxes.data_ptr<float>(), schema_counts.data_ptr<int64_t>(), schema_counts.numel(), maximum_joints, targets.size(1), target_offsets.data_ptr<int64_t>(),
   target_counts.data_ptr<int64_t>(), output_offsets.data_ptr<int64_t>(), output_targets, batch, queries, max_targets, total_targets, l1, findable, visible, nll);
 });
 TORCH_CHECK(cudaGetLastError() == cudaSuccess, "matcher keypoint cost CUDA kernel launch failed");
}
void pairwise_mask_cost_cuda_add_(const torch::Tensor& output, const torch::Tensor& pred_mask_logits, const torch::Tensor& target_masks, const torch::Tensor& target_offsets,
 const torch::Tensor& target_counts, const torch::Tensor& output_offsets, int64_t padded_queries, int64_t max_targets, double ce_cost, double dice_cost) {
 check_matcher_tensor(output, torch::kFloat32, 1, "matcher mask cost output");
 check_matcher_floating_tensor(pred_mask_logits, 3, "matcher sampled mask logits");
 check_matcher_tensor(target_masks, torch::kFloat32, 2, "matcher sampled target masks");
 check_matcher_offsets(output, target_offsets, target_counts, output_offsets);
 TORCH_CHECK(output.device() == pred_mask_logits.device() && output.device() == target_masks.device() && output.device() == target_offsets.device() && output.device() == target_counts.device(),
  "matcher mask CUDA tensors must share one device");
 TORCH_CHECK(padded_queries >= pred_mask_logits.size(1), "matcher mask output shape does not cover predictions");
 TORCH_CHECK(target_masks.size(1) == pred_mask_logits.size(2), "matcher sampled prediction and target masks disagree");
 TORCH_CHECK(target_offsets.size(0) == pred_mask_logits.size(0) && target_counts.size(0) == pred_mask_logits.size(0), "matcher mask batch metadata does not cover predictions");
 const int64_t batch = pred_mask_logits.size(0);
 const int64_t queries = pred_mask_logits.size(1);
 const int64_t points = pred_mask_logits.size(2);
 const int64_t total_targets = target_masks.size(0);
 TORCH_CHECK(checked_extent({batch, queries, points}, std::numeric_limits<int64_t>::max(), "matcher mask-logit pointer extent") == pred_mask_logits.numel(),
  "matcher mask-logit storage does not match its shape");
 TORCH_CHECK(
  checked_extent({total_targets, points}, std::numeric_limits<int64_t>::max(), "matcher target-mask pointer extent") == target_masks.numel(), "matcher target-mask storage does not match its shape");
 const int64_t output_targets = checked_output_targets(output, output_offsets, batch, queries, padded_queries, max_targets, "matcher mask output pointer extent");
 const float checked_ce_cost = checked_cost_coefficient(ce_cost, "matcher mask cross-entropy cost");
 const float checked_dice_cost = checked_cost_coefficient(dice_cost, "matcher mask dice cost");
 c10::cuda::CUDAGuard device_guard(output.device());
 constexpr int threads = 256;
 constexpr int warps_per_block = threads / 32;
 const int64_t pair_count = checked_extent({batch, queries, max_targets}, static_cast<int64_t>(std::numeric_limits<int>::max()) * warps_per_block, "matcher mask cost");
 if (pair_count == 0) { return; }
 const int blocks = static_cast<int>((pair_count + warps_per_block - 1) / warps_per_block);
 AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, pred_mask_logits.scalar_type(), "pairwise_mask_cost", [&] {
  matcher_mask_cost_kernel<scalar_t><<<blocks, threads, 0, at::cuda::getCurrentCUDAStream()>>>(output.data_ptr<float>(), pred_mask_logits.data_ptr<scalar_t>(), target_masks.data_ptr<float>(),
   target_offsets.data_ptr<int64_t>(), target_counts.data_ptr<int64_t>(), output_offsets.data_ptr<int64_t>(), output_targets, batch, queries, max_targets, total_targets, points, checked_ce_cost,
   checked_dice_cost);
 });
 TORCH_CHECK(cudaGetLastError() == cudaSuccess, "matcher mask cost CUDA kernel launch failed");
}
}  // namespace mmltk::backend::models::rfdetr
