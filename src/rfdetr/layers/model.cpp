#include "fastloader/rfdetr/model.h"

#include "fastloader/rfdetr/checkpoint.h"
#include "fastloader/rfdetr/model_config.h"
#include "fastloader/rfdetr/modules.h"
#include "fastloader/rfdetr/weight_catalog.h"
#include "profile_utils.h"
#include "rfdetr/onnx_lowering.h"
#include "rfdetr/onnx_simplify.h"
#include "rfdetr/ms_deform_attn_op.h"
#include "common_utils.h"

#include <ATen/ops/scaled_dot_product_attention.h>
#include <ATen/TensorIndexing.h>
#include <torch/nn/functional.h>
#include <torch/script.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/onnx/onnx.h>

#include <algorithm>
#include <fstream>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace F = torch::nn::functional;

namespace fastloader::rfdetr {

namespace {

using namespace torch::indexing;

constexpr int64_t kDinoHiddenSize = 384;
constexpr int64_t kDinoNumLayers = 12;
constexpr int64_t kDinoNumHeads = 6;
constexpr int64_t kProjectorBlocks = 3;
constexpr std::array<int64_t, 4> kOutFeatureStages = {3, 6, 9, 12};

void erase_unused_module_self_input(const std::shared_ptr<torch::jit::Graph>& graph) {
    if (!graph || graph->inputs().empty()) {
        return;
    }
    auto* self_input = graph->inputs().front();
    const auto class_type = self_input->type()->cast<c10::ClassType>();
    if (!class_type) {
        return;
    }
    if (self_input->hasUses()) {
        throw std::runtime_error(
            "native ONNX export left the module self input live after lowering: " +
            self_input->debugName());
    }
    graph->eraseInput(0);
}

struct LinearOutputState {
    torch::Tensor weight;
    torch::Tensor bias;
};

struct DetectionHeadState {
    LinearOutputState class_embed;
    std::vector<LinearOutputState> encoder_class_embeds;
};

struct NativeTransformerOutput {
    torch::Tensor hidden_states;
    torch::Tensor refs_unsigmoid;
    torch::Tensor enc_memory;
    torch::Tensor enc_boxes;
};

LinearOutputState capture_linear_output_state(const torch::nn::LinearImpl& linear) {
    LinearOutputState state;
    state.weight = linear.weight.detach().cpu().clone();
    if (linear.bias.defined()) {
        state.bias = linear.bias.detach().cpu().clone();
    }
    return state;
}

void restore_linear_output_state(torch::nn::LinearImpl& linear, const LinearOutputState& state) {
    if (linear.weight.sizes() != state.weight.sizes()) {
        throw std::runtime_error("RF-DETR linear weight restore shape mismatch");
    }
    if (linear.bias.defined() != state.bias.defined()) {
        throw std::runtime_error("RF-DETR linear bias restore defined-state mismatch");
    }

    torch::NoGradGuard no_grad;
    linear.weight.copy_(state.weight.to(linear.weight.device(), linear.weight.scalar_type()));
    if (linear.bias.defined()) {
        if (linear.bias.sizes() != state.bias.sizes()) {
            throw std::runtime_error("RF-DETR linear bias restore shape mismatch");
        }
        linear.bias.copy_(state.bias.to(linear.bias.device(), linear.bias.scalar_type()));
    }
}

torch::Tensor resize_repeated_rows(const torch::Tensor& tensor, int64_t rows) {
    if (!tensor.defined()) {
        return tensor;
    }
    if (rows <= 0) {
        throw std::runtime_error("RF-DETR detection head output classes must be positive");
    }
    if (tensor.size(0) == rows) {
        return tensor.detach().clone();
    }
    const int64_t repeats = std::max<int64_t>(1, (rows + tensor.size(0) - 1) / tensor.size(0));
    std::vector<int64_t> repeat_dims(static_cast<size_t>(tensor.dim()), 1);
    repeat_dims[0] = repeats;
    return tensor.repeat(repeat_dims).narrow(0, 0, rows).contiguous();
}

void resize_linear_output(torch::nn::LinearImpl& linear, int64_t rows) {
    torch::NoGradGuard no_grad;
    linear.weight.set_data(resize_repeated_rows(linear.weight, rows).to(linear.weight.device(), linear.weight.scalar_type()));
    if (linear.bias.defined()) {
        linear.bias.set_data(resize_repeated_rows(linear.bias, rows).to(linear.bias.device(), linear.bias.scalar_type()));
    }
}

torch::Tensor gen_sineembed_for_position(const torch::Tensor& pos_tensor, int64_t dim) {
    const double scale = 2.0 * M_PI;
    auto dim_t = torch::arange(dim, torch::TensorOptions().dtype(pos_tensor.dtype()).device(pos_tensor.device()));
    dim_t = torch::pow(
        torch::full_like(dim_t, 10000.0),
        2.0 * torch::floor(dim_t / 2.0) / static_cast<double>(dim));

    auto encode = [&](const torch::Tensor& embed) {
        auto value = embed.unsqueeze(-1) / dim_t;
        return torch::stack(
                   {
                       value.slice(-1, 0, c10::nullopt, 2).sin(),
                       value.slice(-1, 1, c10::nullopt, 2).cos(),
                   },
                   -1)
            .flatten(-2);
    };

    std::vector<torch::Tensor> encoded;
    encoded.reserve(pos_tensor.size(-1));
    encoded.push_back(encode(pos_tensor.select(-1, 1) * scale));
    encoded.push_back(encode(pos_tensor.select(-1, 0) * scale));
    if (pos_tensor.size(-1) == 4) {
        encoded.push_back(encode(pos_tensor.select(-1, 2) * scale));
        encoded.push_back(encode(pos_tensor.select(-1, 3) * scale));
    } else if (pos_tensor.size(-1) != 2) {
        throw std::runtime_error("RF-DETR sine embedding expects last dim 2 or 4");
    }
    return torch::cat(encoded, -1);
}

bool is_out_feature_stage(int64_t stage) {
    return std::find(kOutFeatureStages.begin(), kOutFeatureStages.end(), stage) != kOutFeatureStages.end();
}

class LayerNorm2dImpl : public torch::nn::Module {
public:
    explicit LayerNorm2dImpl(int64_t normalized_shape, double eps = 1.0e-6)
        : weight(register_parameter("weight", torch::ones({normalized_shape}, torch::kFloat32))),
          bias(register_parameter("bias", torch::zeros({normalized_shape}, torch::kFloat32))),
          eps_(eps) {}

    torch::Tensor forward(const torch::Tensor& x) {
        auto y = x.permute({0, 2, 3, 1});
        y = F::layer_norm(
            y,
            F::LayerNormFuncOptions({y.size(3)}).weight(weight).bias(bias).eps(eps_));
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
    ConvXImpl(int64_t in_planes,
              int64_t out_planes,
              int64_t kernel = 3,
              int64_t stride = 1,
              int64_t groups = 1,
              bool layer_norm = false)
        : conv(register_module(
              "conv",
              torch::nn::Conv2d(
                  torch::nn::Conv2dOptions(in_planes, out_planes, kernel)
                      .stride(stride)
                      .padding(kernel / 2)
                      .groups(groups)
                      .bias(false)))),
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
        return out * torch::sigmoid(out);
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
        : cv1(register_module("cv1", ConvX(c1, c2, 3, 1, groups, layer_norm))),
          cv2(register_module("cv2", ConvX(c2, c2, 3, 1, groups, layer_norm))),
          add_(shortcut && c1 == c2) {}

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
        for (int64_t index = 0; index < num_blocks; ++index) {
            m->push_back(Bottleneck(c_, c_, false, 1, layer_norm));
        }
    }

    torch::Tensor forward(const torch::Tensor& x) {
        auto stem = cv1->forward(x);
        std::vector<torch::Tensor> parts;
        parts.reserve(static_cast<size_t>(2 + m->size()));
        parts.push_back(stem.narrow(1, 0, c_));
        parts.push_back(stem.narrow(1, c_, c_));
        for (size_t index = 0; index < m->size(); ++index) {
            parts.push_back(m[index]->as<Bottleneck>()->forward(parts.back()));
        }
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
    explicit Dinov2PatchEmbeddingsImpl(int64_t patch_size)
        : projection(register_module(
              "projection",
              torch::nn::Conv2d(torch::nn::Conv2dOptions(3, kDinoHiddenSize, patch_size).stride(patch_size).bias(true)))),
          patch_size_(patch_size) {}

    torch::Tensor forward(const torch::Tensor& pixel_values) {
        return projection->forward(pixel_values).flatten(2).transpose(1, 2);
    }

    torch::nn::Conv2d projection{nullptr};

private:
    int64_t patch_size_ = 0;
};
TORCH_MODULE(Dinov2PatchEmbeddings);

class WindowedDinov2EmbeddingsImpl : public torch::nn::Module {
public:
    explicit WindowedDinov2EmbeddingsImpl(const NativeRfDetrConfig& config)
        : cls_token(register_parameter("cls_token", torch::randn({1, 1, kDinoHiddenSize}, torch::kFloat32))),
          mask_token(register_parameter("mask_token", torch::zeros({1, kDinoHiddenSize}, torch::kFloat32))),
          position_embeddings(register_parameter(
              "position_embeddings",
              torch::randn(
                  {1, config.positional_encoding_size * config.positional_encoding_size + 1, kDinoHiddenSize},
                  torch::kFloat32))),
          patch_embeddings(register_module("patch_embeddings", Dinov2PatchEmbeddings(config.patch_size))),
          patch_size_(config.patch_size),
          num_windows_(std::max<int64_t>(1, config.num_windows)),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))) {}

    torch::Tensor interpolate_pos_encoding(const torch::Tensor& embeddings, int64_t height, int64_t width) const {
        const int64_t num_patches = embeddings.size(1) - 1;
        const int64_t num_positions = position_embeddings.size(1) - 1;
        if (num_patches == num_positions && height == width) {
            return position_embeddings;
        }

        auto class_pos_embed = position_embeddings.index({Slice(), 0});
        auto patch_pos_embed = position_embeddings.index({Slice(), Slice(1, None)});
        const int64_t dim = embeddings.size(-1);
        const int64_t patch_height = height / patch_size_;
        const int64_t patch_width = width / patch_size_;
        const int64_t sqrt_num_positions = static_cast<int64_t>(std::llround(std::sqrt(static_cast<double>(num_positions))));
        patch_pos_embed = patch_pos_embed.view({1, sqrt_num_positions, sqrt_num_positions, dim}).permute({0, 3, 1, 2});
        patch_pos_embed = F::interpolate(
                              patch_pos_embed.to(torch::kFloat32),
                              F::InterpolateFuncOptions()
                                  .size(std::vector<int64_t>{patch_height, patch_width})
                                  .mode(torch::kBicubic)
                                  .align_corners(false)
                                  .antialias(true))
                              .to(position_embeddings.dtype());
        patch_pos_embed = patch_pos_embed.permute({0, 2, 3, 1}).reshape({1, -1, dim});
        return torch::cat({class_pos_embed.unsqueeze(0), patch_pos_embed}, 1);
    }

