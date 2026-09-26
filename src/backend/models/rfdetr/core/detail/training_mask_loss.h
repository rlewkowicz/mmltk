#pragma once
#include <torch/torch.h>
#include <torch/nn/functional/vision.h>
#include <cstdint>
#include <optional>
#include "../detection_types.h"
namespace mmltk::backend::models::rfdetr {
struct LayerMaskSamples;
const PackedTargetMasks& require_target_masks(const PreparedTargets& targets, const char* context);
void validate_packed_mask_extent(const PackedTargetMasks& masks, const char* context);
torch::Tensor mask_coordinates(const torch::Tensor& supplied, int64_t batch, int64_t count, const torch::Device& device);
torch::Tensor sample_target_masks(const PackedTargetMasks& masks, const torch::Tensor& indices, const torch::Tensor& coordinates, const char* context);
torch::Tensor point_sample(const torch::Tensor& input, const torch::Tensor& coordinates, torch::nn::functional::GridSampleFuncOptions::mode_t mode = torch::kBilinear);
torch::Tensor binary_cross_entropy_with_logits_none(const torch::Tensor& inputs, const torch::Tensor& targets);
torch::Tensor sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& count, bool traced, const torch::Tensor& valid = {});
torch::Tensor dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& count, bool traced, const torch::Tensor& valid = {});
torch::Tensor batch_dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool traced);
torch::Tensor batch_sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool traced);
// Direct reconstruction materializes only selected queries before bilinear
// sampling. Pairwise matching deliberately uses a different operation order.
torch::Tensor empty_sparse_pred_masks(const SparsePredMasks& sparse);
torch::Tensor matched_sparse_pred_masks_for_batch(const SparsePredMasks& sparse, int64_t batch_index, const torch::Tensor& query_indices);
int64_t direct_mask_point_count(const torch::Tensor& masks, int64_t ratio);
struct DirectMaskSamples {
 torch::Tensor logits;
 torch::Tensor targets;
};
// Seeds apply only to omitted operands; omission uses the default generator.
struct DirectMaskRandomSeeds {
 std::uint64_t candidates;
 std::uint64_t remainder;
 torch::Tensor rows{};
};
DirectMaskSamples sample_direct_masks(
 const torch::Tensor& masks, const PreparedTargets& targets, const torch::Tensor& indices, int64_t ratio, const LayerMaskSamples& samples, std::optional<DirectMaskRandomSeeds> seeds = std::nullopt);
// One uniform draw shared by all image, query and target operands in a layer.
// Spatial and predicted samples retain autograd; categorical targets do not.
struct PairwiseMaskSamples {
 torch::Tensor spatial;  // [B, C, P]
 torch::Tensor logits;   // [B, Q, P]
 torch::Tensor targets;  // [B, M, P], padded rows zero
};
PairwiseMaskSamples sample_pairwise_masks(const OutputLayer& layer, const PreparedTargets& targets, const torch::Tensor& indices, const torch::Tensor& valid, int64_t point_ratio,
 const torch::Tensor& coordinates = {}, std::optional<std::uint64_t> sampling_key = std::nullopt);
}  // namespace mmltk::backend::models::rfdetr
