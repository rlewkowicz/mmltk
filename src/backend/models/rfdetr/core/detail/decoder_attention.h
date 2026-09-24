#pragma once
#include <torch/torch.h>
#include <memory>
#include "src/backend/models/rfdetr/core/detection_types.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] torch::Tensor isolated_group_self_attention(torch::nn::MultiheadAttention& attention, const torch::Tensor& target, const torch::Tensor& query_position, const DecoderQueryLayout& layout);
namespace test_support {
// Test access to the actual sealed decoder tail; it introduces no alternate
// execution implementation or parameter inventory.
struct DecoderTailTestAccess {
 static std::shared_ptr<torch::nn::Module> make(int64_t width, int64_t feedforward);
 static torch::Tensor invoke(torch::nn::Module& layer, const torch::Tensor& residual, const torch::Tensor& cross, bool selective);
};
}  // namespace test_support
}  // namespace mmltk::backend::models::rfdetr