    torch::Tensor forward(const torch::Tensor& pixel_values) {
        const int64_t batch_size = pixel_values.size(0);
        const int64_t height = pixel_values.size(2);
        const int64_t width = pixel_values.size(3);

        auto embeddings = patch_embeddings->forward(pixel_values.to(patch_embeddings->projection->weight.dtype()));
        auto cls_tokens = cls_token.expand({batch_size, 1, kDinoHiddenSize});
        embeddings = torch::cat({cls_tokens, embeddings}, 1);
        embeddings = embeddings + interpolate_pos_encoding(embeddings, height, width);

        if (num_windows_ > 1) {
            const int64_t num_h_patches = height / patch_size_;
            const int64_t num_w_patches = width / patch_size_;
            const int64_t num_h_patches_per_window = num_h_patches / num_windows_;
            const int64_t num_w_patches_per_window = num_w_patches / num_windows_;

            auto cls_token_with_pos_embed = embeddings.narrow(1, 0, 1);
            auto pixel_tokens_with_pos_embed = embeddings.narrow(1, 1, embeddings.size(1) - 1);
            pixel_tokens_with_pos_embed =
                pixel_tokens_with_pos_embed.view({batch_size, num_h_patches, num_w_patches, kDinoHiddenSize});
            auto windowed_pixel_tokens =
                pixel_tokens_with_pos_embed.reshape(
                    {batch_size * num_windows_,
                     num_h_patches_per_window,
                     num_windows_,
                     num_w_patches_per_window,
                     kDinoHiddenSize})
                    .permute({0, 2, 1, 3, 4})
                    .reshape(
                        {batch_size * num_windows_ * num_windows_,
                         num_h_patches_per_window * num_w_patches_per_window,
                         kDinoHiddenSize});
            auto windowed_cls_token_with_pos_embed = cls_token_with_pos_embed.repeat({num_windows_ * num_windows_, 1, 1});
            embeddings = torch::cat({windowed_cls_token_with_pos_embed, windowed_pixel_tokens}, 1);
        }
        return dropout->forward(embeddings);
    }

    torch::Tensor cls_token;
    torch::Tensor mask_token;
    torch::Tensor position_embeddings;
    Dinov2PatchEmbeddings patch_embeddings{nullptr};

private:
    int64_t patch_size_ = 0;
    int64_t num_windows_ = 1;
    torch::nn::Dropout dropout{nullptr};
};
TORCH_MODULE(WindowedDinov2Embeddings);

class Dinov2SelfAttentionImpl : public torch::nn::Module {
public:
    Dinov2SelfAttentionImpl()
        : query(register_module("query", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize))),
          key(register_module("key", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize))),
          value(register_module("value", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize))),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))) {}

    torch::Tensor forward(const torch::Tensor& hidden_states) {
        const int64_t batch = hidden_states.size(0);
        const int64_t seq_len = hidden_states.size(1);
        const int64_t head_dim = kDinoHiddenSize / kDinoNumHeads;
        auto reshape = [&](const torch::Tensor& tensor) {
            return tensor.view({batch, seq_len, kDinoNumHeads, head_dim}).permute({0, 2, 1, 3});
        };

        auto query_layer = reshape(query->forward(hidden_states));
        auto key_layer = reshape(key->forward(hidden_states));
        auto value_layer = reshape(value->forward(hidden_states));
        const double scale = 1.0 / std::sqrt(static_cast<double>(head_dim));
        const double dropout_p = is_training() ? dropout->options.p() : 0.0;
        auto context = at::scaled_dot_product_attention(
            query_layer,
            key_layer,
            value_layer,
            c10::nullopt,
            dropout_p,
            false,
            scale);
        return context.permute({0, 2, 1, 3}).contiguous().view({batch, seq_len, kDinoHiddenSize});
    }

private:
    torch::nn::Linear query{nullptr};
    torch::nn::Linear key{nullptr};
    torch::nn::Linear value{nullptr};
    torch::nn::Dropout dropout{nullptr};
};
TORCH_MODULE(Dinov2SelfAttention);

class Dinov2SelfOutputImpl : public torch::nn::Module {
public:
    Dinov2SelfOutputImpl()
        : dense(register_module("dense", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize))),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))) {}

    torch::Tensor forward(const torch::Tensor& hidden_states) {
        return dropout->forward(dense->forward(hidden_states));
    }

    torch::nn::Linear dense{nullptr};

private:
    torch::nn::Dropout dropout{nullptr};
};
TORCH_MODULE(Dinov2SelfOutput);

class Dinov2AttentionImpl : public torch::nn::Module {
public:
    Dinov2AttentionImpl()
        : attention(register_module("attention", Dinov2SelfAttention())),
          output(register_module("output", Dinov2SelfOutput())) {}

    torch::Tensor forward(const torch::Tensor& hidden_states) {
        return output->forward(attention->forward(hidden_states));
    }

private:
    Dinov2SelfAttention attention{nullptr};
    Dinov2SelfOutput output{nullptr};
};
TORCH_MODULE(Dinov2Attention);

class Dinov2LayerScaleImpl : public torch::nn::Module {
public:
    Dinov2LayerScaleImpl()
        : lambda1(register_parameter("lambda1", torch::ones({kDinoHiddenSize}, torch::kFloat32))) {}

    torch::Tensor forward(const torch::Tensor& hidden_state) const {
        return hidden_state * lambda1;
    }

    torch::Tensor lambda1;
};
TORCH_MODULE(Dinov2LayerScale);

class Dinov2MlpImpl : public torch::nn::Module {
public:
    Dinov2MlpImpl()
        : fc1(register_module("fc1", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize * 4))),
          fc2(register_module("fc2", torch::nn::Linear(kDinoHiddenSize * 4, kDinoHiddenSize))) {}

    torch::Tensor forward(const torch::Tensor& hidden_state) {
        return fc2->forward(torch::gelu(fc1->forward(hidden_state)));
    }

private:
    torch::nn::Linear fc1{nullptr};
    torch::nn::Linear fc2{nullptr};
};
TORCH_MODULE(Dinov2Mlp);

class WindowedDinov2LayerImpl : public torch::nn::Module {
public:
    explicit WindowedDinov2LayerImpl(int64_t num_windows)
        : num_windows_(std::max<int64_t>(1, num_windows)),
          norm1(register_module(
              "norm1",
              torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))),
          attention(register_module("attention", Dinov2Attention())),
          layer_scale1(register_module("layer_scale1", Dinov2LayerScale())),
          norm2(register_module(
              "norm2",
              torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))),
          mlp(register_module("mlp", Dinov2Mlp())),
          layer_scale2(register_module("layer_scale2", Dinov2LayerScale())) {}

    torch::Tensor forward(const torch::Tensor& input, bool run_full_attention) {
        auto hidden_states = input;
        const auto shortcut = hidden_states;
        const int64_t original_batch = hidden_states.size(0);
        const int64_t original_hw = hidden_states.size(1);
        if (run_full_attention && num_windows_ > 1) {
            const int64_t num_windows_squared = num_windows_ * num_windows_;
            hidden_states = hidden_states.view({original_batch / num_windows_squared, num_windows_squared * original_hw, kDinoHiddenSize});
        }

        auto attention_output = attention->forward(norm1->forward(hidden_states));
        if (run_full_attention && num_windows_ > 1) {
            attention_output = attention_output.view({original_batch, original_hw, kDinoHiddenSize});
        }

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
public:
    explicit WindowedDinov2EncoderImpl(int64_t num_windows)
        : layer(register_module("layer", torch::nn::ModuleList())) {
        for (int64_t index = 0; index < kDinoNumLayers; ++index) {
            layer->push_back(WindowedDinov2Layer(num_windows));
        }
    }

    std::vector<torch::Tensor> forward(const torch::Tensor& hidden_states_in) {
        auto hidden_states = hidden_states_in;
        std::vector<torch::Tensor> all_hidden_states;
        all_hidden_states.reserve(static_cast<size_t>(kDinoNumLayers + 1));
        for (int64_t index = 0; index < kDinoNumLayers; ++index) {
            all_hidden_states.push_back(hidden_states);
            hidden_states = layer[index]->as<WindowedDinov2Layer>()->forward(hidden_states, is_out_feature_stage(index));
        }
        all_hidden_states.push_back(hidden_states);
        return all_hidden_states;
    }

private:
    torch::nn::ModuleList layer{nullptr};
};
TORCH_MODULE(WindowedDinov2Encoder);

