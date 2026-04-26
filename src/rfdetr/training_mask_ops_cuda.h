#pragma once

#include <torch/torch.h>

namespace mmltk::rfdetr {

torch::Tensor matcher_point_sample_cuda_forward(const torch::Tensor& input, const torch::Tensor& point_coords,
                                                bool nearest);

torch::Tensor sample_packed_masks_cuda(const torch::Tensor& packed_mask_bits, int64_t height, int64_t width,
                                       const torch::Tensor& mask_indices, const torch::Tensor& point_coords);

}  // namespace mmltk::rfdetr
