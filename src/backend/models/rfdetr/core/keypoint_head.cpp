// RF-DETR GroupPose operations, pinned core e2b5f096, Apache-2.0.
#include "detail/keypoint_head.h"
#include "detail/linear.h"
namespace mmltk::backend::models::rfdetr::detail {
ConditionalQueryInitializer::ConditionalQueryInitializer(int64_t dimension, int64_t count, int64_t output_dimension) {
 queries_ = register_parameter("queries", torch::randn({count, output_dimension}));
 query_norm_ = register_module("query_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({output_dimension}).elementwise_affine(false)));
 modulation_ = register_module("adaLN_modulation", torch::nn::Sequential(torch::nn::Linear(dimension, dimension), torch::nn::GELU(), torch::nn::Linear(dimension, output_dimension * 3)));
 output_ = register_module("out_proj", torch::nn::Linear(output_dimension, output_dimension));
 torch::NoGradGuard guard;
 modulation_->at<torch::nn::LinearImpl>(2).weight.zero_();
 modulation_->at<torch::nn::LinearImpl>(2).bias.zero_();
}
torch::Tensor ConditionalQueryInitializer::forward(const torch::Tensor& condition) {
 const auto hidden = modulation_->at<torch::nn::GELUImpl>(1).forward(linear_forward(modulation_->at<torch::nn::LinearImpl>(0), condition.unsqueeze(-2)));
 const auto modulation = linear_forward(modulation_->at<torch::nn::LinearImpl>(2), hidden).chunk(3, -1);
 return linear_forward(*output_, (modulation[0] + 1.0) * query_norm_->forward(queries_) + modulation[1]) * modulation[2] + queries_;
}
}  // namespace mmltk::backend::models::rfdetr::detail