class WindowedDinov2BackboneImpl : public torch::nn::Module {
public:
    explicit WindowedDinov2BackboneImpl(const NativeRfDetrConfig& config)
        : patch_size_(config.patch_size),
          num_windows_(std::max<int64_t>(1, config.num_windows)),
          embeddings(register_module("embeddings", WindowedDinov2Embeddings(config))),
          encoder(register_module("encoder", WindowedDinov2Encoder(config.num_windows))),
          layernorm(register_module(
              "layernorm",
              torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))) {}

    std::vector<torch::Tensor> forward(const torch::Tensor& pixel_values) {
        auto hidden_states = encoder->forward(embeddings->forward(pixel_values));
        const int64_t batch_size = pixel_values.size(0);
        const int64_t num_h_patches = pixel_values.size(2) / patch_size_;
        const int64_t num_w_patches = pixel_values.size(3) / patch_size_;
        const int64_t num_windows_squared = num_windows_ * num_windows_;
        const int64_t num_h_patches_per_window = num_h_patches / num_windows_;
        const int64_t num_w_patches_per_window = num_w_patches / num_windows_;

        std::vector<torch::Tensor> feature_maps;
        feature_maps.reserve(kOutFeatureStages.size());
        for (const auto stage : kOutFeatureStages) {
            auto hidden_state = layernorm->forward(hidden_states[static_cast<size_t>(stage)]);
            hidden_state = hidden_state.slice(1, 1, c10::nullopt);

            if (num_windows_ > 1) {
                hidden_state = hidden_state.reshape({batch_size, num_windows_squared, hidden_state.size(1), kDinoHiddenSize});
                hidden_state = hidden_state.reshape(
                    {batch_size * num_windows_,
                     num_windows_,
                     num_h_patches_per_window,
                     num_w_patches_per_window,
                     kDinoHiddenSize});
                hidden_state = hidden_state.permute({0, 2, 1, 3, 4});
            }

            hidden_state = hidden_state.reshape({batch_size, num_h_patches, num_w_patches, kDinoHiddenSize});
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

class DinoV2WrapperImpl : public torch::nn::Module {
public:
    explicit DinoV2WrapperImpl(const NativeRfDetrConfig& config)
        : encoder(register_module("encoder", WindowedDinov2Backbone(config))) {}

    std::vector<torch::Tensor> forward(const torch::Tensor& tensors) {
        return encoder->forward(tensors);
    }

private:
    WindowedDinov2Backbone encoder{nullptr};
};
TORCH_MODULE(DinoV2Wrapper);

class NativeBackboneProjectorImpl : public torch::nn::Module {
public:
    explicit NativeBackboneProjectorImpl(int64_t hidden_dim)
        : stages(register_module("stages", torch::nn::ModuleList())) {
        auto stage = torch::nn::Sequential();
        stage->push_back(C2f(kDinoHiddenSize * static_cast<int64_t>(kOutFeatureStages.size()), hidden_dim, kProjectorBlocks, true));
        stage->push_back(LayerNorm2d(hidden_dim));
        stages->push_back(stage);
    }

    std::vector<torch::Tensor> forward(const std::vector<torch::Tensor>& inputs) {
        if (inputs.empty()) {
            throw std::runtime_error("RF-DETR projector requires at least one input");
        }
        return {stages[0]->as<torch::nn::Sequential>()->forward(torch::cat(inputs, 1))};
    }

private:
    torch::nn::ModuleList stages{nullptr};
};
TORCH_MODULE(NativeBackboneProjector);



class NativeBackboneImpl : public torch::nn::Module {
public:
    explicit NativeBackboneImpl(const NativeRfDetrConfig& config)
        : encoder(register_module("encoder", DinoV2Wrapper(config))),
          projector(register_module("projector", NativeBackboneProjector(config.hidden_dim))) {}

    std::vector<NestedTensor> forward(const NestedTensor& samples) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.backbone");
        std::vector<torch::Tensor> projected = projector->forward(encoder->forward(samples.tensors));
        std::vector<NestedTensor> out;
        out.reserve(projected.size());
        for (auto& feature : projected) {
            torch::Tensor mask;
            if (samples.mask.defined()) {
                mask = F::interpolate(
                           samples.mask.unsqueeze(1).to(torch::kFloat32),
                           F::InterpolateFuncOptions()
                               .size(std::vector<int64_t>{feature.size(2), feature.size(3)})
                               .mode(torch::kNearest))
                           .squeeze(1)
                           .to(torch::kBool);
            } else {
                mask = torch::zeros(
                    {feature.size(0), feature.size(2), feature.size(3)},
                    torch::TensorOptions().dtype(torch::kBool).device(feature.device()));
            }
            out.push_back(NestedTensor{std::move(feature), std::move(mask)});
        }
        return out;
    }

    // Tensor-only forward for JIT tracing: pixel_values → projected feature tensor.
    // Excludes mask interpolation (not needed in trace, handled by caller).
    torch::Tensor forward_features(const torch::Tensor& pixel_values) {
        auto projected = projector->forward(encoder->forward(pixel_values));
        return projected[0];
    }

private:
    DinoV2Wrapper encoder{nullptr};
    NativeBackboneProjector projector{nullptr};
};

torch::Tensor ms_deform_attn_core_pytorch(const torch::Tensor& value,
                                          const torch::Tensor& value_spatial_shapes,
                                          const torch::Tensor& sampling_locations,
                                          const torch::Tensor& attention_weights) {
    const int64_t batch = value.size(0);
    const int64_t num_heads = value.size(2);
    const int64_t head_dim = value.size(3);
    const int64_t len_q = sampling_locations.size(1);
    const int64_t num_levels = value_spatial_shapes.size(0);
    const int64_t num_points = sampling_locations.size(4);

    auto sampling_grids = 2.0 * sampling_locations - 1.0;
    std::vector<torch::Tensor> sampled_values;
    sampled_values.reserve(static_cast<size_t>(num_levels));

    int64_t start = 0;
    for (int64_t level = 0; level < num_levels; ++level) {
        const int64_t height = value_spatial_shapes.index({level, 0}).item<int64_t>();
        const int64_t width = value_spatial_shapes.index({level, 1}).item<int64_t>();
        auto value_level = value.narrow(1, start, height * width)
                               .flatten(2)
                               .transpose(1, 2)
                               .reshape({batch * num_heads, head_dim, height, width});
        auto sampling_grid_level = sampling_grids.index({Slice(), Slice(), Slice(), level}).transpose(1, 2).flatten(0, 1);
        sampled_values.push_back(F::grid_sample(
            value_level,
            sampling_grid_level,
            F::GridSampleFuncOptions().mode(torch::kBilinear).padding_mode(torch::kZeros).align_corners(false)));
        start += height * width;
    }

    auto sampled = torch::stack(sampled_values, -2).flatten(-2);
    auto normalized_attention =
        attention_weights.transpose(1, 2).reshape({batch * num_heads, 1, len_q, num_levels * num_points});
    auto output = (sampled * normalized_attention).sum(-1).view({batch, num_heads * head_dim, len_q});
    return output.transpose(1, 2).contiguous();
}

class MSDeformAttnImpl : public torch::nn::Module {
public:
    MSDeformAttnImpl(int64_t d_model, int64_t n_levels, int64_t n_heads, int64_t n_points)
        : d_model_(d_model),
          n_levels_(n_levels),
          n_heads_(n_heads),
          n_points_(n_points),
          im2col_step_(64),
          sampling_offsets(register_module(
              "sampling_offsets",
              torch::nn::Linear(d_model, n_heads * n_levels * n_points * 2))),
          attention_weights(register_module(
              "attention_weights",
              torch::nn::Linear(d_model, n_heads * n_levels * n_points))),
          value_proj(register_module("value_proj", torch::nn::Linear(d_model, d_model))),
          output_proj(register_module("output_proj", torch::nn::Linear(d_model, d_model))) {
        if (d_model_ % n_heads_ != 0) {
            throw std::runtime_error("RF-DETR MSDeformAttn requires d_model divisible by n_heads");
        }
        reset_parameters();
    }

    torch::Tensor forward(const torch::Tensor& query,
                          const torch::Tensor& reference_points,
                          const torch::Tensor& input_flatten,
                          const torch::Tensor& input_spatial_shapes,
                          const torch::Tensor& input_level_start_index,
                          const torch::Tensor& input_padding_mask) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.ms_deform_attn");
        const int64_t batch = query.size(0);
        const int64_t len_q = query.size(1);
        const int64_t len_in = input_flatten.size(1);

        auto value = value_proj->forward(input_flatten);
        if (input_padding_mask.defined()) {
            value = value.masked_fill(input_padding_mask.unsqueeze(-1), 0.0);
        }

        auto offsets =
            sampling_offsets->forward(query).view({batch, len_q, n_heads_, n_levels_, n_points_, 2});
        auto attention =
            torch::softmax(
                attention_weights->forward(query).view({batch, len_q, n_heads_, n_levels_ * n_points_}),
                -1)
                .view({batch, len_q, n_heads_, n_levels_, n_points_});

        torch::Tensor sampling_locations;
        if (reference_points.size(-1) == 2) {
            auto offset_normalizer =
                torch::stack({input_spatial_shapes.select(1, 1), input_spatial_shapes.select(1, 0)}, -1).to(query.dtype());
            sampling_locations =
                reference_points.unsqueeze(2).unsqueeze(4) +
                offsets / offset_normalizer.view({1, 1, 1, n_levels_, 1, 2});
        } else if (reference_points.size(-1) == 4) {
            sampling_locations =
                reference_points.index({Slice(), Slice(), Slice(), Slice(None, 2)}).unsqueeze(2).unsqueeze(4) +
                offsets / static_cast<double>(n_points_) *
                    reference_points.index({Slice(), Slice(), Slice(), Slice(2, None)}).unsqueeze(2).unsqueeze(4) * 0.5;
        } else {
            throw std::runtime_error("RF-DETR MSDeformAttn expects reference_points last dim 2 or 4");
        }

        value = value.view({batch, len_in, n_heads_, d_model_ / n_heads_});
        torch::Tensor attended;
        if (value.is_cuda() && !force_pytorch_deformable_attn_) {
            attended = ms_deform_attn_cuda_autograd(
                value,
                input_spatial_shapes,
                input_level_start_index,
                sampling_locations,
                attention,
                im2col_step_);
        } else {
            attended = ms_deform_attn_core_pytorch(
                value,
                input_spatial_shapes,
                sampling_locations,
                attention);
        }
        return output_proj->forward(attended);
    }

private:
    void reset_parameters() {
        torch::NoGradGuard no_grad;
        sampling_offsets->weight.zero_();
        auto thetas = torch::arange(n_heads_, torch::TensorOptions().dtype(torch::kFloat32)) *
                      (2.0 * M_PI / static_cast<double>(n_heads_));
        auto grid_init = torch::stack({thetas.cos(), thetas.sin()}, -1);
        grid_init = (grid_init / std::get<0>(grid_init.abs().max(-1, true)))
                        .view({n_heads_, 1, 1, 2})
                        .repeat({1, n_levels_, n_points_, 1});
        for (int64_t index = 0; index < n_points_; ++index) {
            grid_init.index_put_({Slice(), Slice(), index, Slice()}, grid_init.index({Slice(), Slice(), index, Slice()}) * (index + 1));
        }
        sampling_offsets->bias.copy_(grid_init.view(-1));
        attention_weights->weight.zero_();
        attention_weights->bias.zero_();
        torch::nn::init::xavier_uniform_(value_proj->weight);
        value_proj->bias.zero_();
        torch::nn::init::xavier_uniform_(output_proj->weight);
        output_proj->bias.zero_();
    }

    int64_t d_model_ = 0;
    int64_t n_levels_ = 0;
    int64_t n_heads_ = 0;
    int64_t n_points_ = 0;
    int64_t im2col_step_ = 64;
    torch::nn::Linear sampling_offsets{nullptr};
    torch::nn::Linear attention_weights{nullptr};
    torch::nn::Linear value_proj{nullptr};
    torch::nn::Linear output_proj{nullptr};

public:
    bool force_pytorch_deformable_attn_ = false;
};
TORCH_MODULE(MSDeformAttn);

