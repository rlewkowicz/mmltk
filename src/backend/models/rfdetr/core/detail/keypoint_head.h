#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <utility>
#include <torch/torch.h>
#include "src/backend/models/rfdetr/core/class_layout.h"
namespace mmltk::backend::models::rfdetr::detail {
inline constexpr int64_t kKeypointPredictionDimension = 8;
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
[[nodiscard]] int64_t compact_keypoint_count(std::span<const std::uint16_t> counts);
[[nodiscard]] torch::Tensor keypoint_active_mask(std::span<const std::uint16_t> counts);
[[nodiscard]] torch::Tensor keypoint_class_mask(std::span<const std::uint16_t> counts);
[[nodiscard]] torch::Tensor refine_keypoints(const torch::Tensor& delta, const torch::Tensor& references);
[[nodiscard]] torch::Tensor padded_keypoints(const torch::Tensor& compact, std::span<const std::uint16_t> counts);
[[nodiscard]] torch::Tensor keypoint_class_logits(const torch::Tensor& padded, const torch::Tensor& active, int64_t classes);
[[nodiscard]] std::vector<std::pair<int64_t, int64_t>> keypoint_transfer_rows(const ResolvedClassLayout&, std::span<const std::uint16_t>, const ResolvedClassLayout&, std::span<const std::uint16_t>);
void validate_keypoint_layout(const ResolvedClassLayout&, std::span<const std::uint16_t> counts);
[[nodiscard]] std::vector<std::uint16_t> keypoint_counts_from_mask(const torch::Tensor& mask);
}
