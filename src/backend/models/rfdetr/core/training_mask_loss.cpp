#include "detail/training_mask_loss.h"
#include "detail/detection_sampling.h"
#include <torch/nn/functional/loss.h>
#include <ATen/CPUGeneratorImpl.h>
#include <ATen/cuda/CUDAGeneratorImpl.h>
#include "detail/training_mask_ops_cuda.h"
#include "detail/matcher_workspace.h"
#include "runtime.h"
#include "detail/traced_loss_cache.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
namespace mmltk::backend::models::rfdetr {
namespace F = torch::nn::functional;
using namespace torch::indexing;
// The zero-query result of a sparse mask projection: no masks, but the spatial extent, dtype, and
// device the projection would have produced. Both projection entry points return this one shape.
torch::Tensor empty_sparse_pred_masks(const SparsePredMasks& sparse) {
 return torch::empty({0, sparse.spatial_features.size(-2), sparse.spatial_features.size(-1)}, torch::TensorOptions().dtype(sparse.spatial_features.dtype()).device(sparse.spatial_features.device()));
}
torch::Tensor matched_sparse_pred_masks_for_batch(const SparsePredMasks& sparse, int64_t batch_index, const torch::Tensor& query_indices) {
 if (query_indices.dim() != 1) { throw std::runtime_error("native RF-DETR sparse mask projection expects 1D query indices"); }
 if (query_indices.numel() == 0) { return empty_sparse_pred_masks(sparse); }
 auto device_query_indices = query_indices.to(sparse.query_features.device(), torch::kInt64, false, false);
 const auto batch_spatial_features = sparse.spatial_features.index({batch_index});
 auto batch_query_features = sparse.query_features.index({batch_index, device_query_indices});
 if (batch_query_features.scalar_type() != batch_spatial_features.scalar_type()) { batch_query_features = batch_query_features.to(batch_spatial_features.scalar_type()); }
 const auto bias = sparse.bias.to(batch_spatial_features.dtype());
 const auto flattened_spatial = batch_spatial_features.flatten(1).contiguous();
 return torch::matmul(batch_query_features, flattened_spatial).view({batch_query_features.size(0), batch_spatial_features.size(-2), batch_spatial_features.size(-1)}) + bias;
}
const PackedTargetMasks& require_target_masks(const PreparedTargets& targets, const char* context) {
 if (!targets.packed_masks.has_value() || !targets.packed_masks->bits.defined()) { throw std::runtime_error(std::string(context) + " requires target masks"); }
 return *targets.packed_masks;
}
void validate_packed_mask_extent(const PackedTargetMasks& masks, const char* context) {
 if (!masks.bits.defined() || masks.bits.dim() != 2 || masks.bits.scalar_type() != torch::kInt64)
  throw std::invalid_argument(std::string(context) + " requires a 2D int64 packed mask inventory");
 if (masks.height <= 0 || masks.width <= 0 || masks.height > std::numeric_limits<int64_t>::max() / masks.width)
  throw std::invalid_argument(std::string(context) + " has invalid packed mask dimensions");
 const auto pixels = masks.height * masks.width;
 if (masks.bits.size(1) < pixels / 64 + (pixels % 64 != 0))
  throw std::invalid_argument(std::string(context) + " packed mask words do not cover the image");
}
// Explicit samples belong to one invocation. The ordinary path draws only the
// original random tensor, with no additional storage or host inspection.
torch::Tensor mask_coordinates(const torch::Tensor& supplied, int64_t batch, int64_t count, const torch::Device& device) {
 if (!supplied.defined()) return torch::rand({batch, count, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
 if (supplied.device() != device || supplied.scalar_type() != torch::kFloat32 || supplied.sizes() != torch::IntArrayRef({batch, count, 2}))
  throw std::invalid_argument("explicit mask coordinates have an incompatible shape, device or dtype");
 return supplied;
}
namespace {
int64_t nearest_grid_sample_index(float coord, int64_t size) {
 const float grid = 2.0f * coord - 1.0f;
 const float source = ((grid + 1.0f) * static_cast<float>(size) - 1.0f) / 2.0f;
 const auto index = static_cast<int64_t>(std::nearbyint(source));
 return std::clamp<int64_t>(index, 0, size - 1);
}
torch::Tensor sample_packed_target_masks_cpu(const PackedTargetMasks& masks, const torch::Tensor& mask_indices, const torch::Tensor& point_coords, const char* context) {
 if (point_coords.dim() != 3 || point_coords.size(2) != 2) { throw std::runtime_error(std::string(context) + " expects point coordinates shaped [batch, points, 2]"); }
 if (!masks.bits.defined() || masks.bits.dim() != 2) { throw std::runtime_error(std::string(context) + " expects packed target masks shaped [instances, words]"); }
 if (masks.height <= 0 || masks.width <= 0) { throw std::runtime_error(std::string(context) + " requires positive packed mask dimensions"); }
 if (!point_coords.device().is_cpu()) { throw std::runtime_error(std::string(context) + " CPU packed mask sampling requires CPU point coordinates"); }
 if (!mask_indices.device().is_cpu() || mask_indices.scalar_type() != torch::kInt64 || mask_indices.dim() != 1) {
  throw std::runtime_error(std::string(context) + " CPU packed mask sampling requires 1D CPU int64 mask indices");
 }
 const auto masks_contiguous = masks.bits.contiguous();
 const auto indices = mask_indices.contiguous();
 const auto coords = point_coords.scalar_type() == torch::kFloat32 ? point_coords.contiguous() : point_coords.to(torch::kFloat32).contiguous();
 if (coords.size(0) != 1 && coords.size(0) != indices.size(0)) { throw std::runtime_error(std::string(context) + " point coordinate batch must match sampled masks or be shared"); }
 auto output = torch::empty({indices.size(0), coords.size(1)}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU));
 const auto* words = reinterpret_cast<const uint64_t*>(masks_contiguous.data_ptr<int64_t>());
 const auto* index_data = indices.data_ptr<int64_t>();
 const auto* coords_data = coords.data_ptr<float>();
 auto* output_data = output.data_ptr<float>();
 const int64_t words_per_mask = masks_contiguous.size(1);
 const int64_t num_points = coords.size(1);
 for (int64_t mask_slot = 0; mask_slot < indices.size(0); ++mask_slot) {
  const int64_t coord_batch = coords.size(0) == 1 ? 0 : mask_slot;
  const int64_t mask_index = index_data[mask_slot];
  if (mask_index < 0 || mask_index >= masks.bits.size(0)) throw std::invalid_argument(std::string(context) + " mask index is outside the packed inventory");
  const auto* mask_words = words + mask_index * words_per_mask;
  for (int64_t point_index = 0; point_index < num_points; ++point_index) {
   const auto* coord_ptr = coords_data + (coord_batch * num_points + point_index) * 2;
   const int64_t x = nearest_grid_sample_index(coord_ptr[0], masks.width);
   const int64_t y = nearest_grid_sample_index(coord_ptr[1], masks.height);
   const int64_t pixel_index = y * masks.width + x;
   const int64_t word_index = pixel_index >> 6;
   const int64_t bit_index = pixel_index & 63;
   output_data[mask_slot * num_points + point_index] = ((mask_words[word_index] >> bit_index) & uint64_t{1}) != 0 ? 1.0f : 0.0f;
  }
 }
 return output;
}
} // namespace
torch::Tensor sample_target_masks(const PackedTargetMasks& masks, const torch::Tensor& mask_indices, const torch::Tensor& point_coords, const char* context) {
 validate_packed_mask_extent(masks, context);
 const auto indices = mask_indices.scalar_type() == torch::kInt64 ? mask_indices.contiguous() : mask_indices.to(torch::kInt64).contiguous();
 const auto coords = point_coords.scalar_type() == torch::kFloat32 ? point_coords.contiguous() : point_coords.to(torch::kFloat32).contiguous();
 if (masks.bits.device() != coords.device()) { throw std::runtime_error(std::string(context) + " requires packed masks and point coordinates on the same device"); }
 if (masks.bits.device() != indices.device()) { throw std::runtime_error(std::string(context) + " requires packed masks and mask indices on the same device"); }
 if (masks.bits.device().is_cuda()) {
  return sample_packed_masks_cuda(masks.bits, masks.height, masks.width, indices, coords, masks.inverse_transforms, masks.occluder_mask_indices, masks.occluder_inverse_transforms, masks.erasure);
 }
 if (masks.inverse_transforms.defined() || masks.erasure.defined()) { throw std::runtime_error(std::string(context) + " cannot transform packed masks on CPU"); }
 return sample_packed_target_masks_cpu(masks, indices, coords, context);
}
namespace {
using BinaryLossFn = std::function<torch::Tensor(const torch::Tensor&, const torch::Tensor&)>;
torch::Tensor run_binary_traced_or_direct(
 TracedLossOp<2> TracedLossOpCache::* cache_member, const char* class_name, const bool use_jit_traced, const BinaryLossFn& direct_fn, const torch::Tensor& first, const torch::Tensor& second) {
 if (use_jit_traced) {
  auto* workspace = active_matcher_workspace();
  if (!workspace) throw std::logic_error("criterion trace requires an active matcher workspace");
  auto& slot = workspace->loss_cache().*cache_member;
  return slot.invoke(class_name, direct_fn, first, second);
 }
 return direct_fn(first, second);
}
}
torch::Tensor binary_cross_entropy_with_logits_none(const torch::Tensor& inputs, const torch::Tensor& targets) {
 return F::binary_cross_entropy_with_logits(inputs, targets, F::BinaryCrossEntropyWithLogitsFuncOptions().reduction(torch::kNone));
}
torch::Tensor sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& num_masks, bool use_jit_traced_loss_ops, const torch::Tensor& valid) {
 const auto rows = run_binary_traced_or_direct(
         &TracedLossOpCache::sigmoid_ce, "__torch__.NativeRfDetrSigmoidCeLoss", use_jit_traced_loss_ops,
         [](const torch::Tensor& a, const torch::Tensor& b) { return binary_cross_entropy_with_logits_none(a, b).mean(1); }, inputs, targets);
 return (valid.defined() ? torch::where(valid, rows, torch::zeros_like(rows)) : rows).sum() / num_masks;
}
torch::Tensor dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, const torch::Tensor& num_masks, bool use_jit_traced_loss_ops, const torch::Tensor& valid) {
 const auto rows = run_binary_traced_or_direct(
         &TracedLossOpCache::dice, "__torch__.NativeRfDetrDiceLoss", use_jit_traced_loss_ops,
         [](const torch::Tensor& a, const torch::Tensor& b) {
          const auto probs = a.sigmoid().flatten(1);
          const auto flat_targets = b.flatten(1);
          const auto numerator = 2 * (probs * flat_targets).sum(-1);
          const auto denominator = probs.sum(-1) + flat_targets.sum(-1);
          return 1 - (numerator + 1) / (denominator + 1);
         },
         inputs, targets);
 return (valid.defined() ? torch::where(valid, rows, torch::zeros_like(rows)) : rows).sum() / num_masks;
}
torch::Tensor batch_dice_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool use_jit_traced_loss_ops) {
 return run_binary_traced_or_direct(
  &TracedLossOpCache::batch_dice, "__torch__.NativeRfDetrBatchDiceLoss", use_jit_traced_loss_ops,
  [](const torch::Tensor& a, const torch::Tensor& b) {
   const auto probs = a.sigmoid();
   const auto flat_targets = b;
   const auto numerator = 2 * torch::matmul(probs, flat_targets.transpose(-1, -2));
   const auto denominator = probs.sum(-1).unsqueeze(-1) + flat_targets.sum(-1).unsqueeze(-2);
   return 1 - (numerator + 1) / (denominator + 1);
  },
  inputs, targets);
}
torch::Tensor batch_sigmoid_ce_loss(const torch::Tensor& inputs, const torch::Tensor& targets, bool use_jit_traced_loss_ops) {
 return run_binary_traced_or_direct(
  &TracedLossOpCache::batch_sigmoid_ce, "__torch__.NativeRfDetrBatchSigmoidCeLoss", use_jit_traced_loss_ops,
  [](const torch::Tensor& a, const torch::Tensor& b) {
   const auto flat_targets = b;
   const auto positives = binary_cross_entropy_with_logits_none(a, torch::ones_like(a));
   const auto negatives = binary_cross_entropy_with_logits_none(a, torch::zeros_like(a));
   return (torch::matmul(positives, flat_targets.transpose(-1, -2)) + torch::matmul(negatives, (1 - flat_targets).transpose(-1, -2)));
  },
  inputs, targets) / static_cast<double>(targets.size(-1));
}
torch::Tensor point_sample(const torch::Tensor& input, const torch::Tensor& point_coords, F::GridSampleFuncOptions::mode_t mode) {
 torch::Tensor grid = point_coords;
 bool add_dim = false;
 if (grid.dim() == 3) {
  add_dim = true;
  grid = grid.unsqueeze(2);
 }
 auto output = F::grid_sample(input, 2.0 * grid - 1.0, F::GridSampleFuncOptions().mode(mode).padding_mode(torch::kBorder).align_corners(false));
 if (add_dim) { output = output.squeeze(3); }
 return output;
}
namespace {
torch::Tensor get_uncertain_point_coords_with_randomness(const torch::Tensor& coarse_logits, int64_t num_points, const LayerMaskSamples& samples, const std::optional<DirectMaskRandomSeeds>& seeds) {
 const int64_t num_boxes = coarse_logits.size(0);
 const int64_t num_sampled = num_points * 3;
 const auto num_uncertain_points = static_cast<int64_t>(0.75 * static_cast<double>(num_points));
 const int64_t num_random_points = num_points - num_uncertain_points;
 std::optional<at::Generator> generator;
 const auto coordinates = [&](const torch::Tensor& supplied, int64_t count, std::uint64_t seed) {
  if (supplied.defined() || !seeds) return mask_coordinates(supplied, num_boxes, count, coarse_logits.device());
  // Missing draws share only this invocation's generator; each stream starts
  // from its own seed, independently of explicit operands and global RNG state.
  if (!generator) generator = coarse_logits.is_cuda()
   ? at::cuda::detail::createCUDAGenerator(coarse_logits.device().index()) : at::detail::createCPUGenerator();
  generator->set_current_seed(seed);
  return at::rand({num_boxes, count, 2}, *generator, coarse_logits.options().dtype(torch::kFloat32));
 };
 auto point_coords = coordinates(samples.uncertain_candidates, num_sampled, seeds ? seeds->candidates : 0);
 const auto point_logits = point_sample(coarse_logits, point_coords, torch::kBilinear);
 const auto point_uncertainties = -torch::abs(point_logits);
 torch::Tensor sampled_coords;
 if (num_uncertain_points > 0) {
  auto idx = std::get<1>(point_uncertainties.index({Slice(), 0, Slice()}).topk(num_uncertain_points, 1));
  const auto shift = num_sampled * torch::arange(num_boxes, torch::TensorOptions().dtype(torch::kInt64).device(coarse_logits.device()));
  idx = idx + shift.unsqueeze(1);
  sampled_coords = point_coords.view({-1, 2}).index({idx.reshape({-1})}).view({num_boxes, num_uncertain_points, 2});
 } else {
  sampled_coords = torch::empty({num_boxes, 0, 2}, torch::TensorOptions().dtype(torch::kFloat32).device(coarse_logits.device()));
 }
 if (num_random_points > 0) {
  sampled_coords = torch::cat(
   {
    sampled_coords,
    coordinates(samples.uncertain_random, num_random_points, seeds ? seeds->remainder : 0),
   },
   1);
 }
 return sampled_coords;
}

} // namespace