class NativeDecoderLayerImpl : public torch::nn::Module {
public:
    NativeDecoderLayerImpl(int64_t d_model,
                           int64_t sa_nhead,
                           int64_t ca_nhead,
                           int64_t dim_feedforward,
                           int64_t group_detr,
                           int64_t num_feature_levels,
                           int64_t dec_n_points)
        : group_detr_(std::max<int64_t>(1, group_detr)),
          self_attn(register_module(
              "self_attn",
              torch::nn::MultiheadAttention(
                  torch::nn::MultiheadAttentionOptions(d_model, sa_nhead).dropout(0.0)))),
          dropout1(register_module("dropout1", torch::nn::Dropout(0.0))),
          norm1(register_module("norm1", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          cross_attn(register_module(
              "cross_attn",
              MSDeformAttn(d_model, num_feature_levels, ca_nhead, dec_n_points))),
          linear1(register_module("linear1", torch::nn::Linear(d_model, dim_feedforward))),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))),
          linear2(register_module("linear2", torch::nn::Linear(dim_feedforward, d_model))),
          norm2(register_module("norm2", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          norm3(register_module("norm3", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          dropout2(register_module("dropout2", torch::nn::Dropout(0.0))),
          dropout3(register_module("dropout3", torch::nn::Dropout(0.0))) {}

    torch::Tensor forward(const torch::Tensor& tgt,
                          const torch::Tensor& memory,
                          const torch::Tensor& memory_key_padding_mask,
                          const torch::Tensor& query_pos,
                          const torch::Tensor& reference_points,
                          const torch::Tensor& spatial_shapes,
                          const torch::Tensor& level_start_index) {
        const int64_t batch = tgt.size(0);
        const int64_t num_queries = tgt.size(1);
        auto q = (tgt + query_pos).transpose(0, 1);
        auto k = q;
        auto v = tgt.transpose(0, 1);
        if (is_training() && group_detr_ > 1) {
            const int64_t group_queries = num_queries / group_detr_;
            q = torch::cat(q.split(group_queries, 0), 1);
            k = torch::cat(k.split(group_queries, 0), 1);
            v = torch::cat(v.split(group_queries, 0), 1);
        }

        auto tgt2 = std::get<0>(self_attn->forward(q, k, v, {}, false));
        if (is_training() && group_detr_ > 1) {
            tgt2 = torch::cat(tgt2.split(batch, 1), 0);
        }
        tgt2 = tgt2.transpose(0, 1);

        auto output = norm1->forward(tgt + dropout1->forward(tgt2));
        tgt2 = cross_attn->forward(output + query_pos, reference_points, memory, spatial_shapes, level_start_index, memory_key_padding_mask);
        output = norm2->forward(output + dropout2->forward(tgt2));
        tgt2 = linear2->forward(dropout->forward(torch::relu(linear1->forward(output))));
        return norm3->forward(output + dropout3->forward(tgt2));
    }

private:
    int64_t group_detr_ = 1;
    torch::nn::MultiheadAttention self_attn{nullptr};
    torch::nn::Dropout dropout1{nullptr};
    torch::nn::LayerNorm norm1{nullptr};
    MSDeformAttn cross_attn{nullptr};
    torch::nn::Linear linear1{nullptr};
    torch::nn::Dropout dropout{nullptr};
    torch::nn::Linear linear2{nullptr};
    torch::nn::LayerNorm norm2{nullptr};
    torch::nn::LayerNorm norm3{nullptr};
    torch::nn::Dropout dropout2{nullptr};
    torch::nn::Dropout dropout3{nullptr};
};
TORCH_MODULE(NativeDecoderLayer);

class NativeDecoderImpl : public torch::nn::Module {
public:
    explicit NativeDecoderImpl(const NativeRfDetrConfig& config)
        : num_layers_(std::max<int64_t>(1, config.dec_layers)),
          d_model_(config.hidden_dim),
          lite_refpoint_refine_(config.lite_refpoint_refine),
          bbox_reparam_(config.bbox_reparam),
          layers(register_module("layers", torch::nn::ModuleList())),
          norm(register_module("norm", torch::nn::LayerNorm(std::vector<int64_t>{config.hidden_dim}))),
          ref_point_head(register_module(
              "ref_point_head",
              std::make_shared<RfDetrMlpImpl>(2 * config.hidden_dim, config.hidden_dim, config.hidden_dim, 2))) {
        for (int64_t index = 0; index < num_layers_; ++index) {
            layers->push_back(NativeDecoderLayer(
                config.hidden_dim,
                config.sa_nheads,
                config.ca_nheads,
                config.dim_feedforward,
                config.group_detr,
                1,
                config.dec_n_points));
        }
    }

    void set_bbox_embed(const std::shared_ptr<RfDetrMlpImpl>& bbox_embed) {
        bbox_embed_ = bbox_embed;
    }

    std::pair<torch::Tensor, torch::Tensor> forward(const torch::Tensor& tgt,
                                                    const torch::Tensor& memory,
                                                    const torch::Tensor& memory_key_padding_mask,
                                                    const torch::Tensor& pos,
                                                    const torch::Tensor& refpoints_unsigmoid,
                                                    const torch::Tensor& level_start_index,
                                                    const torch::Tensor& spatial_shapes,
                                                    const torch::Tensor& valid_ratios) {
        auto output = tgt;
        auto refpoints = refpoints_unsigmoid;

        std::vector<torch::Tensor> intermediate;
        intermediate.reserve(static_cast<size_t>(num_layers_));
        std::vector<torch::Tensor> hs_refpoints_unsigmoid;
        hs_refpoints_unsigmoid.push_back(refpoints_unsigmoid);

        auto refine = [&](const torch::Tensor& base, const torch::Tensor& delta) {
            if (bbox_reparam_) {
                auto cxcy =
                    delta.index({Slice(), Slice(), Slice(None, 2)}) * base.index({Slice(), Slice(), Slice(2, None)}) +
                    base.index({Slice(), Slice(), Slice(None, 2)});
                auto wh =
                    delta.index({Slice(), Slice(), Slice(2, None)}).exp() * base.index({Slice(), Slice(), Slice(2, None)});
                return torch::cat({cxcy, wh}, -1);
            }
            return base + delta;
        };

        auto get_reference = [&](const torch::Tensor& ref_input) {
            auto obj_center = ref_input.index({Slice(), Slice(), Slice(None, 4)});
            auto refpoints_input =
                obj_center.unsqueeze(2) * torch::cat({valid_ratios, valid_ratios}, -1).to(obj_center.dtype()).unsqueeze(1);
            auto query_sine_embed = gen_sineembed_for_position(refpoints_input.index({Slice(), Slice(), 0}), d_model_ / 2);
            auto query_pos = ref_point_head->forward(query_sine_embed);
            return std::make_tuple(refpoints_input, query_pos);
        };

        torch::Tensor refpoints_input;
        torch::Tensor query_pos;
        if (lite_refpoint_refine_) {
            std::tie(refpoints_input, query_pos) = get_reference(bbox_reparam_ ? refpoints : refpoints.sigmoid());
        }

        for (int64_t layer_id = 0; layer_id < num_layers_; ++layer_id) {
            if (!lite_refpoint_refine_) {
                std::tie(refpoints_input, query_pos) = get_reference(bbox_reparam_ ? refpoints : refpoints.sigmoid());
            }

            output = layers[layer_id]
                         ->as<NativeDecoderLayer>()
                         ->forward(output, memory, memory_key_padding_mask, query_pos, refpoints_input, spatial_shapes, level_start_index);

            if (!lite_refpoint_refine_ && bbox_embed_) {
                auto new_refpoints = refine(refpoints, bbox_embed_->forward(output));
                if (layer_id != num_layers_ - 1) {
                    hs_refpoints_unsigmoid.push_back(new_refpoints);
                }
                refpoints = new_refpoints.detach();
            }

            intermediate.push_back(norm->forward(output));
            (void)pos;
        }

        if (bbox_embed_) {
            return {torch::stack(intermediate), torch::stack(hs_refpoints_unsigmoid)};
        }
        return {torch::stack(intermediate), refpoints.unsqueeze(0)};
    }

private:
    int64_t num_layers_ = 0;
    int64_t d_model_ = 0;
    bool lite_refpoint_refine_ = true;
    bool bbox_reparam_ = true;
    torch::nn::ModuleList layers{nullptr};
    torch::nn::LayerNorm norm{nullptr};
    std::shared_ptr<RfDetrMlpImpl> ref_point_head;
    std::shared_ptr<RfDetrMlpImpl> bbox_embed_;
};

std::pair<torch::Tensor, torch::Tensor> gen_encoder_output_proposals(const torch::Tensor& memory,
                                                                     const torch::Tensor& memory_padding_mask,
                                                                     const torch::Tensor& spatial_shapes,
                                                                     bool unsigmoid) {
    const int64_t batch = memory.size(0);
    std::vector<torch::Tensor> proposals;
    proposals.reserve(static_cast<size_t>(spatial_shapes.size(0)));

    int64_t start = 0;
    for (int64_t level = 0; level < spatial_shapes.size(0); ++level) {
        const int64_t height = spatial_shapes.index({level, 0}).item<int64_t>();
        const int64_t width = spatial_shapes.index({level, 1}).item<int64_t>();

        torch::Tensor valid_h;
        torch::Tensor valid_w;
        if (memory_padding_mask.defined()) {
            auto mask_flatten = memory_padding_mask.narrow(1, start, height * width).view({batch, height, width, 1});
            valid_h = torch::sum(torch::logical_not(mask_flatten.index({Slice(), Slice(), 0, 0})), 1);
            valid_w = torch::sum(torch::logical_not(mask_flatten.index({Slice(), 0, Slice(), 0})), 1);
        } else {
            valid_h = torch::full({batch}, height, torch::TensorOptions().dtype(torch::kInt64).device(memory.device()));
            valid_w = torch::full({batch}, width, torch::TensorOptions().dtype(torch::kInt64).device(memory.device()));
        }

        auto grid_y = torch::arange(height, torch::TensorOptions().dtype(torch::kFloat32).device(memory.device()))
                          .view({height, 1})
                          .repeat({1, width});
        auto grid_x = torch::arange(width, torch::TensorOptions().dtype(torch::kFloat32).device(memory.device()))
                          .view({1, width})
                          .repeat({height, 1});
        auto grid = torch::cat({grid_x.unsqueeze(-1), grid_y.unsqueeze(-1)}, -1);
        auto scale = torch::cat({valid_w.unsqueeze(-1), valid_h.unsqueeze(-1)}, 1).view({batch, 1, 1, 2}).to(grid.dtype());
        grid = (grid.unsqueeze(0).expand({batch, -1, -1, -1}) + 0.5) / scale;
        auto wh = torch::ones_like(grid) * (0.05 * std::pow(2.0, static_cast<double>(level)));
        proposals.push_back(torch::cat({grid, wh}, -1).view({batch, -1, 4}));
        start += height * width;
    }

    auto output_proposals = torch::cat(proposals, 1);
    auto output_proposals_valid = ((output_proposals > 0.01) & (output_proposals < 0.99)).all(-1, true);

    if (unsigmoid) {
        output_proposals = torch::log(output_proposals / (1.0 - output_proposals));
        if (memory_padding_mask.defined()) {
            output_proposals = output_proposals.masked_fill(
                memory_padding_mask.unsqueeze(-1),
                std::numeric_limits<float>::infinity());
        }
        output_proposals = output_proposals.masked_fill(
            ~output_proposals_valid,
            std::numeric_limits<float>::infinity());
    } else {
        if (memory_padding_mask.defined()) {
            output_proposals = output_proposals.masked_fill(memory_padding_mask.unsqueeze(-1), 0.0);
        }
        output_proposals = output_proposals.masked_fill(~output_proposals_valid, 0.0);
    }

    auto output_memory = memory;
    if (memory_padding_mask.defined()) {
        output_memory = output_memory.masked_fill(memory_padding_mask.unsqueeze(-1), 0.0);
    }
    output_memory = output_memory.masked_fill(~output_proposals_valid, 0.0);
    return {output_memory.to(memory.dtype()), output_proposals.to(memory.dtype())};
}

class NativeTransformerImpl : public torch::nn::Module {
public:
    explicit NativeTransformerImpl(const NativeRfDetrConfig& config)
        : decoder(register_module("decoder", std::make_shared<NativeDecoderImpl>(config))),
          enc_output(register_module("enc_output", torch::nn::ModuleList())),
          enc_output_norm(register_module("enc_output_norm", torch::nn::ModuleList())),
          enc_out_class_embed(register_module("enc_out_class_embed", torch::nn::ModuleList())),
          enc_out_bbox_embed(register_module("enc_out_bbox_embed", torch::nn::ModuleList())),
          config_(config) {
        const int64_t groups = std::max<int64_t>(1, config.group_detr);
        for (int64_t index = 0; index < groups; ++index) {
            enc_output->push_back(torch::nn::Linear(config.hidden_dim, config.hidden_dim));
            enc_output_norm->push_back(torch::nn::LayerNorm(std::vector<int64_t>{config.hidden_dim}));
            enc_out_class_embed->push_back(torch::nn::Linear(config.hidden_dim, std::max(1, config.num_classes)));
            enc_out_bbox_embed->push_back(std::make_shared<RfDetrMlpImpl>(config.hidden_dim, config.hidden_dim, 4, 3));
        }

        torch::NoGradGuard no_grad;
        const double prior_prob = 0.01;
        const float bias_value = static_cast<float>(-std::log((1.0 - prior_prob) / prior_prob));
        for (size_t index = 0; index < enc_out_class_embed->size(); ++index) {
            auto& linear = enc_out_class_embed->at<torch::nn::LinearImpl>(index);
            linear.bias.fill_(bias_value);
        }
    }

    void set_decoder_bbox_embed(const std::shared_ptr<RfDetrMlpImpl>& bbox_embed) {
        decoder->set_bbox_embed(bbox_embed);
    }

    void resize_output_classes(int64_t output_classes) {
        for (size_t index = 0; index < enc_out_class_embed->size(); ++index) {
            auto& linear = enc_out_class_embed->at<torch::nn::LinearImpl>(index);
            resize_linear_output(linear, output_classes);
        }
    }

    std::vector<LinearOutputState> output_class_state() const {
        std::vector<LinearOutputState> state;
        state.reserve(static_cast<size_t>(enc_out_class_embed->size()));
        for (size_t index = 0; index < enc_out_class_embed->size(); ++index) {
            state.push_back(capture_linear_output_state(
                enc_out_class_embed->at<torch::nn::LinearImpl>(index)));
        }
        return state;
    }

    void restore_output_class_state(const std::vector<LinearOutputState>& state) {
        if (state.size() != static_cast<size_t>(enc_out_class_embed->size())) {
            throw std::runtime_error("RF-DETR encoder class-head count changed during restore");
        }
        for (size_t index = 0; index < state.size(); ++index) {
            auto& linear = enc_out_class_embed->at<torch::nn::LinearImpl>(index);
            restore_linear_output_state(linear, state[index]);
        }
    }

    torch::Tensor encoder_class_logits(const torch::Tensor& hs_enc, bool training_mode) {
        if (!hs_enc.defined()) {
            return {};
        }
        const int64_t groups = training_mode ? std::max<int64_t>(1, config_.group_detr) : 1;
        auto hs_chunks = hs_enc.chunk(groups, 1);
        std::vector<torch::Tensor> logits;
        logits.reserve(static_cast<size_t>(groups));
        for (int64_t index = 0; index < groups; ++index) {
            logits.push_back(
                enc_out_class_embed->at<torch::nn::LinearImpl>(static_cast<size_t>(index))
                    .forward(hs_chunks[static_cast<size_t>(index)]));
        }
        return torch::cat(logits, 1);
    }

    NativeTransformerOutput forward(const std::vector<torch::Tensor>& srcs,
                                    const std::vector<torch::Tensor>& masks,
                                    const std::vector<torch::Tensor>& pos_embeds,
                                    const torch::Tensor& refpoint_embed,
                                    const torch::Tensor& query_feat,
                                    bool training_mode) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.transformer");
        if (srcs.empty()) {
            throw std::runtime_error("RF-DETR transformer requires at least one source feature");
        }

        const int64_t batch = srcs.front().size(0);
        std::vector<torch::Tensor> src_flatten;
        std::vector<torch::Tensor> mask_flatten;
        std::vector<torch::Tensor> pos_flatten;
        std::vector<torch::Tensor> valid_ratios;
        std::vector<int64_t> spatial_shape_values;

        src_flatten.reserve(srcs.size());
        mask_flatten.reserve(masks.size());
        pos_flatten.reserve(pos_embeds.size());
        valid_ratios.reserve(masks.size());
        spatial_shape_values.reserve(srcs.size() * 2);

        {
            FASTLOADER_PROFILE_SCOPE("rfdetr.model.transformer.flatten_inputs");
            for (size_t level = 0; level < srcs.size(); ++level) {
                const auto& src = srcs[level];
                const auto& pos = pos_embeds[level];
                spatial_shape_values.push_back(src.size(2));
                spatial_shape_values.push_back(src.size(3));
                src_flatten.push_back(src.flatten(2).transpose(1, 2));
                pos_flatten.push_back(pos.flatten(2).transpose(1, 2));
                if (!masks.empty()) {
                    mask_flatten.push_back(masks[level].flatten(1));
                    auto valid_h = torch::sum(torch::logical_not(masks[level].index({Slice(), Slice(), 0})), 1).to(torch::kFloat32) /
                                   static_cast<double>(masks[level].size(1));
                    auto valid_w = torch::sum(torch::logical_not(masks[level].index({Slice(), 0, Slice()})), 1).to(torch::kFloat32) /
                                   static_cast<double>(masks[level].size(2));
                    valid_ratios.push_back(torch::stack({valid_w, valid_h}, -1));
                }
            }
        }

        auto memory = torch::cat(src_flatten, 1);
        auto lvl_pos_embed_flatten = torch::cat(pos_flatten, 1);
        torch::Tensor mask_flatten_tensor;
        torch::Tensor valid_ratios_tensor;
        if (!mask_flatten.empty()) {
            mask_flatten_tensor = torch::cat(mask_flatten, 1);
            valid_ratios_tensor = torch::stack(valid_ratios, 1);
        }

        auto spatial_shapes = torch::tensor(
            spatial_shape_values,
            torch::TensorOptions().dtype(torch::kLong).device(memory.device()))
                                  .view({static_cast<int64_t>(srcs.size()), 2});
        auto level_start_index =
            torch::cat({spatial_shapes.new_zeros({1}), spatial_shapes.prod(1).cumsum(0).slice(0, 0, -1)});

        torch::Tensor hidden_states;
        torch::Tensor refs_unsigmoid;
        torch::Tensor enc_memory;
        torch::Tensor enc_boxes;

        if (config_.two_stage) {
            FASTLOADER_PROFILE_SCOPE("rfdetr.model.transformer.two_stage");
            auto [output_memory, output_proposals] =
                gen_encoder_output_proposals(memory, mask_flatten_tensor, spatial_shapes, !config_.bbox_reparam);

            std::vector<torch::Tensor> refpoint_embed_ts;
            std::vector<torch::Tensor> memory_ts;
            std::vector<torch::Tensor> boxes_ts;
            const int64_t groups = training_mode ? std::max<int64_t>(1, config_.group_detr) : 1;
            refpoint_embed_ts.reserve(static_cast<size_t>(groups));
            memory_ts.reserve(static_cast<size_t>(groups));
            boxes_ts.reserve(static_cast<size_t>(groups));

            for (int64_t group_index = 0; group_index < groups; ++group_index) {
                auto output_memory_group =
                    enc_output_norm->at<torch::nn::LayerNormImpl>(static_cast<size_t>(group_index))
                        .forward(enc_output->at<torch::nn::LinearImpl>(static_cast<size_t>(group_index)).forward(output_memory));
                auto enc_outputs_class =
                    enc_out_class_embed->at<torch::nn::LinearImpl>(static_cast<size_t>(group_index)).forward(output_memory_group);

                torch::Tensor enc_outputs_coord;
                if (config_.bbox_reparam) {
                    auto coord_delta =
                        enc_out_bbox_embed->at<RfDetrMlpImpl>(static_cast<size_t>(group_index)).forward(output_memory_group);
                    auto cxcy =
                        coord_delta.index({Slice(), Slice(), Slice(None, 2)}) *
                            output_proposals.index({Slice(), Slice(), Slice(2, None)}) +
                        output_proposals.index({Slice(), Slice(), Slice(None, 2)});
                    auto wh =
                        coord_delta.index({Slice(), Slice(), Slice(2, None)}).exp() *
                        output_proposals.index({Slice(), Slice(), Slice(2, None)});
                    enc_outputs_coord = torch::cat({cxcy, wh}, -1);
                } else {
                    enc_outputs_coord =
                        enc_out_bbox_embed->at<RfDetrMlpImpl>(static_cast<size_t>(group_index)).forward(output_memory_group) +
                        output_proposals;
                }

                auto proposal_scores = std::get<0>(enc_outputs_class.max(-1));
                const int64_t topk = std::min<int64_t>(config_.num_queries, enc_outputs_class.size(1));
                auto topk_indices = std::get<1>(proposal_scores.topk(topk, 1));
                auto gathered_boxes =
                    enc_outputs_coord.gather(1, topk_indices.unsqueeze(-1).expand({batch, topk, 4}));
                refpoint_embed_ts.push_back(gathered_boxes.detach());
                boxes_ts.push_back(gathered_boxes);
                memory_ts.push_back(
                    output_memory_group.gather(
                        1,
                        topk_indices.unsqueeze(-1).expand({batch, topk, config_.hidden_dim})));
            }

            enc_memory = torch::cat(memory_ts, 1);
            enc_boxes = config_.bbox_reparam ? torch::cat(boxes_ts, 1) : torch::cat(boxes_ts, 1).sigmoid();

            if (config_.dec_layers > 0) {
                FASTLOADER_PROFILE_SCOPE("rfdetr.model.transformer.decoder");
                auto tgt = query_feat.unsqueeze(0).repeat({batch, 1, 1});
                auto refpoints = refpoint_embed.unsqueeze(0).repeat({batch, 1, 1});
                auto topk_refs = torch::cat(refpoint_embed_ts, 1);
                const int64_t ts_len = topk_refs.size(1);
                auto refpoints_ts_subset = refpoints.narrow(1, 0, ts_len);
                auto refpoints_subset = refpoints.narrow(1, ts_len, refpoints.size(1) - ts_len);
                if (config_.bbox_reparam) {
                    auto cxcy =
                        refpoints_ts_subset.index({Slice(), Slice(), Slice(None, 2)}) *
                            topk_refs.index({Slice(), Slice(), Slice(2, None)}) +
                        topk_refs.index({Slice(), Slice(), Slice(None, 2)});
                    auto wh =
                        refpoints_ts_subset.index({Slice(), Slice(), Slice(2, None)}).exp() *
                        topk_refs.index({Slice(), Slice(), Slice(2, None)});
                    refpoints_ts_subset = torch::cat({cxcy, wh}, -1);
                } else {
                    refpoints_ts_subset = refpoints_ts_subset + topk_refs;
                }
                refpoints = torch::cat({refpoints_ts_subset, refpoints_subset}, 1);
                std::tie(hidden_states, refs_unsigmoid) =
                    decoder->forward(
                        tgt,
                        memory,
                        mask_flatten_tensor,
                        lvl_pos_embed_flatten,
                        refpoints,
                        level_start_index,
                        spatial_shapes,
                        valid_ratios_tensor.to(memory.dtype()));
            }
        } else if (config_.dec_layers > 0) {
            FASTLOADER_PROFILE_SCOPE("rfdetr.model.transformer.decoder");
            auto tgt = query_feat.unsqueeze(0).repeat({batch, 1, 1});
            auto refpoints = refpoint_embed.unsqueeze(0).repeat({batch, 1, 1});
            std::tie(hidden_states, refs_unsigmoid) =
                decoder->forward(
                    tgt,
                    memory,
                    mask_flatten_tensor,
                    lvl_pos_embed_flatten,
                    refpoints,
                    level_start_index,
                    spatial_shapes,
                    valid_ratios_tensor.to(memory.dtype()));
        }

        return NativeTransformerOutput{
            std::move(hidden_states),
            std::move(refs_unsigmoid),
            std::move(enc_memory),
            std::move(enc_boxes),
        };
    }

private:
    std::shared_ptr<NativeDecoderImpl> decoder;
    torch::nn::ModuleList enc_output{nullptr};
    torch::nn::ModuleList enc_output_norm{nullptr};
    torch::nn::ModuleList enc_out_class_embed{nullptr};
    torch::nn::ModuleList enc_out_bbox_embed{nullptr};
    NativeRfDetrConfig config_;
};

const ModelPresetConfig* infer_preset_from_path(const std::filesystem::path& input_path) {
    const std::string normalized = input_path.lexically_normal().string();
    const ModelPresetConfig* best_match = nullptr;
    size_t best_score = 0;
    for (const auto& preset : model_presets()) {
        size_t score = 0;
        const std::string canonical_weight_filename(preset.canonical_weight_filename);
        if (normalized.find(canonical_weight_filename) != std::string::npos) {
            score = 1000 + canonical_weight_filename.size();
        }
        const std::string preset_name(preset.preset_name);
        if (normalized.find(preset_name) != std::string::npos) {
            score = std::max(score, preset_name.size());
        }
        if (score > best_score) {
            best_match = &preset;
            best_score = score;
        }
    }
    return best_match;
}

NativeRfDetrConfig make_config_from_preset(const ModelPresetConfig& preset) {
    return NativeRfDetrConfig{
        std::string(preset.preset_name),
        preset.resolution,
        preset.segmentation_head,
        preset.num_classes,
        preset.num_queries,
        preset.num_select,
        preset.dec_layers,
        preset.group_detr,
        preset.two_stage,
        preset.hidden_dim,
        preset.patch_size,
        preset.num_windows,
        preset.positional_encoding_size,
        8,
        16,
        2,
        2048,
        true,
        true,
        preset.cls_loss_coef,
        preset.bbox_loss_coef,
        preset.giou_loss_coef,
        preset.mask_ce_loss_coef,
        preset.mask_dice_loss_coef,
    };
}

NativeRfDetrConfig make_config_from_path(const std::filesystem::path& input_path) {
    if (const auto* preset = infer_preset_from_path(input_path)) {
        return make_config_from_preset(*preset);
    }
    NativeRfDetrConfig config;
    config.preset_name = input_path.stem().string();
    return config;
}

std::filesystem::path canonical_existing_path(const std::filesystem::path& path, std::string_view label) {
    if (path.empty()) {
        throw std::runtime_error(std::string(label) + " path must not be empty");
    }
    auto canonical_path = std::filesystem::absolute(path).lexically_normal();
    if (!std::filesystem::exists(canonical_path)) {
        throw std::runtime_error("missing " + std::string(label) + " file: " + canonical_path.string());
    }
    return canonical_path;
}

NativeCheckpoint prepare_checkpoint_for_module(const NativeCheckpoint& checkpoint, const torch::nn::Module& module) {
    auto parameters = module.named_parameters(true);
    auto buffers = module.named_buffers(true);

    NativeCheckpoint prepared = checkpoint;
    std::vector<StateDictEntry> expansions;

    for (auto& entry : prepared.state_dict) {
        // Remap upstream Python top-level encoder heads to C++ transformer namespace
        if (entry.name.find("enc_output.") == 0 ||
            entry.name.find("enc_output_norm.") == 0 ||
            entry.name.find("enc_out_class_embed.") == 0 ||
            entry.name.find("enc_out_bbox_embed.") == 0) {
            
            const std::string original_name = entry.name;
            entry.name = "transformer." + original_name;

            // If we are loading a group-0 weight from an upstream checkpoint into a multi-group
            // model, broadcast the group-0 weights to all groups to avoid random initialization.
            if (original_name.find(".0.") != std::string::npos) {
                for (int64_t group = 1; group < 32; ++group) {
                    std::string group_suffix = "." + std::to_string(group) + ".";
                    std::string expanded_name = original_name;
                    size_t pos = expanded_name.find(".0.");
                    expanded_name.replace(pos, 3, group_suffix);
                    
                    const std::string target_name = "transformer." + expanded_name;
                    if (parameters.find(target_name) != nullptr || buffers.find(target_name) != nullptr) {
                        expansions.push_back({target_name, entry.tensor.clone()});
                    }
                }
            }
        }

        torch::Tensor* target = parameters.find(entry.name);
        if (target == nullptr) {
            target = buffers.find(entry.name);
        }
        if (target == nullptr) {
            continue;
        }
        if ((entry.name == "refpoint_embed.weight" || entry.name == "query_feat.weight") &&
            entry.tensor.dim() == target->dim() &&
            entry.tensor.size(0) >= target->size(0)) {
            entry.tensor = entry.tensor.narrow(0, 0, target->size(0)).contiguous();
        }
    }

    for (auto& expansion : expansions) {
        prepared.state_dict.push_back(std::move(expansion));
    }

    return prepared;
}

std::optional<int64_t> checkpoint_output_class_count(const NativeCheckpoint& checkpoint) {
    for (const auto& entry : checkpoint.state_dict) {
        if ((entry.name == "class_embed.bias" || entry.name == "class_embed.weight") &&
            entry.tensor.defined() &&
            entry.tensor.dim() >= 1) {
            return entry.tensor.size(0);
        }
    }
    return std::nullopt;
}

void apply_checkpoint_detection_config(NativeRfDetrConfig& config, const NativeCheckpointMetadata& metadata) {
    if (metadata.sum_group_losses.has_value()) {
        config.sum_group_losses = *metadata.sum_group_losses;
    }
    if (metadata.use_varifocal_loss.has_value()) {
        config.use_varifocal_loss = *metadata.use_varifocal_loss;
    }
    if (metadata.use_position_supervised_loss.has_value()) {
        config.use_position_supervised_loss = *metadata.use_position_supervised_loss;
    }
    if (metadata.ia_bce_loss.has_value()) {
        config.ia_bce_loss = *metadata.ia_bce_loss;
    }
    if (metadata.aux_loss.has_value()) {
        config.aux_loss = *metadata.aux_loss;
    }
    if (metadata.mask_point_sample_ratio.has_value()) {
        config.mask_point_sample_ratio = *metadata.mask_point_sample_ratio;
    }
    if (metadata.focal_alpha.has_value()) {
        config.focal_alpha = *metadata.focal_alpha;
    }
    if (metadata.cls_loss_coef.has_value()) {
        config.cls_loss_coef = *metadata.cls_loss_coef;
    }
    if (metadata.bbox_loss_coef.has_value()) {
        config.bbox_loss_coef = *metadata.bbox_loss_coef;
    }
    if (metadata.giou_loss_coef.has_value()) {
        config.giou_loss_coef = *metadata.giou_loss_coef;
    }
    if (metadata.mask_ce_loss_coef.has_value()) {
        config.mask_ce_loss_coef = *metadata.mask_ce_loss_coef;
    }
    if (metadata.mask_dice_loss_coef.has_value()) {
        config.mask_dice_loss_coef = *metadata.mask_dice_loss_coef;
    }
    if (metadata.set_cost_class.has_value()) {
        config.set_cost_class = *metadata.set_cost_class;
    }
    if (metadata.set_cost_bbox.has_value()) {
        config.set_cost_bbox = *metadata.set_cost_bbox;
    }
    if (metadata.set_cost_giou.has_value()) {
        config.set_cost_giou = *metadata.set_cost_giou;
    }
}

} // namespace

ResolvedModelArtifacts resolve_model_artifacts(const ModelArtifactRequest& request) {
    const size_t selected_input_count =
        static_cast<size_t>(!request.weights_path.empty()) +
        static_cast<size_t>(!request.onnx_path.empty()) +
        static_cast<size_t>(!request.tensorrt_path.empty());
    if (selected_input_count != 1) {
        throw std::runtime_error("RF-DETR model input requires exactly one of --weights, --onnx, or --tensorrt");
    }

    if (!request.weights_path.empty()) {
        return resolve_upstream_weight_artifacts(request.weights_path);
    }

    auto resolve_non_weight_config = [&](const std::filesystem::path& input_path) {
        if (!request.preset_name.empty()) {
            if (const auto* preset = find_model_preset(request.preset_name)) {
                return make_config_from_preset(*preset);
            }
            throw std::runtime_error("unknown RF-DETR preset override: " + request.preset_name);
        }

        NativeRfDetrConfig config = make_config_from_path(input_path);
        if (config.resolution <= 0 || config.num_queries <= 0) {
            throw std::runtime_error(
                "unable to infer RF-DETR preset from backend artifact path; provide a preset name or use a "
                "canonical preset filename: " + input_path.string());
        }
        return config;
    };

    ResolvedModelArtifacts artifacts;
    if (!request.onnx_path.empty()) {
        artifacts.input_kind = "onnx";
        artifacts.input_path = canonical_existing_path(request.onnx_path, "RF-DETR ONNX");
        artifacts.config = resolve_non_weight_config(artifacts.input_path);
        artifacts.artifact_root = artifacts.input_path.parent_path();
        artifacts.onnx_path = artifacts.input_path;
        return artifacts;
    }

    artifacts.input_kind = "tensorrt";
    artifacts.input_path = canonical_existing_path(request.tensorrt_path, "RF-DETR TensorRT engine");
    artifacts.config = resolve_non_weight_config(artifacts.input_path);
    artifacts.artifact_root = artifacts.input_path.parent_path();
    artifacts.tensorrt_path = artifacts.input_path;
    return artifacts;
}

ResolvedModelArtifacts resolve_upstream_weight_artifacts(const std::filesystem::path& weights_path) {
    const auto canonical_weights_path = canonical_existing_path(weights_path, "RF-DETR weights");

    ResolvedModelArtifacts artifacts;
    artifacts.input_path = canonical_weights_path;
    artifacts.weights_path = canonical_weights_path;
    artifacts.artifact_root = canonical_weights_path.parent_path();

    const auto checkpoint = load_checkpoint(canonical_weights_path);
    if (!checkpoint.metadata.preset_name.empty()) {
        if (const auto* preset = find_model_preset(checkpoint.metadata.preset_name)) {
            artifacts.config = make_config_from_preset(*preset);
        } else {
            artifacts.config = make_config_from_path(canonical_weights_path);
            artifacts.config.preset_name = checkpoint.metadata.preset_name;
        }
    } else if (const auto* preset =
                   find_model_preset_by_weight_filename(canonical_weights_path.filename().string())) {
        artifacts.config = make_config_from_preset(*preset);
    } else if (const auto* inferred_preset = infer_preset_from_path(canonical_weights_path)) {
        artifacts.config = make_config_from_preset(*inferred_preset);
    } else {
        throw std::runtime_error(
            "unable to infer RF-DETR preset from weights file or native checkpoint metadata: " +
            canonical_weights_path.string());
    }

    if (checkpoint.metadata.num_classes > 0) {
        artifacts.config.num_classes = static_cast<int>(checkpoint.metadata.num_classes);
    }
    apply_checkpoint_detection_config(artifacts.config, checkpoint.metadata);

    artifacts.input_kind = is_native_checkpoint_file(canonical_weights_path) ? "native-pt" : "upstream-python";
    return artifacts;
}

NativeRfDetrModel::NativeRfDetrModel(const NativeRfDetrConfig& config)
    : config_(config.num_queries > 0 ? config : make_config_from_preset(model_presets().front())),
      backbone_(register_module("backbone", torch::nn::ModuleList())),
      class_embed(register_module(
          "class_embed",
          torch::nn::Linear(config_.hidden_dim, std::max(1, config_.num_classes)))),
      refpoint_embed(register_module(
          "refpoint_embed",
          torch::nn::Embedding(std::max(1, config_.num_queries * std::max(1, config_.group_detr)), 4))),
      query_feat(register_module(
          "query_feat",
          torch::nn::Embedding(
              std::max(1, config_.num_queries * std::max(1, config_.group_detr)),
              config_.hidden_dim))) {
    at::globalContext().setSDPUseMemEfficient(false);
    at::globalContext().setSDPUseFlash(false);
    
    backbone_->push_back(std::make_shared<NativeBackboneImpl>(config_));
    backbone_->push_back(PositionEmbeddingSine(config_.hidden_dim / 2, 10000.0, true, 2.0 * M_PI));

    transformer_ = register_module("transformer", std::make_shared<NativeTransformerImpl>(config_));
    bbox_embed_ =
        register_module("bbox_embed", std::make_shared<RfDetrMlpImpl>(config_.hidden_dim, config_.hidden_dim, 4, 3));
    if (config_.segmentation) {
        segmentation_head_ = register_module(
            "segmentation_head",
            std::make_shared<SegmentationHeadImpl>(
                config_.hidden_dim,
                std::max<int64_t>(1, config_.dec_layers)));
    }

    if (auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_)) {
        if (!config_.lite_refpoint_refine) {
            transformer->set_decoder_bbox_embed(std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_));
        }
    }

    torch::NoGradGuard no_grad;
    refpoint_embed->weight.zero_();

    const double prior_prob = 0.01;
    const float bias_value = static_cast<float>(-std::log((1.0 - prior_prob) / prior_prob));
    class_embed->bias.fill_(bias_value);

    if (auto bbox = std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_)) {
        auto params = bbox->named_parameters(true);
        if (auto* weight = params.find("layers.2.weight")) {
            weight->zero_();
        }
        if (auto* bias = params.find("layers.2.bias")) {
            bias->zero_();
        }
    }
}

