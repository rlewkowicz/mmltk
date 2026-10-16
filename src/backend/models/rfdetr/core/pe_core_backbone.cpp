// Native projection of RF-DETR+ df2d02ae and timm 1.0.27 PE-Core-T.
// RF-DETR+ Copyright (c) 2026 Roboflow, Inc.; Platform Model License 1.0.
// timm Copyright Ross Wightman; Apache-2.0. See NOTICE and LICENSE.
#include "detail/backbone.h"
#include "detail/decoder_attention.h"
#include "detail/linear.h"
#include <torch/torch.h>
#include <torch/nn/functional.h>
#include <ATen/TensorIndexing.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr::detail {
namespace {
namespace F = torch::nn::functional;
using namespace torch::indexing;
torch::Tensor rotary_embedding(int64_t height, int64_t width, int64_t head_dim, torch::TensorOptions options) {
 // timm 1.0.27 uses reciprocal positive powers, xy indexing, grid_offset=1
 // and a 24x24 reference grid. Preserve those rounding boundaries.
 const auto bands = 1.0 / torch::pow(10000.0, torch::arange(head_dim / 4, options) / static_cast<double>(head_dim / 4));
 const auto xs = (torch::arange(width, options) + 1.0) / static_cast<double>(width) * 24.0;
 const auto ys = (torch::arange(height, options) + 1.0) / static_cast<double>(height) * 24.0;
 const auto grid = torch::stack({xs.unsqueeze(0).expand({height, width}), ys.unsqueeze(1).expand({height, width})}, -1);
 const auto angles = (grid.unsqueeze(-1) * bands).reshape({height * width, -1}).repeat_interleave(2, -1);
 return torch::cat({angles.sin(), angles.cos()}, -1);
}
class PeAttention final : public torch::nn::Module {
public:
 explicit PeAttention(const BackboneConfig& config) : heads_(config.heads), dimension_(config.dimension) {
  qkv = register_module("qkv", torch::nn::Linear(dimension_, dimension_ * 3));
  proj = register_module("proj", torch::nn::Linear(dimension_, dimension_));
 }
 torch::Tensor forward(const torch::Tensor& input, const torch::Tensor& rope) {
  const auto batch = input.size(0), tokens = input.size(1), width = dimension_ / heads_;
  const auto projected = linear_forward(*qkv, input).reshape({batch, tokens, 3, heads_, width}).permute({2, 0, 3, 1, 4});
  const auto rotate = [&](const torch::Tensor& value) {
   const auto rotated = torch::stack({-value.slice(-1, 1, c10::nullopt, 2), value.slice(-1, 0, c10::nullopt, 2)}, -1).reshape_as(value);
   return (value * rope.narrow(-1, width, width) + rotated * rope.narrow(-1, 0, width)).to(projected.scalar_type());
  };
  const auto attended = model_scaled_dot_product_attention(rotate(projected[0]), rotate(projected[1]), projected[2], {}, std::nullopt, force_math);
  return linear_forward(*proj, attended.transpose(1, 2).reshape({batch, tokens, dimension_}));
 }
 bool force_math = false;
 torch::nn::Linear qkv{nullptr}, proj{nullptr};

private:
 int64_t heads_, dimension_;
};
class PeMlp final : public torch::nn::Module {
public:
 explicit PeMlp(const BackboneConfig& config) {
  fc1 = register_module("fc1", torch::nn::Linear(config.dimension, config.feedforward_dimension));
  fc2 = register_module("fc2", torch::nn::Linear(config.feedforward_dimension, config.dimension));
 }
 torch::Tensor forward(const torch::Tensor& input) { return linear_forward(*fc2, torch::gelu(linear_forward(*fc1, input))); }
 torch::nn::Linear fc1{nullptr}, fc2{nullptr};
};
class PeBlock final : public torch::nn::Module {
public:
 explicit PeBlock(const BackboneConfig& config) {
  norm1 = register_module("norm1", torch::nn::LayerNorm(torch::nn::LayerNormOptions({config.dimension}).eps(1.e-5)));
  attn = register_module("attn", std::make_shared<PeAttention>(config));
  norm2 = register_module("norm2", torch::nn::LayerNorm(torch::nn::LayerNormOptions({config.dimension}).eps(1.e-5)));
  mlp = register_module("mlp", std::make_shared<PeMlp>(config));
 }
 torch::Tensor forward(const torch::Tensor& input, const torch::Tensor& rope) {
  const auto attended = input + attn->forward(norm1->forward(input), rope);
  return attended + mlp->forward(norm2->forward(attended));
 }
 torch::nn::LayerNorm norm1{nullptr}, norm2{nullptr};
 std::shared_ptr<PeAttention> attn;
 std::shared_ptr<PeMlp> mlp;
};
class PeTrunk final : public torch::nn::Module {
public:
 explicit PeTrunk(const NativeRfDetrConfig& config) : config_(config) {
  auto patch = register_module("patch_embed", std::make_shared<torch::nn::Module>());
  projection_ = patch->register_module("proj", torch::nn::Conv2d(torch::nn::Conv2dOptions(3, config.backbone.dimension, config.patch_size).stride(config.patch_size).bias(false)));
  cls_token_ = register_parameter("cls_token", torch::empty({1, 1, config.backbone.dimension}));
  pos_embed_ = register_parameter("pos_embed", torch::empty({1, config.positional_encoding_size * config.positional_encoding_size + 1, config.backbone.dimension}));
  norm_pre_ = register_module("norm_pre", torch::nn::LayerNorm(torch::nn::LayerNormOptions({config.backbone.dimension}).eps(1.e-5)));
  blocks_ = register_module("blocks", torch::nn::ModuleList());
  for (int64_t index = 0; index < config.backbone.layers; ++index) blocks_->push_back(std::make_shared<PeBlock>(config.backbone));
  norm_ = register_module("norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({config.backbone.dimension}).eps(1.e-5)));
  // Upstream constructs its nonpersistent stored-grid embedding on the CPU,
  // then moves it with the module. This derived cache is not checkpoint state.
  rope_ = rotary_embedding(config.positional_encoding_size, config.positional_encoding_size, config.backbone.dimension / config.backbone.heads, torch::TensorOptions().dtype(torch::kFloat32));
  torch::NoGradGuard guard;
  for (const auto& module : modules(false))
   if (auto* linear = dynamic_cast<torch::nn::LinearImpl*>(module.get())) {
    initialize_backbone_weight(linear->weight);
    if (linear->bias.defined()) linear->bias.zero_();
   }
  initialize_backbone_weight(pos_embed_);
  initialize_backbone_weight(cls_token_);
  for (size_t index = 0; index < blocks_->size(); ++index) {
   auto& block = blocks_->at<PeBlock>(index);
   const auto scale = std::sqrt(2.0 * static_cast<double>(index + 1));
   block.attn->proj->weight.div_(scale);
   block.mlp->fc2->weight.div_(scale);
  }
 }
 void to(torch::Device device, torch::Dtype dtype, bool non_blocking = false) override {
  torch::nn::Module::to(device, dtype, non_blocking);
  rope_ = rope_.to(device, dtype, non_blocking);
 }
 void to(torch::Dtype dtype, bool non_blocking = false) override {
  torch::nn::Module::to(dtype, non_blocking);
  rope_ = rope_.to(dtype, non_blocking);
 }
 void to(torch::Device device, bool non_blocking = false) override {
  torch::nn::Module::to(device, non_blocking);
  rope_ = rope_.to(device, non_blocking);
 }
 std::vector<torch::Tensor> forward(const torch::Tensor& pixels) {
  const int64_t windows = config_.num_windows, squares = windows * windows, dim = config_.backbone.dimension;
  const int64_t batch = pixels.size(0), h = pixels.size(2) / config_.patch_size, w = pixels.size(3) / config_.patch_size;
  if (pixels.size(2) % (config_.patch_size * windows) || pixels.size(3) % (config_.patch_size * windows)) throw std::invalid_argument("PE-Core-T input is not divisible by patch_size * num_windows");
  auto positions = pos_embed_;
  if (h != config_.positional_encoding_size || w != config_.positional_encoding_size) {
   auto grid = positions.narrow(1, 1, positions.size(1) - 1).reshape({1, config_.positional_encoding_size, config_.positional_encoding_size, dim}).permute({0, 3, 1, 2});
   grid = F::interpolate(grid.to(torch::kFloat32), F::InterpolateFuncOptions().size(std::vector<int64_t>{h, w}).mode(torch::kBicubic).align_corners(false).antialias(true));
   positions = torch::cat({positions.narrow(1, 0, 1), grid.permute({0, 2, 3, 1}).reshape({1, h * w, dim}).to(positions.scalar_type())}, 1);
  }
  auto tokens = projection_->forward(pixels).flatten(2).transpose(1, 2);
  tokens = norm_pre_->forward(torch::cat({cls_token_.expand({batch, -1, -1}), tokens}, 1) + positions);
  const auto options = pixels.options().dtype(torch::kFloat32);
  const int64_t head_dim = dim / config_.backbone.heads;
  auto rope = h == config_.positional_encoding_size && w == config_.positional_encoding_size ? rope_ : rotary_embedding(h, w, head_dim, options).to(rope_.scalar_type());
  auto no_op = torch::cat({torch::zeros({1, head_dim}, rope.options()), torch::ones({1, head_dim}, rope.options())}, -1);
  torch::Tensor full_rope, window_rope;
  if (windows > 1) {
   auto patches = tokens.narrow(1, 1, h * w).reshape({batch, windows, h / windows, windows, w / windows, dim}).permute({0, 1, 3, 2, 4, 5}).reshape({batch * squares, h * w / squares, dim});
   tokens = torch::cat({tokens.narrow(1, 0, 1).repeat_interleave(squares, 0), patches}, 1);
   rope = rope.reshape({windows, h / windows, windows, w / windows, head_dim * 2}).permute({0, 2, 1, 3, 4}).reshape({squares, h * w / squares, head_dim * 2});
   rope = torch::cat({no_op.unsqueeze(0).expand({squares, 1, head_dim * 2}), rope}, 1);
   full_rope = rope.reshape({-1, head_dim * 2});
   window_rope = rope.repeat({batch, 1, 1}).unsqueeze(1);
  } else
   full_rope = window_rope = torch::cat({no_op, rope}, 0);
  std::vector<torch::Tensor> features;
  features.reserve(config_.backbone.features.size());
  for (size_t index = 0; index <= config_.backbone.features.back(); ++index) {
   const bool selected = std::ranges::find(config_.backbone.features, index) != config_.backbone.features.end();
   const bool full = windows > 1 && selected;
   const auto window_tokens = tokens.size(1);
   if (full) tokens = tokens.reshape({batch, squares * window_tokens, dim});
   tokens = blocks_->at<PeBlock>(index).forward(tokens, full ? full_rope : window_rope);
   if (full) tokens = tokens.reshape({batch * squares, window_tokens, dim});
   if (selected) {
    auto output = norm_->forward(tokens).slice(1, 1);
    if (windows > 1) output = output.reshape({batch, windows, windows, h / windows, w / windows, dim}).permute({0, 1, 3, 2, 4, 5});
    features.push_back(output.reshape({batch, h, w, dim}).permute({0, 3, 1, 2}).contiguous());
   }
  }
  return features;
 }

private:
 NativeRfDetrConfig config_;
 torch::nn::Conv2d projection_{nullptr};
 torch::Tensor cls_token_, pos_embed_, rope_;
 torch::nn::LayerNorm norm_pre_{nullptr}, norm_{nullptr};
 torch::nn::ModuleList blocks_{nullptr};
};
class PeEncoder final : public BackboneEncoder {
public:
 explicit PeEncoder(const NativeRfDetrConfig& config) : model_(register_module("model", std::make_shared<PeTrunk>(config))) {}
 std::vector<torch::Tensor> forward(const torch::Tensor& pixels) override { return model_->forward(pixels); }
 void set_force_math_attention(bool value) override {
  for (const auto& module : modules(false))
   if (auto* attention = dynamic_cast<PeAttention*>(module.get())) attention->force_math = value;
 }

private:
 std::shared_ptr<PeTrunk> model_;
};
}  // namespace
std::shared_ptr<BackboneEncoder> make_pe_core_encoder(const NativeRfDetrConfig& config) { return std::make_shared<PeEncoder>(config); }
}  // namespace mmltk::backend::models::rfdetr::detail
