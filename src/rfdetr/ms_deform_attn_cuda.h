#pragma once

#include <torch/torch.h>

#include <vector>

namespace mmltk::rfdetr {

torch::Tensor ms_deform_attn_cuda_forward(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                          const torch::Tensor& level_start_index,
                                          const torch::Tensor& sampling_locations,
                                          const torch::Tensor& attention_weights, int64_t im2col_step);

std::vector<torch::Tensor> ms_deform_attn_cuda_backward(const torch::Tensor& value, const torch::Tensor& spatial_shapes,
                                                        const torch::Tensor& level_start_index,
                                                        const torch::Tensor& sampling_locations,
                                                        const torch::Tensor& attention_weights,
                                                        const torch::Tensor& grad_output, int64_t im2col_step);

}  