void NativeRfDetrModel::reinitialize_detection_head(int64_t output_classes) {
    if (output_classes <= 0) {
        throw std::runtime_error("RF-DETR detection head output classes must be positive");
    }

    resize_linear_output(*class_embed, output_classes);

    auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_);
    if (!transformer) {
        throw std::runtime_error("RF-DETR transformer is not initialized");
    }
    transformer->resize_output_classes(output_classes);
}

ModelOutputs NativeRfDetrModel::forward(const NestedTensor& batch,
                                        SegmentationHeadImpl* seg_override,
                                        bool include_masks) {
    const bool is_train = is_training();
    if ((is_train && is_compiled_train_) || (!is_train && is_compiled_eval_)) {
        std::vector<torch::jit::IValue> inputs;
        inputs.push_back(batch.tensors);
        if (batch.mask.defined()) {
            inputs.push_back(batch.mask);
        } else {
            inputs.push_back(torch::zeros(
                {batch.tensors.size(0), batch.tensors.size(2), batch.tensors.size(3)},
                torch::TensorOptions().dtype(torch::kBool).device(batch.tensors.device())));
        }

        auto& target_model = is_train ? traced_model_train_ : traced_model_eval_;
        auto out_dict = target_model.forward(inputs).toGenericDict();
        ModelOutputs outputs;
        
        outputs.main.pred_logits = out_dict.at("pred_logits").toTensor();
        outputs.main.pred_boxes = out_dict.at("pred_boxes").toTensor();
        if (include_masks && out_dict.contains("pred_masks")) {
            outputs.main.pred_masks = out_dict.at("pred_masks").toTensor();
        }
        if (out_dict.contains("sparse_spatial")) {
            OutputLayer::SparsePredMasks sparse;
            sparse.spatial_features = out_dict.at("sparse_spatial").toTensor();
            sparse.query_features = out_dict.at("sparse_query").toTensor();
            sparse.bias = out_dict.at("sparse_bias").toTensor();
            outputs.main.sparse_pred_masks = std::move(sparse);
        }

        if (out_dict.contains("enc_logits")) {
            OutputLayer enc_out;
            enc_out.pred_logits = out_dict.at("enc_logits").toTensor();
            enc_out.pred_boxes = out_dict.at("enc_boxes").toTensor();
            if (include_masks && out_dict.contains("enc_masks")) {
                enc_out.pred_masks = out_dict.at("enc_masks").toTensor();
            }
            if (out_dict.contains("enc_sparse_spatial")) {
                OutputLayer::SparsePredMasks sparse;
                sparse.spatial_features = out_dict.at("enc_sparse_spatial").toTensor();
                sparse.query_features = out_dict.at("enc_sparse_query").toTensor();
                sparse.bias = out_dict.at("enc_sparse_bias").toTensor();
                enc_out.sparse_pred_masks = std::move(sparse);
            }
            outputs.enc_outputs = std::move(enc_out);
        }

        for (size_t i = 0;; ++i) {
            std::string key_logits = "aux_logits_" + std::to_string(i);
            if (!out_dict.contains(key_logits)) {
                break;
            }
            OutputLayer aux_out;
            aux_out.pred_logits = out_dict.at(key_logits).toTensor();
            aux_out.pred_boxes = out_dict.at("aux_boxes_" + std::to_string(i)).toTensor();
            if (include_masks && out_dict.contains("aux_masks_" + std::to_string(i))) {
                aux_out.pred_masks = out_dict.at("aux_masks_" + std::to_string(i)).toTensor();
            }
            if (out_dict.contains("aux_sparse_spatial_" + std::to_string(i))) {
                OutputLayer::SparsePredMasks sparse;
                sparse.spatial_features = out_dict.at("aux_sparse_spatial_" + std::to_string(i)).toTensor();
                sparse.query_features = out_dict.at("aux_sparse_query_" + std::to_string(i)).toTensor();
                sparse.bias = out_dict.at("aux_sparse_bias_" + std::to_string(i)).toTensor();
                aux_out.sparse_pred_masks = std::move(sparse);
            }
            outputs.aux_outputs.push_back(std::move(aux_out));
        }

        return outputs;
    }

    FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward");
    if (!batch.tensors.defined()) {
        throw std::runtime_error("NativeRfDetrModel::forward requires batch.tensors");
    }

    NestedTensor samples = batch;
    if (!samples.mask.defined()) {
        samples.mask = torch::zeros(
            {samples.tensors.size(0), samples.tensors.size(2), samples.tensors.size(3)},
            torch::TensorOptions().dtype(torch::kBool).device(samples.tensors.device()));
    }

    std::vector<NestedTensor> features;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.backbone");
        const bool use_traced_backbone = is_train
            ? has_traced_backbone_train_ : has_traced_backbone_eval_;
        if (use_traced_backbone) {
            auto& traced = is_train ? traced_backbone_train_ : traced_backbone_eval_;
            auto feature = traced.forward({samples.tensors}).toTensor();
            torch::Tensor mask = F::interpolate(
                samples.mask.unsqueeze(1).to(torch::kFloat32),
                F::InterpolateFuncOptions()
                    .size(std::vector<int64_t>{feature.size(2), feature.size(3)})
                    .mode(torch::kNearest))
                .squeeze(1)
                .to(torch::kBool);
            features.push_back(NestedTensor{std::move(feature), std::move(mask)});
        } else {
            features = backbone_->at<NativeBackboneImpl>(0).forward(samples);
        }
    }
    std::vector<torch::Tensor> srcs;
    std::vector<torch::Tensor> masks;
    std::vector<torch::Tensor> poss;
    srcs.reserve(features.size());
    masks.reserve(features.size());
    poss.reserve(features.size());
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.position_embeddings");
        for (const auto& feature : features) {
            srcs.push_back(feature.tensors);
            masks.push_back(feature.mask);
            poss.push_back(backbone_->at<PositionEmbeddingSineImpl>(1).forward_full_valid(
                feature.tensors.size(0),
                feature.tensors.size(2),
                feature.tensors.size(3),
                feature.tensors.device()));
        }
    }

    const int64_t active_query_count =
        is_training() ? config_.num_queries * std::max(1, config_.group_detr) : config_.num_queries;
    const auto ref_weights = refpoint_embed->weight.narrow(0, 0, std::max<int64_t>(1, active_query_count));
    const auto query_weights = query_feat->weight.narrow(0, 0, std::max<int64_t>(1, active_query_count));

    auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_);
    auto bbox = std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_);
    if (!transformer || !bbox) {
        throw std::runtime_error("RF-DETR native modules are not initialized");
    }

    NativeTransformerOutput transformed;
    {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.transformer");
        transformed = transformer->forward(srcs, masks, poss, ref_weights, query_weights, is_training());
    }

    std::vector<torch::Tensor> decoder_query_features;
    if (transformed.hidden_states.defined()) {
        auto states = transformed.hidden_states.unbind(0);
        decoder_query_features.assign(states.begin(), states.end());
    }

    std::vector<torch::Tensor> dense_masks;
    std::vector<OutputLayer::SparsePredMasks> sparse_masks;
    if (include_masks && segmentation_head_ && !decoder_query_features.empty()) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.segmentation");
        auto* seg = seg_override ? seg_override
                                 : std::dynamic_pointer_cast<SegmentationHeadImpl>(segmentation_head_).get();
        if (!seg) {
            throw std::runtime_error("RF-DETR segmentation head is not initialized");
        }
        if (is_training()) {
            sparse_masks = seg->sparse_forward(
                features.front().tensors,
                decoder_query_features,
                {samples.tensors.size(2), samples.tensors.size(3)});
        } else {
            dense_masks = seg->forward(
                features.front().tensors,
                decoder_query_features,
                {samples.tensors.size(2), samples.tensors.size(3)});
        }
    }

    ModelOutputs outputs;
    if (!decoder_query_features.empty()) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.decoder_heads");
        const int64_t ref_layers = transformed.refs_unsigmoid.size(0);
        for (size_t index = 0; index < decoder_query_features.size(); ++index) {
            const int64_t ref_index = ref_layers == 1 ? 0 : static_cast<int64_t>(index);
            const auto& hs = decoder_query_features[index];
            auto refs = transformed.refs_unsigmoid.index({ref_index});

            torch::Tensor boxes;
            if (config_.bbox_reparam) {
                auto delta = bbox->forward(hs);
                auto cxcy =
                    delta.index({Slice(), Slice(), Slice(None, 2)}) * refs.index({Slice(), Slice(), Slice(2, None)}) +
                    refs.index({Slice(), Slice(), Slice(None, 2)});
                auto wh =
                    delta.index({Slice(), Slice(), Slice(2, None)}).exp() * refs.index({Slice(), Slice(), Slice(2, None)});
                boxes = torch::cat({cxcy, wh}, -1);
            } else {
                boxes = (bbox->forward(hs) + refs).sigmoid();
            }

            OutputLayer layer;
            layer.pred_logits = class_embed->forward(hs);
            layer.pred_boxes = boxes;
            if (include_masks && segmentation_head_) {
                if (is_training()) {
                    layer.sparse_pred_masks = sparse_masks[index];
                } else {
                    layer.pred_masks = dense_masks[index];
                }
            }

            if (index + 1 == decoder_query_features.size()) {
                outputs.main = std::move(layer);
            } else {
                outputs.aux_outputs.push_back(std::move(layer));
            }
        }
    }

    if (config_.two_stage && transformed.enc_memory.defined() && transformed.enc_boxes.defined()) {
        FASTLOADER_PROFILE_SCOPE("rfdetr.model.forward.encoder_heads");
        OutputLayer enc_output;
        enc_output.pred_logits = transformer->encoder_class_logits(transformed.enc_memory, is_training());
        enc_output.pred_boxes = transformed.enc_boxes;
        if (include_masks && segmentation_head_) {
            auto* enc_seg = seg_override ? seg_override
                                         : std::dynamic_pointer_cast<SegmentationHeadImpl>(segmentation_head_).get();
            if (is_training()) {
                enc_output.sparse_pred_masks = enc_seg->sparse_forward(
                    features.front().tensors,
                    {transformed.enc_memory},
                    {samples.tensors.size(2), samples.tensors.size(3)},
                    true)[0];
            } else {
                enc_output.pred_masks = enc_seg->forward(
                    features.front().tensors,
                    {transformed.enc_memory},
                    {samples.tensors.size(2), samples.tensors.size(3)},
                    true)[0];
            }
        }
        outputs.enc_outputs = std::move(enc_output);
    }

    return outputs;
}

