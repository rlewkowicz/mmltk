#pragma once
#include <memory>
#include <inplace_vector>
#include <vector>
#include <torch/nn/module.h>
#include <torch/types.h>
#include "src/backend/models/rfdetr/contract/model_config.h"
namespace mmltk::backend::models::rfdetr::detail {
class BackboneEncoder : public torch::nn::Module {
public:
 virtual std::vector<torch::Tensor> forward(const torch::Tensor&) = 0;
 virtual void set_force_math_attention(bool) = 0;
};
void initialize_backbone_weight(torch::Tensor);
[[nodiscard]] std::shared_ptr<BackboneEncoder> make_pe_core_encoder(const NativeRfDetrConfig&);
class NativeBackboneImpl final : public torch::nn::Module {
public:
 explicit NativeBackboneImpl(const NativeRfDetrConfig&);
 [[nodiscard]] std::inplace_vector<torch::Tensor, 2> forward_features(const torch::Tensor&);
 void set_force_math_attention(bool);

private:
 std::shared_ptr<BackboneEncoder> encoder_;
 std::shared_ptr<torch::nn::Module> projector_, cross_projector_;
};
}  // namespace mmltk::backend::models::rfdetr::detail
