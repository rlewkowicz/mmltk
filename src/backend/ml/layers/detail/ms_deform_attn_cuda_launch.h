#pragma once
#include <cuda_runtime.h>
#include <cstdint>
namespace mmltk::backend::ml::layers {
namespace ms_deform_attn_launch {
inline constexpr int kMaxLevels = 16;
struct DeviceLayout final {
 int spatial_shapes[kMaxLevels][2]{};
 int level_start_index[kMaxLevels]{};
};
struct LaunchPlan {
 int batch = 0;
 int im2col_step = 0;
 int chunk_count = 0;
 int batch_n = 0;
 int spatial_size = 0;
 int num_heads = 0;
 int channels = 0;
 int num_levels = 0;
 int num_query = 0;
 int num_point = 0;
 std::int64_t value_stride = 0;
 std::int64_t sampling_loc_stride = 0;
 std::int64_t attn_weight_stride = 0;
 std::int64_t output_stride = 0;
};
[[nodiscard]] inline LaunchPlan make_launch_plan(
 const int batch, const int spatial_size, const int num_heads, const int channels, const int num_levels, const int num_query, const int num_point, const int im2col_step) {
 return LaunchPlan{
  batch,
  im2col_step,
  batch / im2col_step,
  im2col_step,
  spatial_size,
  num_heads,
  channels,
  num_levels,
  num_query,
  num_point,
  static_cast<std::int64_t>(spatial_size) * num_heads * channels,
  static_cast<std::int64_t>(num_query) * num_heads * num_levels * num_point * 2,
  static_cast<std::int64_t>(num_query) * num_heads * num_levels * num_point,
  static_cast<std::int64_t>(num_query) * num_heads * channels,
 };
}  // namespace mmltk::backend::ml::layers
[[nodiscard]] inline std::int64_t batch_offset(const LaunchPlan& plan, const int batch_index, const std::int64_t stride) { return static_cast<std::int64_t>(batch_index) * plan.im2col_step * stride; }
struct CommonLaunch {
 const float* value = nullptr;
 DeviceLayout layout{};
 const float* sampling_loc = nullptr;
 const float* attn_weight = nullptr;
 int batch = 0;
 int spatial_size = 0;
 int num_heads = 0;
 int channels = 0;
 int num_levels = 0;
 int num_query = 0;
 int num_point = 0;
 int im2col_step = 0;
 cudaStream_t stream = nullptr;
};
[[nodiscard]] inline CommonLaunch make_common_launch(const float* value, const DeviceLayout& layout, const float* sampling_loc, const float* attn_weight, const int batch, const int spatial_size,
 const int num_heads, const int channels, const int num_levels, const int num_query, const int num_point, const int im2col_step, cudaStream_t stream) {
 return CommonLaunch{
  value,
  layout,
  sampling_loc,
  attn_weight,
  batch,
  spatial_size,
  num_heads,
  channels,
  num_levels,
  num_query,
  num_point,
  im2col_step,
  stream,
 };
}
struct ForwardLaunch : CommonLaunch {
 float* output = nullptr;
};
struct BackwardLaunch : CommonLaunch {
 const float* grad_output = nullptr;
 float* grad_value = nullptr;
 float* grad_sampling_loc = nullptr;
 float* grad_attn_weight = nullptr;
};
// Per-chunk pointers shared by the forward and backward kernels: the launch pointers advanced to batch_index.
struct CommonBatchArgs {
 const float* value = nullptr;
 DeviceLayout layout{};
 const float* sampling_loc = nullptr;
 const float* attn_weight = nullptr;
};
struct ForwardBatchArgs : CommonBatchArgs {
 float* output = nullptr;
};
struct BackwardBatchArgs : CommonBatchArgs {
 const float* grad_output = nullptr;
 float* grad_value = nullptr;
 float* grad_sampling_loc = nullptr;
 float* grad_attn_weight = nullptr;
};
[[nodiscard]] inline CommonBatchArgs make_common_batch_args(const CommonLaunch& launch, const LaunchPlan& plan, const int batch_index) {
 return CommonBatchArgs{
  launch.value + batch_offset(plan, batch_index, plan.value_stride),
  launch.layout,
  launch.sampling_loc + batch_offset(plan, batch_index, plan.sampling_loc_stride),
  launch.attn_weight + batch_offset(plan, batch_index, plan.attn_weight_stride),
 };
}
[[nodiscard]] inline ForwardBatchArgs make_forward_batch_args(const ForwardLaunch& launch, const LaunchPlan& plan, const int batch_index) {
 return ForwardBatchArgs{
  make_common_batch_args(launch, plan, batch_index),
  launch.output + batch_offset(plan, batch_index, plan.output_stride),
 };
}
[[nodiscard]] inline BackwardBatchArgs make_backward_batch_args(const BackwardLaunch& launch, const LaunchPlan& plan, const int batch_index) {
 return BackwardBatchArgs{
  make_common_batch_args(launch, plan, batch_index),
  launch.grad_output + batch_offset(plan, batch_index, plan.output_stride),
  launch.grad_value + batch_offset(plan, batch_index, plan.value_stride),
  launch.grad_sampling_loc + batch_offset(plan, batch_index, plan.sampling_loc_stride),
  launch.grad_attn_weight + batch_offset(plan, batch_index, plan.attn_weight_stride),
 };
}
}  // namespace ms_deform_attn_launch
void launch_ms_deform_attn_cuda_forward(const ms_deform_attn_launch::ForwardLaunch& launch);
void launch_ms_deform_attn_cuda_backward(const ms_deform_attn_launch::BackwardLaunch& launch);
}  // namespace mmltk::backend::ml::layers