std::shared_ptr<SegmentationHeadImpl> NativeRfDetrModel::clone_segmentation_head() const {
    if (!segmentation_head_) {
        return nullptr;
    }
    auto clone = std::make_shared<SegmentationHeadImpl>(
        config_.hidden_dim,
        std::max<int64_t>(1, config_.dec_layers));
    // Move to the same device as the model's segmentation head.
    // Weights are synced from the model before each forward in the lane.
    auto device = segmentation_head_->parameters().front().device();
    clone->to(device);
    return clone;
}

SegmentationHeadImpl* NativeRfDetrModel::segmentation_head_ptr() const {
    if (!segmentation_head_) {
        return nullptr;
    }
    return dynamic_cast<SegmentationHeadImpl*>(segmentation_head_.get());
}

StateDictLoadSummary NativeRfDetrModel::load_weights(const std::filesystem::path& weights_path, bool strict) {
    artifacts_ = resolve_upstream_weight_artifacts(weights_path);
    const auto checkpoint = load_checkpoint(weights_path);
    const int64_t original_output_classes = class_embed->weight.size(0);
    const auto checkpoint_classes = checkpoint_output_class_count(checkpoint);
    const bool resized_classes = checkpoint_classes.has_value() && *checkpoint_classes != original_output_classes;
    auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_);
    if (!transformer) {
        throw std::runtime_error("RF-DETR transformer is not initialized");
    }

    auto capture_detection_head_state = [&]() {
        DetectionHeadState state;
        state.class_embed = capture_linear_output_state(*class_embed);
        state.encoder_class_embeds = transformer->output_class_state();
        return state;
    };

    auto restore_detection_head_state = [&](const DetectionHeadState& state) {
        reinitialize_detection_head(state.class_embed.weight.size(0));
        restore_linear_output_state(*class_embed, state.class_embed);
        transformer->restore_output_class_state(state.encoder_class_embeds);
    };

    std::optional<DetectionHeadState> original_detection_head_state;
    if (resized_classes) {
        original_detection_head_state = capture_detection_head_state();
        reinitialize_detection_head(*checkpoint_classes);
    }

    try {
        const auto prepared = prepare_checkpoint_for_module(checkpoint, *this);
        auto summary = apply_checkpoint_to_module(*this, prepared, strict);
        if (resized_classes) {
            reinitialize_detection_head(original_output_classes);
        }
        return summary;
    } catch (...) {
        if (resized_classes) {
            restore_detection_head_state(*original_detection_head_state);
        }
        throw;
    }
}

