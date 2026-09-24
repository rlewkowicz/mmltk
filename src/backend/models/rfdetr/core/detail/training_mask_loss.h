#pragma once
#include <torch/torch.h>
#include <torch/nn/functional/vision.h>
#include "detection_sampling.h"
namespace mmltk::backend::models::rfdetr {
const PackedTargetMasks& require_target_masks(const PreparedTargets& targets, const char* context);
void validate_packed_mask_extent(const PackedTargetMasks& masks, const char* context);
torch::Tensor mask_coordinates(const torch::Tensor& supplied, int64_t batch, int64_t count, const torch::Device& device);
torch::Tensor sample_target_masks(const PackedTargetMasks& masks, const torch::Tensor& indices, const torch::Tensor& coordinates, const char* context);
torch::Tensor point_sample(const torch::Tensor& input, const torch::Tensor& coordinates, torch::nn::functional::GridSampleFuncOptions::mode_t mode = torch::kBilinear);
torch::Tensor binary_cross_entropy_with_logits_none(const torch::Tensor& inputs, const torch::Tensor& targets);
torch::Tensor sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& count, bool traced);
torch::Tensor dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& count, bool traced);
torch::Tensor batch_dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool traced);
torch::Tensor batch_sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool traced);
torch::Tensor get_uncertain_point_coords_with_randomness(const torch::Tensor& logits, int64_t points, int64_t oversample, double importance, const LayerMaskSamples& samples);
// One uniform draw shared by all image, query and target operands in a layer.
// Spatial and predicted samples retain autograd; categorical targets do not.
struct PairwiseMaskSamples {
 torch::Tensor spatial; // [B, C, P]
 torch::Tensor logits; // [B, Q, P]
 torch::Tensor targets; // [B, M, P], padded rows zero
};
PairwiseMaskSamples sample_pairwise_masks(const OutputLayer& layer, const PreparedTargets& targets,
 const torch::Tensor& indices, const torch::Tensor& valid, int64_t point_ratio, const torch::Tensor& coordinates = {});
} // namespace mmltk::backend::models::rfdetr
