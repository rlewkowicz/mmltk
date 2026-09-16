#pragma once
#include <torch/nn/functional/vision.h>
#include <torch/torch.h>
namespace mmltk::backend::ml::layers {
using MsDeformTensor = torch::Tensor;
MsDeformTensor ms_deform_attn_cuda_autograd(const MsDeformTensor& value, const MsDeformTensor& spatial_shapes, const MsDeformTensor& level_start_index,
                                            const MsDeformTensor& sampling_locations, const MsDeformTensor& attention_weights, int64_t im2col_step);
MsDeformTensor ms_deform_attn_reference(const MsDeformTensor& value, const MsDeformTensor& spatial_shapes, const MsDeformTensor& sampling_locations,
                                        const MsDeformTensor& attention_weights);
}  // namespace mmltk::backend::ml::layers
namespace mmltk::backend::ml::layers::ms_deform_attn_detail {
using ::torch::kBilinear;
using ::torch::kZeros;
using ::torch::stack;
using ::torch::indexing::Slice;
using ::torch::nn::functional::grid_sample;
using ::torch::nn::functional::GridSampleFuncOptions;
}  // namespace mmltk::backend::ml::layers::ms_deform_attn_detail