void NativeRfDetrModel::optimize_for_inference(int batch_size, bool for_training, CompilationMode mode) {
    const bool already_compiled = for_training
        ? (is_compiled_train_ || has_traced_backbone_train_)
        : (is_compiled_eval_ || has_traced_backbone_eval_);
    if (already_compiled) return;

    if (for_training) {
        this->train();
    } else {
        this->eval();
    }

    if (mode == CompilationMode::kNone) {
        std::fprintf(stderr, "rfdetr: compilation disabled, using raw C++ forward for %s\n",
                     for_training ? "training" : "evaluation");
        return;
    }

    auto device = this->parameters().front().device();
    auto dtype = this->parameters().front().dtype();
    auto dummy_pixel_values = torch::zeros(
        {batch_size, 3, config_.resolution, config_.resolution},
        torch::TensorOptions().dtype(dtype).device(device));

    if (mode == CompilationMode::kSelective) {
        // Trace only the backbone (DINOv2 encoder + projector).
        // This is the dominant compute and is safely traceable (no custom CUDA
        // kernels, no data-dependent control flow — only config-constant branches).
        // The transformer, decoder, detection/segmentation heads run as raw C++.
        std::fprintf(stderr, "rfdetr: selectively compiling backbone for %s via torch::jit...\n",
                     for_training ? "training" : "evaluation");

        auto& backbone = backbone_->at<NativeBackboneImpl>(0);

        auto cu = std::make_shared<torch::jit::CompilationUnit>();
        auto cls_name = for_training
            ? "__torch__.NativeRfDetrBackboneTrain"
            : "__torch__.NativeRfDetrBackboneEval";
        auto cls = torch::jit::ClassType::create(cls_name, cu, true);

        auto* target = for_training ? &traced_backbone_train_ : &traced_backbone_eval_;
        *target = torch::jit::Module(cu, cls);

        for (const auto& kv : backbone.named_parameters(true)) {
            std::string name = kv.key();
            std::replace(name.begin(), name.end(), '.', '_');
            target->register_parameter(name, kv.value(), false);
        }
        for (const auto& kv : backbone.named_buffers(true)) {
            std::string name = kv.key();
            std::replace(name.begin(), name.end(), '.', '_');
            target->register_buffer(name, kv.value());
        }

        auto trace_res = torch::jit::tracer::trace(
            {dummy_pixel_values},
            [&](torch::jit::Stack args) -> torch::jit::Stack {
                return {backbone.forward_features(args[0].toTensor())};
            },
            [](const torch::autograd::Variable&) { return ""; },
            false, false, target
        );
        target->type()->addMethod(
            cu->create_function("forward", trace_res.first->graph, true));

        if (for_training) {
            has_traced_backbone_train_ = true;
        } else {
            has_traced_backbone_eval_ = true;
        }
        return;
    }

    // kFullTrace: trace the entire model (original behavior).
    std::fprintf(stderr, "rfdetr: compiling entire model %s graph via torch::jit...\n",
                 for_training ? "training" : "evaluation");

    auto dummy_mask = torch::zeros(
        {batch_size, config_.resolution, config_.resolution},
        torch::TensorOptions().dtype(torch::kBool).device(device));

    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls_name = for_training ? "__torch__.NativeRfDetrModelTrain" : "__torch__.NativeRfDetrModelEval";
    auto cls = torch::jit::ClassType::create(cls_name, cu, true);

    torch::jit::Module* target_traced_model = for_training ? &traced_model_train_ : &traced_model_eval_;
    *target_traced_model = torch::jit::Module(cu, cls);

    for (const auto& kv : this->named_parameters(true)) {
        std::string name = kv.key();
        std::replace(name.begin(), name.end(), '.', '_');
        target_traced_model->register_parameter(name, kv.value(), false);
    }
    for (const auto& kv : this->named_buffers(true)) {
        std::string name = kv.key();
        std::replace(name.begin(), name.end(), '.', '_');
        target_traced_model->register_buffer(name, kv.value());
    }

    std::pair<std::shared_ptr<torch::jit::tracer::TracingState>, torch::jit::Stack> trace_res;
    {
        // NOTE: NoWarn suppresses critical tracing warnings about data-dependent
        // control flow. Removed intentionally so warnings are visible.
        trace_res = torch::jit::tracer::trace(
            {dummy_pixel_values, dummy_mask},
            [&](torch::jit::Stack args) -> torch::jit::Stack {
                auto outputs = this->forward(NestedTensor{args[0].toTensor(), args[1].toTensor()});
                c10::Dict<std::string, torch::Tensor> dict;
                dict.insert("pred_logits", outputs.main.pred_logits);
                dict.insert("pred_boxes", outputs.main.pred_boxes);
                if (outputs.main.pred_masks) {
                    dict.insert("pred_masks", *outputs.main.pred_masks);
                }
                if (outputs.main.sparse_pred_masks) {
                    dict.insert("sparse_spatial", outputs.main.sparse_pred_masks->spatial_features);
                    dict.insert("sparse_query", outputs.main.sparse_pred_masks->query_features);
                    dict.insert("sparse_bias", outputs.main.sparse_pred_masks->bias);
                }
                if (outputs.enc_outputs) {
                    dict.insert("enc_logits", outputs.enc_outputs->pred_logits);
                    dict.insert("enc_boxes", outputs.enc_outputs->pred_boxes);
                    if (outputs.enc_outputs->pred_masks) {
                        dict.insert("enc_masks", *outputs.enc_outputs->pred_masks);
                    }
                    if (outputs.enc_outputs->sparse_pred_masks) {
                        dict.insert("enc_sparse_spatial", outputs.enc_outputs->sparse_pred_masks->spatial_features);
                        dict.insert("enc_sparse_query", outputs.enc_outputs->sparse_pred_masks->query_features);
                        dict.insert("enc_sparse_bias", outputs.enc_outputs->sparse_pred_masks->bias);
                    }
                }
                for (size_t i = 0; i < outputs.aux_outputs.size(); ++i) {
                    dict.insert("aux_logits_" + std::to_string(i), outputs.aux_outputs[i].pred_logits);
                    dict.insert("aux_boxes_" + std::to_string(i), outputs.aux_outputs[i].pred_boxes);
                    if (outputs.aux_outputs[i].pred_masks) {
                        dict.insert("aux_masks_" + std::to_string(i), *outputs.aux_outputs[i].pred_masks);
                    }
                    if (outputs.aux_outputs[i].sparse_pred_masks) {
                        dict.insert("aux_sparse_spatial_" + std::to_string(i), outputs.aux_outputs[i].sparse_pred_masks->spatial_features);
                        dict.insert("aux_sparse_query_" + std::to_string(i), outputs.aux_outputs[i].sparse_pred_masks->query_features);
                        dict.insert("aux_sparse_bias_" + std::to_string(i), outputs.aux_outputs[i].sparse_pred_masks->bias);
                    }
                }
                return {dict};
            },
            [](const torch::autograd::Variable&) { return ""; },
            false, false, target_traced_model
        );
    }
    target_traced_model->type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));
    if (for_training) {
        is_compiled_train_ = true;
    } else {
        is_compiled_eval_ = true;
    }
}

