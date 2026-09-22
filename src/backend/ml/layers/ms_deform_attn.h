#pragma once
#include <torch/types.h>
#include <cstdint>
namespace mmltk::backend::ml::layers {
torch::Tensor ms_deform_attn_cuda_autograd(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& level_start_index, const torch::Tensor& sampling_locations,
 const torch::Tensor& attention_weights, int64_t im2col_step);
torch::Tensor ms_deform_attn_reference(const torch::Tensor& value, const torch::Tensor& spatial_shapes, const torch::Tensor& sampling_locations, const torch::Tensor& attention_weights);
}  // namespace mmltk::backend::ml::layers