int64_t direct_mask_point_count(const torch::Tensor& masks, int64_t ratio) {
 if (ratio <= 0) throw std::invalid_argument("direct mask sampling requires a positive ratio");
 return std::max<int64_t>(masks.size(-2), masks.size(-2) * masks.size(-1) / ratio);
}
DirectMaskSamples sample_direct_masks(const torch::Tensor& masks, const PreparedTargets& targets,
 const torch::Tensor& indices, int64_t ratio, const LayerMaskSamples& samples, std::optional<DirectMaskRandomSeeds> seeds) {
 const auto logits = masks.unsqueeze(1);
 torch::Tensor coordinates;
 {
  torch::NoGradGuard no_grad;
  coordinates = get_uncertain_point_coords_with_randomness(logits, direct_mask_point_count(masks, ratio), samples, seeds);
 }
 const auto point_logits = point_sample(logits, coordinates, torch::kBilinear).squeeze(1);
 torch::Tensor labels;
 {
  torch::NoGradGuard no_grad;
  labels = sample_target_masks(require_target_masks(targets, "direct mask reconstruction"), indices, coordinates, "direct mask reconstruction").to(point_logits.dtype());
 }
 return {point_logits, labels};
}

PairwiseMaskSamples sample_pairwise_masks(const OutputLayer& layer, const PreparedTargets& targets,
 const torch::Tensor& indices, const torch::Tensor& valid, int64_t point_ratio, const torch::Tensor& coordinates) {
 if (!layer.sparse_pred_masks) throw std::invalid_argument("Match-Free masks require sparse spatial/query projections");
 const auto& sparse = *layer.sparse_pred_masks;
 const auto& spatial = sparse.spatial_features;
 if (point_ratio <= 0 || spatial.dim() != 4 || spatial.size(2) <= 0 || spatial.size(3) <= 0 || spatial.size(2) > std::numeric_limits<int64_t>::max() / spatial.size(3))
  throw std::invalid_argument("invalid Match-Free mask sampling dimensions or ratio");
 if (sparse.query_features.dim() != 3 || sparse.query_features.size(0) != spatial.size(0) || sparse.query_features.size(2) != spatial.size(1) ||
     indices.dim() != 2 || indices.size(0) != spatial.size(0) || indices.scalar_type() != torch::kInt64 || valid.sizes() != indices.sizes() || valid.scalar_type() != torch::kBool)
  throw std::invalid_argument("incompatible Match-Free mask projections or target layout");
 const auto& masks = require_target_masks(targets, "Match-Free");
 validate_packed_mask_extent(masks, "Match-Free");
 if (masks.bits.size(0) < targets.all_labels.size(0)) throw std::invalid_argument("Match-Free packed masks do not cover the target inventory");
 const auto points = std::max<int64_t>(1, spatial.size(2) * spatial.size(3) / point_ratio);
 const auto coords = mask_coordinates(coordinates, 1, points, spatial.device());
 auto features = point_sample(spatial.to(torch::kFloat32), coords.expand({spatial.size(0), points, 2}));
 auto logits = torch::bmm(sparse.query_features.to(torch::kFloat32), features) + sparse.bias.to(torch::kFloat32);
 auto sampled_targets = sample_target_masks(masks, indices.reshape({-1}), coords, "Match-Free").view({indices.size(0), indices.size(1), points});
 sampled_targets = torch::where(valid.unsqueeze(-1), sampled_targets, torch::zeros_like(sampled_targets));
 return {features, logits, sampled_targets};
}
} // namespace mmltk::backend::models::rfdetr