void NativeRfDetrModel::set_force_pytorch_deformable_attn(bool value) {
    for (auto& module : this->modules(false)) {
        if (auto* deform = dynamic_cast<MSDeformAttnImpl*>(module.get())) {
            deform->force_pytorch_deformable_attn_ = value;
        }
    }
}

void NativeRfDetrModel::export_onnx(const std::filesystem::path& output_path,
                                     int opset_version,
                                     int batch_size,
                                     bool simplify) {
    validate_supported_onnx_export_opset(opset_version);
    this->eval();
    set_force_pytorch_deformable_attn(true);

    auto device = this->parameters().front().device();
    auto dtype = this->parameters().front().dtype();
    auto dummy_pixel_values = torch::zeros(
        {batch_size, 3, config_.resolution, config_.resolution},
        torch::TensorOptions().dtype(dtype).device(device));

    auto cu = std::make_shared<torch::jit::CompilationUnit>();
    auto cls = torch::jit::ClassType::create("__torch__.NativeRfDetrOnnxExport", cu, true);
    torch::jit::Module export_module(cu, cls);

    for (const auto& kv : this->named_parameters(true)) {
        std::string name = kv.key();
        std::replace(name.begin(), name.end(), '.', '_');
        export_module.register_parameter(name, kv.value(), false);
    }
    for (const auto& kv : this->named_buffers(true)) {
        std::string name = kv.key();
        std::replace(name.begin(), name.end(), '.', '_');
        export_module.register_buffer(name, kv.value());
    }

    const bool has_masks = config_.segmentation;
    auto dummy_mask = torch::zeros(
        {batch_size, config_.resolution, config_.resolution},
        torch::TensorOptions().dtype(torch::kBool).device(device));
    auto trace_res = torch::jit::tracer::trace(
        {dummy_pixel_values},
        [&](torch::jit::Stack args) -> torch::jit::Stack {
            auto pixel_values = args[0].toTensor();
            auto outputs = this->forward(NestedTensor{pixel_values, dummy_mask});
            if (has_masks && outputs.main.pred_masks) {
                return {outputs.main.pred_logits, outputs.main.pred_boxes, *outputs.main.pred_masks};
            }
            return {outputs.main.pred_logits, outputs.main.pred_boxes};
        },
        [](const torch::autograd::Variable&) { return ""; },
        false, false, &export_module);

    export_module.type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));

    std::map<std::string, at::Tensor> initializers;
    OnnxInitializerMap lowering_initializers;
    for (const auto& kv : this->named_parameters(true)) {
        initializers[kv.key()] = kv.value();
        lowering_initializers.emplace(kv.key(), kv.value());
        std::string flattened_name = kv.key();
        std::replace(flattened_name.begin(), flattened_name.end(), '.', '_');
        lowering_initializers.emplace(std::move(flattened_name), kv.value());
    }
    for (const auto& kv : this->named_buffers(true)) {
        initializers[kv.key()] = kv.value();
        lowering_initializers.emplace(kv.key(), kv.value());
        std::string flattened_name = kv.key();
        std::replace(flattened_name.begin(), flattened_name.end(), '.', '_');
        lowering_initializers.emplace(std::move(flattened_name), kv.value());
    }

    auto graph = trace_res.first->graph;
    lower_graph_for_onnx_export(graph, &lowering_initializers);
    erase_unused_module_self_input(graph);

    std::map<std::string, at::Tensor> export_initializers;
    for (auto* input : graph->inputs()) {
        const std::string input_name = input->debugName();
        if (const auto it = initializers.find(input_name); it != initializers.end()) {
            export_initializers.emplace(it->first, it->second);
            continue;
        }
        if (const auto it = lowering_initializers.find(input_name); it != lowering_initializers.end()) {
            export_initializers.emplace(input_name, it->second);
        }
    }

    std::unordered_map<std::string, std::unordered_map<int64_t, std::string>> dynamic_axes;

    auto [model_proto, raw_data, sym_dim_map, use_external_data_format, node_names] =
        torch::jit::export_onnx(
            graph,
            export_initializers,
            static_cast<int64_t>(opset_version),
            dynamic_axes,
            false,
            ::torch::onnx::OperatorExportTypes::ONNX,
            true,
            false,
            {},
            true,
            false,
            output_path.string());

    if (simplify) {
        run_onnx_simplify(*model_proto);
    }

    std::string serialized = torch::jit::serialize_model_proto_to_string(model_proto);
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        set_force_pytorch_deformable_attn(false);
        throw std::runtime_error("failed to open ONNX output file: " + output_path.string());
    }
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    out.close();

    set_force_pytorch_deformable_attn(false);
    std::fprintf(stderr, "onnx: exported model to %s\n", output_path.c_str());
}

void export_weights_to_onnx(const std::filesystem::path& weights_path,
                            const std::filesystem::path& output_path,
                            int device_id,
                            int opset_version,
                            bool simplify) {
    auto artifacts = resolve_upstream_weight_artifacts(weights_path);
    auto device = torch::Device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_id));
    NativeRfDetrModel model(artifacts.config);
    model.to(device);
    model.load_weights(weights_path);
    model.export_onnx(output_path, opset_version, 1, simplify);
}

} // namespace fastloader::rfdetr
