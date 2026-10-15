#pragma once
#include <cstdint>
#include <torch/torch.h>
namespace mmltk::backend::models::rfdetr::detail {
class ConditionalQueryInitializer final : public torch::nn::Module {
public:
 ConditionalQueryInitializer(int64_t dimension, int64_t count, int64_t output_dimension);
 [[nodiscard]] torch::Tensor forward(const torch::Tensor& condition);

private:
 torch::Tensor queries_;
 torch::nn::LayerNorm query_norm_{nullptr};
 torch::nn::Sequential modulation_{nullptr};
 torch::nn::Linear output_{nullptr};
};
}  // namespace mmltk::backend::models::rfdetr::detail
