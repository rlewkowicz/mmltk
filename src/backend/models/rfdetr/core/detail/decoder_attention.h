#pragma once
#include <torch/torch.h>
#include <memory>
#include <optional>
#include "src/backend/models/rfdetr/core/detection_types.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] torch::Tensor isolated_group_self_attention(
 torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& query_position, const DecoderQueryLayout& layout, bool force_math = false);
[[nodiscard]] torch::Tensor keypoint_self_attention(torch::nn::MultiheadAttention&, const torch::Tensor&, const torch::Tensor&, const torch::Tensor& excluded = {}, bool force_math = false);
// Local backend selection shared by backbone and decoder attention.
[[nodiscard]] torch::Tensor model_scaled_dot_product_attention(
 const torch::Tensor& query, const torch::Tensor& key, const torch::Tensor& value, const torch::Tensor& admitted_mask = {}, std::optional<double> scale = std::nullopt, bool force_math = false);
namespace test_support {
// Test access to the actual sealed decoder tail; it introduces no alternate
// execution implementation or parameter inventory.
struct DecoderTailTestAccess {
 static std::shared_ptr<torch::nn::Module> make(int64_t width, int64_t feedforward);
 static torch::Tensor invoke(torch::nn::Module& layer, const torch::Tensor& residual, const torch::Tensor& cross, bool selective);
};
}  // namespace test_support
}  // namespace mmltk::backend::models::rfdetr
