#include "detail/backbone.h"
#include "detail/modules_technical.h"
#include "detail/decoder_attention.h"
#include "detail/linear.h"
#include "detail/position_embedding.h"
#include <torch/nn/functional.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace F = torch::nn::functional;
namespace mmltk::backend::models::rfdetr {
namespace {
class LayerNorm2dImpl : public torch::nn::Module {
public:
 explicit LayerNorm2dImpl(int64_t normalized_shape, double eps = 1.0e-6)
     : weight(register_parameter("weight", torch::ones({normalized_shape}, torch::kFloat32))), bias(register_parameter("bias", torch::zeros({normalized_shape}, torch::kFloat32))), eps_(eps) {}
 torch::Tensor forward(const torch::Tensor& x) {
  auto y = x.permute({0, 2, 3, 1});
  y = F::layer_norm(y, F::LayerNormFuncOptions({y.size(3)}).weight(weight).bias(bias).eps(eps_));
  return y.permute({0, 3, 1, 2});
 }
 torch::Tensor weight;
 torch::Tensor bias;

private:
 double eps_ = 1.0e-6;
};
TORCH_MODULE(LayerNorm2d);
class ConvXImpl : public torch::nn::Module {
public:
 ConvXImpl(int64_t in_planes, int64_t out_planes, int64_t kernel = 3, int64_t stride = 1, int64_t groups = 1, bool layer_norm = false)
     : conv(register_module("conv", torch::nn::Conv2d(torch::nn::Conv2dOptions(in_planes, out_planes, kernel).stride(stride).padding(kernel / 2).groups(groups).bias(false)))),
       use_layer_norm_(layer_norm) {
  if (use_layer_norm_) {
   ln = register_module("bn", LayerNorm2d(out_planes));
  } else {
   bn = register_module("bn", torch::nn::BatchNorm2d(out_planes));
  }
 }
 torch::Tensor forward(const torch::Tensor& x) {
  auto out = conv->forward(x.contiguous());
  out = use_layer_norm_ ? ln->forward(out) : bn->forward(out);
  return torch::silu(out);
 }
 torch::nn::Conv2d conv{nullptr};

private:
 bool use_layer_norm_ = false;
 LayerNorm2d ln{nullptr};
 torch::nn::BatchNorm2d bn{nullptr};
};
TORCH_MODULE(ConvX);
class BottleneckImpl : public torch::nn::Module {
public:
 BottleneckImpl(int64_t c1, int64_t c2, bool shortcut = true, int64_t groups = 1, bool layer_norm = false)
     : cv1(register_module("cv1", ConvX(c1, c2, 3, 1, groups, layer_norm))), cv2(register_module("cv2", ConvX(c2, c2, 3, 1, groups, layer_norm))), add_(shortcut && c1 == c2) {}
 torch::Tensor forward(const torch::Tensor& x) {
  auto out = cv2->forward(cv1->forward(x));
  return add_ ? x + out : out;
 }

private:
 ConvX cv1{nullptr};
 ConvX cv2{nullptr};
 bool add_ = false;
};
TORCH_MODULE(Bottleneck);
class C2fImpl : public torch::nn::Module {
public:
 C2fImpl(int64_t c1, int64_t c2, int64_t num_blocks, bool layer_norm = false)
     : c_(c2 / 2),
       cv1(register_module("cv1", ConvX(c1, 2 * c_, 1, 1, 1, layer_norm))),
       cv2(register_module("cv2", ConvX((2 + num_blocks) * c_, c2, 1, 1, 1, layer_norm))),
       m(register_module("m", torch::nn::ModuleList())) {
  for (int64_t index = 0; index < num_blocks; ++index) { m->push_back(Bottleneck(c_, c_, false, 1, layer_norm)); }
 }
 torch::Tensor forward(const torch::Tensor& x) {
  auto stem = cv1->forward(x);
  std::vector<torch::Tensor> parts;
  parts.reserve(static_cast<size_t>(2 + m->size()));
  parts.push_back(stem.narrow(1, 0, c_));
  parts.push_back(stem.narrow(1, c_, c_));
  for (size_t index = 0; index < m->size(); ++index) { parts.push_back(m[index]->as<Bottleneck>()->forward(parts.back())); }
  return cv2->forward(torch::cat(parts, 1));
 }

private:
 int64_t c_ = 0;
 ConvX cv1{nullptr};
 ConvX cv2{nullptr};
 torch::nn::ModuleList m{nullptr};
};
TORCH_MODULE(C2f);
class Dinov2PatchEmbeddingsImpl : public torch::nn::Module {
public:
 Dinov2PatchEmbeddingsImpl(int64_t patch_size, int64_t dimension)
     : projection(register_module("projection", torch::nn::Conv2d(torch::nn::Conv2dOptions(3, dimension, patch_size).stride(patch_size).bias(true)))), patch_size_(patch_size) {}
 torch::Tensor forward(const torch::Tensor& pixel_values) { return projection->forward(pixel_values).flatten(2).transpose(1, 2); }
 torch::nn::Conv2d projection{nullptr};

private:
 int64_t patch_size_ = 0;
};
TORCH_MODULE(Dinov2PatchEmbeddings);
class WindowedDinov2EmbeddingsImpl : public torch::nn::Module {
 int64_t dimension_;

public:
 explicit WindowedDinov2EmbeddingsImpl(const NativeRfDetrConfig& config)
     : dimension_(config.backbone.dimension),
       cls_token(register_parameter("cls_token", torch::empty({1, 1, dimension_}, torch::kFloat32))),
       mask_token(register_parameter("mask_token", torch::zeros({1, dimension_}, torch::kFloat32))),
       position_embeddings(register_parameter("position_embeddings", torch::empty({1, config.positional_encoding_size * config.positional_encoding_size + 1, dimension_}, torch::kFloat32))),
       patch_embeddings(register_module("patch_embeddings", Dinov2PatchEmbeddings(config.patch_size, config.backbone.dimension))),
       patch_size_(config.patch_size),
       num_windows_(std::max<int64_t>(1, config.num_windows)) {}
 torch::Tensor interpolate_pos_encoding(const torch::Tensor& embeddings, int64_t height, int64_t width) const {
  const int64_t num_patches = embeddings.size(1) - 1;
  const int64_t num_positions = position_embeddings.size(1) - 1;
  if (num_patches == num_positions && height == width) { return position_embeddings; }
  const int64_t patch_height = height / patch_size_;
  const int64_t patch_width = width / patch_size_;
  const auto sqrt_num_positions = static_cast<int64_t>(std::llround(std::sqrt(static_cast<double>(num_positions))));
  return detail::interpolate_position_embedding(position_embeddings, sqrt_num_positions, patch_height, patch_width);
 }
 torch::Tensor forward(const torch::Tensor& pixel_values) {
  const int64_t batch_size = pixel_values.size(0);
  const int64_t height = pixel_values.size(2);
  const int64_t width = pixel_values.size(3);
  auto embeddings = patch_embeddings->forward(pixel_values.to(patch_embeddings->projection->weight.dtype()));
  auto cls_tokens = cls_token.expand({batch_size, 1, dimension_});
  embeddings = torch::cat({cls_tokens, embeddings}, 1);
  embeddings = embeddings + interpolate_pos_encoding(embeddings, height, width);
  if (num_windows_ > 1) {
   const int64_t num_h_patches = height / patch_size_;
   const int64_t num_w_patches = width / patch_size_;
   const int64_t num_h_patches_per_window = num_h_patches / num_windows_;
   const int64_t num_w_patches_per_window = num_w_patches / num_windows_;
   auto cls_token_with_pos_embed = embeddings.narrow(1, 0, 1);
   auto pixel_tokens_with_pos_embed = embeddings.narrow(1, 1, embeddings.size(1) - 1);
   pixel_tokens_with_pos_embed = pixel_tokens_with_pos_embed.view({batch_size, num_h_patches, num_w_patches, dimension_});
   auto windowed_pixel_tokens = pixel_tokens_with_pos_embed.reshape({batch_size * num_windows_, num_h_patches_per_window, num_windows_, num_w_patches_per_window, dimension_})
                                 .permute({0, 2, 1, 3, 4})
                                 .reshape({batch_size * num_windows_ * num_windows_, num_h_patches_per_window * num_w_patches_per_window, dimension_});
   auto windowed_cls_token_with_pos_embed = cls_token_with_pos_embed.repeat({num_windows_ * num_windows_, 1, 1});
   embeddings = torch::cat({windowed_cls_token_with_pos_embed, windowed_pixel_tokens}, 1);
  }
  return embeddings;
 }
 torch::Tensor cls_token;
 torch::Tensor mask_token;
 torch::Tensor position_embeddings;
 Dinov2PatchEmbeddings patch_embeddings{nullptr};

private:
 int64_t patch_size_ = 0;
 int64_t num_windows_ = 1;
};
TORCH_MODULE(WindowedDinov2Embeddings);
class Dinov2SelfAttentionImpl : public torch::nn::Module {
 int64_t dimension_;
 int64_t heads_;

public:
 bool force_math_attention_ = false;
 Dinov2SelfAttentionImpl(const BackboneConfig& config)
     : dimension_(config.dimension),
       heads_(config.heads),
       query(register_module("query", torch::nn::Linear(dimension_, dimension_))),
       key(register_module("key", torch::nn::Linear(dimension_, dimension_))),
       value(register_module("value", torch::nn::Linear(dimension_, dimension_))) {}
 torch::Tensor forward(const torch::Tensor& hidden_states) {
  const int64_t batch = hidden_states.size(0);
  const int64_t seq_len = hidden_states.size(1);
  const int64_t head_dim = dimension_ / heads_;
  auto reshape = [&](const torch::Tensor& tensor) { return tensor.view({batch, seq_len, heads_, head_dim}).permute({0, 2, 1, 3}); };
  auto query_layer = reshape(detail::linear_forward(*query, hidden_states));
  auto key_layer = reshape(detail::linear_forward(*key, hidden_states));
  auto value_layer = reshape(detail::linear_forward(*value, hidden_states));
  const double scale = 1.0 / std::sqrt(static_cast<double>(head_dim));
  auto context = model_scaled_dot_product_attention(query_layer, key_layer, value_layer, {}, scale, force_math_attention_);
  return context.permute({0, 2, 1, 3}).contiguous().view({batch, seq_len, dimension_});
 }

private:
 torch::nn::Linear query{nullptr};
 torch::nn::Linear key{nullptr};
 torch::nn::Linear value{nullptr};
};
TORCH_MODULE(Dinov2SelfAttention);
class Dinov2SelfOutputImpl : public torch::nn::Module {
 int64_t dimension_;

public:
 Dinov2SelfOutputImpl(const BackboneConfig& config) : dimension_(config.dimension), dense(register_module("dense", torch::nn::Linear(dimension_, dimension_))) {}
 torch::Tensor forward(const torch::Tensor& hidden_states) { return detail::linear_forward(*dense, hidden_states); }
 torch::nn::Linear dense{nullptr};
};
TORCH_MODULE(Dinov2SelfOutput);
class Dinov2AttentionImpl : public torch::nn::Module {
public:
 Dinov2AttentionImpl(const BackboneConfig& config) : attention(register_module("attention", Dinov2SelfAttention(config))), output(register_module("output", Dinov2SelfOutput(config))) {}
 torch::Tensor forward(const torch::Tensor& hidden_states) { return output->forward(attention->forward(hidden_states)); }

private:
 Dinov2SelfAttention attention{nullptr};
 Dinov2SelfOutput output{nullptr};
};
TORCH_MODULE(Dinov2Attention);
class Dinov2LayerScaleImpl : public torch::nn::Module {
 int64_t dimension_;

public:
 Dinov2LayerScaleImpl(const BackboneConfig& config) : dimension_(config.dimension), lambda1(register_parameter("lambda1", torch::ones({dimension_}, torch::kFloat32))) {}
 torch::Tensor forward(const torch::Tensor& hidden_state) const { return hidden_state * lambda1; }
 torch::Tensor lambda1;
};
TORCH_MODULE(Dinov2LayerScale);
class Dinov2MlpImpl : public torch::nn::Module {
 int64_t dimension_;

public:
 Dinov2MlpImpl(const BackboneConfig& config)
     : dimension_(config.dimension),
       fc1(register_module("fc1", torch::nn::Linear(dimension_, config.feedforward_dimension))),
       fc2(register_module("fc2", torch::nn::Linear(config.feedforward_dimension, dimension_))) {}
 torch::Tensor forward(const torch::Tensor& hidden_state) { return detail::linear_forward(*fc2, torch::gelu(detail::linear_forward(*fc1, hidden_state))); }

private:
 torch::nn::Linear fc1{nullptr};
 torch::nn::Linear fc2{nullptr};
};
TORCH_MODULE(Dinov2Mlp);
class WindowedDinov2LayerImpl : public torch::nn::Module {
 int64_t dimension_;

public:
 explicit WindowedDinov2LayerImpl(const NativeRfDetrConfig& config)
     : dimension_(config.backbone.dimension),
       num_windows_(std::max<int64_t>(1, config.num_windows)),
       norm1(register_module("norm1", torch::nn::LayerNorm(torch::nn::LayerNormOptions({dimension_}).eps(1.0e-6)))),
       attention(register_module("attention", Dinov2Attention(config.backbone))),
       layer_scale1(register_module("layer_scale1", Dinov2LayerScale(config.backbone))),
       norm2(register_module("norm2", torch::nn::LayerNorm(torch::nn::LayerNormOptions({dimension_}).eps(1.0e-6)))),
       mlp(register_module("mlp", Dinov2Mlp(config.backbone))),
       layer_scale2(register_module("layer_scale2", Dinov2LayerScale(config.backbone))) {}
 torch::Tensor forward(const torch::Tensor& input, bool run_full_attention) {
  auto hidden_states = input;
  const auto shortcut = hidden_states;
  const int64_t original_batch = hidden_states.size(0);
  const int64_t original_hw = hidden_states.size(1);
  if (run_full_attention && num_windows_ > 1) {
   const int64_t num_windows_squared = num_windows_ * num_windows_;
   hidden_states = hidden_states.view({original_batch / num_windows_squared, num_windows_squared * original_hw, dimension_});
  }
  auto attention_output = attention->forward(norm1->forward(hidden_states));
  if (run_full_attention && num_windows_ > 1) { attention_output = attention_output.view({original_batch, original_hw, dimension_}); }
  hidden_states = shortcut + layer_scale1->forward(attention_output);
  auto layer_output = layer_scale2->forward(mlp->forward(norm2->forward(hidden_states)));
  return hidden_states + layer_output;
 }

private:
 int64_t num_windows_ = 1;
 torch::nn::LayerNorm norm1{nullptr};
 Dinov2Attention attention{nullptr};
 Dinov2LayerScale layer_scale1{nullptr};
 torch::nn::LayerNorm norm2{nullptr};
 Dinov2Mlp mlp{nullptr};
 Dinov2LayerScale layer_scale2{nullptr};
};
TORCH_MODULE(WindowedDinov2Layer);
class WindowedDinov2EncoderImpl : public torch::nn::Module {
 BackboneConfig topology_;

public:
 explicit WindowedDinov2EncoderImpl(const NativeRfDetrConfig& config) : topology_(config.backbone), layer(register_module("layer", torch::nn::ModuleList())) {
  for (int64_t index = 0; index < topology_.layers; ++index) { layer->push_back(WindowedDinov2Layer(config)); }
 }
 std::vector<torch::Tensor> forward(const torch::Tensor& hidden_states_in) {
  auto hidden_states = hidden_states_in;
  std::vector<torch::Tensor> all_hidden_states;
  all_hidden_states.reserve(static_cast<size_t>(topology_.layers + 1));
  for (int64_t index = 0; index < topology_.layers; ++index) {
   all_hidden_states.push_back(hidden_states);
   hidden_states = layer[index]->as<WindowedDinov2Layer>()->forward(hidden_states, std::ranges::find(topology_.features, index) != topology_.features.end());
  }
  all_hidden_states.push_back(hidden_states);
  return all_hidden_states;
 }

private:
 torch::nn::ModuleList layer{nullptr};
};
TORCH_MODULE(WindowedDinov2Encoder);
class WindowedDinov2BackboneImpl : public torch::nn::Module {
 BackboneConfig topology_;
 int64_t dimension_;

public:
 explicit WindowedDinov2BackboneImpl(const NativeRfDetrConfig& config)
     : topology_(config.backbone),
       dimension_(config.backbone.dimension),
       patch_size_(config.patch_size),
       num_windows_(std::max<int64_t>(1, config.num_windows)),
       embeddings(register_module("embeddings", WindowedDinov2Embeddings(config))),
       encoder(register_module("encoder", WindowedDinov2Encoder(config))),
       layernorm(register_module("layernorm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({dimension_}).eps(1.0e-6)))) {
  torch::NoGradGuard no_grad;
  for (const auto& module : modules(false)) {
   if (auto* linear = dynamic_cast<torch::nn::LinearImpl*>(module.get())) {
    detail::initialize_backbone_weight(linear->weight);
    if (linear->bias.defined()) linear->bias.zero_();
   } else if (auto* convolution = dynamic_cast<torch::nn::Conv2dImpl*>(module.get())) {
    detail::initialize_backbone_weight(convolution->weight);
    if (convolution->bias.defined()) convolution->bias.zero_();
   } else if (auto* norm = dynamic_cast<torch::nn::LayerNormImpl*>(module.get())) {
    norm->weight.fill_(1.0);
    norm->bias.zero_();
   }
  }
  detail::initialize_backbone_weight(embeddings->position_embeddings);
  detail::initialize_backbone_weight(embeddings->cls_token);
 }
 std::vector<torch::Tensor> forward(const torch::Tensor& pixel_values) {
  auto hidden_states = encoder->forward(embeddings->forward(pixel_values));
  const int64_t batch_size = pixel_values.size(0);
  const int64_t num_h_patches = pixel_values.size(2) / patch_size_;
  const int64_t num_w_patches = pixel_values.size(3) / patch_size_;
  const int64_t num_windows_squared = num_windows_ * num_windows_;
  const int64_t num_h_patches_per_window = num_h_patches / num_windows_;
  const int64_t num_w_patches_per_window = num_w_patches / num_windows_;
  std::vector<torch::Tensor> feature_maps;
  feature_maps.reserve(topology_.features.size());
  for (const auto stage : topology_.features) {
   auto hidden_state = layernorm->forward(hidden_states[static_cast<size_t>(stage)]);
   hidden_state = hidden_state.slice(1, 1, c10::nullopt);
   if (num_windows_ > 1) {
    hidden_state = hidden_state.reshape({batch_size, num_windows_squared, hidden_state.size(1), dimension_});
    hidden_state = hidden_state.reshape({batch_size * num_windows_, num_windows_, num_h_patches_per_window, num_w_patches_per_window, dimension_});
    hidden_state = hidden_state.permute({0, 2, 1, 3, 4});
   }
   hidden_state = hidden_state.reshape({batch_size, num_h_patches, num_w_patches, dimension_});
   feature_maps.push_back(hidden_state.permute({0, 3, 1, 2}).contiguous());
  }
  return feature_maps;
 }

private:
 int64_t patch_size_ = 0;
 int64_t num_windows_ = 1;
 WindowedDinov2Embeddings embeddings{nullptr};
 WindowedDinov2Encoder encoder{nullptr};
 torch::nn::LayerNorm layernorm{nullptr};
};
TORCH_MODULE(WindowedDinov2Backbone);
class DinoV2WrapperImpl final : public detail::BackboneEncoder {
public:
 explicit DinoV2WrapperImpl(const NativeRfDetrConfig& config) : encoder(register_module("encoder", WindowedDinov2Backbone(config))) {}
 std::vector<torch::Tensor> forward(const torch::Tensor& tensors) override { return encoder->forward(tensors); }
 void set_force_math_attention(bool value) override {
  for (const auto& module : modules(false))
   if (auto* attention = dynamic_cast<Dinov2SelfAttentionImpl*>(module.get())) attention->force_math_attention_ = value;
 }

private:
 WindowedDinov2Backbone encoder{nullptr};
};
class NativeBackboneProjectorImpl : public torch::nn::Module {
public:
 explicit NativeBackboneProjectorImpl(const NativeRfDetrConfig& config) : stages(register_module("stages", torch::nn::ModuleList())) {
  auto stage = torch::nn::Sequential();
  stage->push_back(C2f(config.backbone.dimension * static_cast<int64_t>(config.backbone.features.size()), config.hidden_dim, 3, true));
  stage->push_back(LayerNorm2d(config.hidden_dim));
  stages->push_back(stage);
 }
 std::vector<torch::Tensor> forward(const std::vector<torch::Tensor>& inputs) {
  if (inputs.empty()) { throw std::runtime_error("RF-DETR projector requires at least one input"); }
  return {stages[0]->as<torch::nn::Sequential>()->forward(torch::cat(inputs, 1))};
 }

private:
 torch::nn::ModuleList stages{nullptr};
};
TORCH_MODULE(NativeBackboneProjector);
}  // namespace
std::shared_ptr<torch::nn::Module> test_support::ProjectorConvTestAccess::make(int64_t channels) { return std::make_shared<ConvXImpl>(channels, channels, 1, 1, 1, true); }
torch::Tensor test_support::ProjectorConvTestAccess::invoke(torch::nn::Module& module, const torch::Tensor& input) { return dynamic_cast<ConvXImpl&>(module).forward(input); }
namespace detail {
void initialize_backbone_weight(torch::Tensor tensor) {
 // DINO and timm use trunc_normal_(std=.02, a=-2, b=2), with absolute
 // truncation bounds. The normal-CDF endpoints round to zero and one.
 tensor.uniform_(-1.0, 1.0).erfinv_().mul_(0.02 * std::sqrt(2.0)).clamp_(-2.0, 2.0);
}
std::shared_ptr<BackboneEncoder> make_backbone_encoder(const NativeRfDetrConfig& config) {
 return config.backbone.kind == BackboneKind::PeCoreTiny ? make_pe_core_encoder(config) : std::make_shared<DinoV2WrapperImpl>(config);
}
NativeBackboneImpl::NativeBackboneImpl(const NativeRfDetrConfig& config) {
 encoder_ = register_module("encoder", make_backbone_encoder(config));
 projector_ = register_module("projector", std::make_shared<NativeBackboneProjectorImpl>(config));
 if (config.projector == ProjectorKind::DualKeypointOnly) cross_projector_ = register_module("cross_attn_projector", std::make_shared<NativeBackboneProjectorImpl>(config));
}
std::inplace_vector<torch::Tensor, 2> NativeBackboneImpl::forward_features(const torch::Tensor& pixels) {
 const auto encoded = encoder_->forward(pixels);
 std::inplace_vector<torch::Tensor, 2> result{std::static_pointer_cast<NativeBackboneProjectorImpl>(projector_)->forward(encoded).front()};
 if (cross_projector_) result.push_back(std::static_pointer_cast<NativeBackboneProjectorImpl>(cross_projector_)->forward(encoded).front());
 return result;
}
void NativeBackboneImpl::set_force_math_attention(bool value) { encoder_->set_force_math_attention(value); }
NormalizedModelStateCandidate NativeBackboneImpl::stage_encoder_pretraining(const NativeRfDetrConfig& config, const std::vector<NormalizedModelStateEntry>& source) {
 return detail::stage_encoder_pretraining(*encoder_, config, source);
}
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
