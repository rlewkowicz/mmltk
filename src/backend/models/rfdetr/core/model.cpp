module;
#include <ATen/Context.h>
#include <ATen/TensorIndexing.h>
#include <ATen/ops/scaled_dot_product_attention.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/nn/functional.h>
#include <torch/script.h>
#include <torch/torch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "detail/model_technical.h"
#include "detail/modules_technical.h"
#include "detail/training_supervision.h"
#include "ms_deform_attn.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/common/math/checked_arithmetic.h"

module mmltk.backend.models.rfdetr.core.model;

import mmltk.backend.ml.cuda.gpu_quiescence;
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

namespace F = torch::nn::functional;

namespace mmltk::backend::models::rfdetr {

using mmltk::backend::ml::layers::ms_deform_attn_cuda_autograd;
using mmltk::backend::ml::layers::ms_deform_attn_reference;

struct NativeRfDetrModel::Impl final : torch::nn::Module, detail::NativeModelTechnicalOwner {
    explicit Impl(const NativeRfDetrConfig& config, ModelClassLayout layout);
    std::shared_ptr<const ResolvedClassLayout> layout_;

    [[nodiscard]] torch::nn::Module& module() noexcept override { return *this; }
    [[nodiscard]] const torch::nn::Module& module() const noexcept override { return *this; }
    ModelOutputs forward(const NestedTensor& batch, bool include_masks = true) override;
    ModelOutputs forward_for_match_free(const NestedTensor& batch) override;
    void initialize_training_supervision(std::uint64_t request_seed) override;
    TrainingLoss supervision_loss(const ModelOutputs& outputs, const PreparedTargets& targets, const DeviceLossNormalizer& normalizer,
                                  bool training_mode) override;
    void configure_supervision_timing(const SupervisionTimingSetup& setup) override;
    void begin_supervised_step_timing() override;
    void end_supervised_step_timing() override;
    void begin_criterion_timing() override;
    void end_criterion_timing() override;
    SupervisionTimingHandoff harvest_supervision_timing() override;
    ModelStateLoadSummary load_normalized_state(const std::vector<NormalizedModelStateEntry>& state, bool strict = false, const ModelClassLayout* admitted_layout = nullptr) override;
    detail::NormalizedModelStateCandidate stage_normalized_state(const std::vector<NormalizedModelStateEntry>& state,
                                                                 detail::NormalizedModelStateAdmission admission, const ResolvedClassLayout* source_layout = nullptr) override;
    void commit_normalized_state(detail::NormalizedModelStateCandidate candidate) override;
    void optimize_for_inference(int batch_size, bool for_training, CompilationMode mode);
    void set_force_pytorch_deformable_attn(bool value) override;
    [[nodiscard]] const NativeRfDetrConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool is_compiled(const bool for_training) const noexcept { return for_training ? is_compiled_train_ : is_compiled_eval_; }

   protected:
    TrainingSupervisionRuntimeState export_training_supervision_runtime() const override;
    void import_training_supervision_runtime(const TrainingSupervisionRuntimeState& state) override;

   private:
    [[nodiscard]] std::vector<detail::ClassTensorAxis> class_axes() const;
    ModelOutputs forward_impl(const NestedTensor& batch, bool include_masks, bool capture_match_free_features,
                              const DenoisingQueryBatch* denoising = nullptr);
    ModelOutputs forward_with_denoising(const NestedTensor& batch, const PreparedTargets& targets,
                                        const TrainingStepIdentity& identity) override;

    NativeRfDetrConfig config_;
    torch::nn::ModuleList backbone_{nullptr};
    std::shared_ptr<torch::nn::Module> transformer_;
    torch::nn::Linear class_embed{nullptr};
    std::shared_ptr<torch::nn::Module> bbox_embed_;
    torch::nn::Embedding refpoint_embed{nullptr};
    torch::nn::Embedding query_feat{nullptr};
    std::shared_ptr<torch::nn::Module> segmentation_head_;
    std::shared_ptr<TrainingSupervisionImpl> training_supervision_;
    bool is_compiled_eval_ = false;
    bool is_compiled_train_ = false;
    torch::jit::Module traced_model_eval_;
    torch::jit::Module traced_model_train_;
    bool has_traced_backbone_eval_ = false;
    bool has_traced_backbone_train_ = false;
    torch::jit::Module traced_backbone_eval_;
    torch::jit::Module traced_backbone_train_;
};

namespace {

using namespace torch::indexing;

constexpr int64_t kDinoHiddenSize = 384;
constexpr int64_t kDinoNumLayers = 12;
constexpr int64_t kDinoNumHeads = 6;
constexpr int64_t kProjectorBlocks = 3;
constexpr std::array<int64_t, 4> kOutFeatureStages = {3, 6, 9, 12};

// Backbone features come out at their own spatial resolution, so the padding mask has to be rebound
// to each feature map. Every backbone path does it the same way, so the resize lives here once.
torch::Tensor resize_mask_to_feature(const torch::Tensor& mask, const torch::Tensor& feature) {
    return F::interpolate(mask.unsqueeze(1).to(torch::kFloat32),
                          F::InterpolateFuncOptions().size(std::vector<int64_t>{feature.size(2), feature.size(3)}).mode(torch::kNearest))
        .squeeze(1)
        .to(torch::kBool);
}

// Reparameterized box refinement: scales the delta's center offset by the base extent and applies the delta's
// log-space width/height factor to the base extent. Inputs are [batch, queries, 4] cxcywh tensors.
torch::Tensor reparam_box_refine(const torch::Tensor& base, const torch::Tensor& delta) {
    auto cxcy = delta.index({Slice(), Slice(), Slice(None, 2)}) * base.index({Slice(), Slice(), Slice(2, None)}) +
                base.index({Slice(), Slice(), Slice(None, 2)});
    auto wh = delta.index({Slice(), Slice(), Slice(2, None)}).exp() * base.index({Slice(), Slice(), Slice(2, None)});
    return torch::cat({cxcy, wh}, -1);
}

// Copies source into destination when the optional metadata field carries a value.
template <typename Source, typename Destination>
void apply_if_set(const std::optional<Source>& source, Destination& destination) {
    if (source.has_value()) { destination = *source; }
}

struct NativeTransformerOutput {
    torch::Tensor hidden_states;
    torch::Tensor refs_unsigmoid;
    torch::Tensor enc_memory;
    torch::Tensor enc_boxes;
};

void append_classifier_axes(std::vector<detail::ClassTensorAxis>& axes, const torch::nn::Module& module, const detail::ClassTensorFamily& family) {
    const auto prefix = family.prefix();
    for (const auto& parameter : module.named_parameters(true)) {
        const auto name = prefix + parameter.key();
        if (const auto axis = family.Match(name)) axes.push_back({name, axis->dimension, axis->coordinates});
    }
}

torch::Tensor gen_sineembed_for_position(const torch::Tensor& pos_tensor, int64_t dim) {
    const double scale = 2.0 * M_PI;
    auto dim_t = torch::arange(dim, torch::TensorOptions().dtype(pos_tensor.dtype()).device(pos_tensor.device()));
    dim_t = torch::pow(torch::full_like(dim_t, 10000.0), 2.0 * torch::floor(dim_t / 2.0) / static_cast<double>(dim));

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

bool is_out_feature_stage(int64_t stage) { return std::ranges::find(kOutFeatureStages, stage) != kOutFeatureStages.end(); }

class LayerNorm2dImpl : public torch::nn::Module {
   public:
    explicit LayerNorm2dImpl(int64_t normalized_shape, double eps = 1.0e-6)
        : weight(register_parameter("weight", torch::ones({normalized_shape}, torch::kFloat32))),
          bias(register_parameter("bias", torch::zeros({normalized_shape}, torch::kFloat32))),
          eps_(eps) {}

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
        : conv(register_module(
              "conv",
              torch::nn::Conv2d(
                  torch::nn::Conv2dOptions(in_planes, out_planes, kernel).stride(stride).padding(kernel / 2).groups(groups).bias(false)))),
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
              "projection", torch::nn::Conv2d(torch::nn::Conv2dOptions(3, kDinoHiddenSize, patch_size).stride(patch_size).bias(true)))),
          patch_size_(patch_size) {}

    torch::Tensor forward(const torch::Tensor& pixel_values) { return projection->forward(pixel_values).flatten(2).transpose(1, 2); }

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
              torch::randn({1, config.positional_encoding_size * config.positional_encoding_size + 1, kDinoHiddenSize}, torch::kFloat32))),
          patch_embeddings(register_module("patch_embeddings", Dinov2PatchEmbeddings(config.patch_size))),
          patch_size_(config.patch_size),
          num_windows_(std::max<int64_t>(1, config.num_windows)),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))) {}

    torch::Tensor interpolate_pos_encoding(const torch::Tensor& embeddings, int64_t height, int64_t width) const {
        const int64_t num_patches = embeddings.size(1) - 1;
        const int64_t num_positions = position_embeddings.size(1) - 1;
        if (num_patches == num_positions && height == width) { return position_embeddings; }

        auto class_pos_embed = position_embeddings.index({Slice(), 0});
        auto patch_pos_embed = position_embeddings.index({Slice(), Slice(1, None)});
        const int64_t dim = embeddings.size(-1);
        const int64_t patch_height = height / patch_size_;
        const int64_t patch_width = width / patch_size_;
        const auto sqrt_num_positions = static_cast<int64_t>(std::llround(std::sqrt(static_cast<double>(num_positions))));
        patch_pos_embed = patch_pos_embed.view({1, sqrt_num_positions, sqrt_num_positions, dim}).permute({0, 3, 1, 2});
        patch_pos_embed = F::interpolate(patch_pos_embed.to(torch::kFloat32), F::InterpolateFuncOptions()
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
            pixel_tokens_with_pos_embed = pixel_tokens_with_pos_embed.view({batch_size, num_h_patches, num_w_patches, kDinoHiddenSize});
            auto windowed_pixel_tokens =
                pixel_tokens_with_pos_embed
                    .reshape({batch_size * num_windows_, num_h_patches_per_window, num_windows_, num_w_patches_per_window, kDinoHiddenSize})
                    .permute({0, 2, 1, 3, 4})
                    .reshape(
                        {batch_size * num_windows_ * num_windows_, num_h_patches_per_window * num_w_patches_per_window, kDinoHiddenSize});
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
        auto context = at::scaled_dot_product_attention(query_layer, key_layer, value_layer, c10::nullopt, dropout_p, false, scale);
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

    torch::Tensor forward(const torch::Tensor& hidden_states) { return dropout->forward(dense->forward(hidden_states)); }

    torch::nn::Linear dense{nullptr};

   private:
    torch::nn::Dropout dropout{nullptr};
};
TORCH_MODULE(Dinov2SelfOutput);

class Dinov2AttentionImpl : public torch::nn::Module {
   public:
    Dinov2AttentionImpl()
        : attention(register_module("attention", Dinov2SelfAttention())), output(register_module("output", Dinov2SelfOutput())) {}

    torch::Tensor forward(const torch::Tensor& hidden_states) { return output->forward(attention->forward(hidden_states)); }

   private:
    Dinov2SelfAttention attention{nullptr};
    Dinov2SelfOutput output{nullptr};
};
TORCH_MODULE(Dinov2Attention);

class Dinov2LayerScaleImpl : public torch::nn::Module {
   public:
    Dinov2LayerScaleImpl() : lambda1(register_parameter("lambda1", torch::ones({kDinoHiddenSize}, torch::kFloat32))) {}

    torch::Tensor forward(const torch::Tensor& hidden_state) const { return hidden_state * lambda1; }

    torch::Tensor lambda1;
};
TORCH_MODULE(Dinov2LayerScale);

class Dinov2MlpImpl : public torch::nn::Module {
   public:
    Dinov2MlpImpl()
        : fc1(register_module("fc1", torch::nn::Linear(kDinoHiddenSize, kDinoHiddenSize * 4))),
          fc2(register_module("fc2", torch::nn::Linear(kDinoHiddenSize * 4, kDinoHiddenSize))) {}

    torch::Tensor forward(const torch::Tensor& hidden_state) { return fc2->forward(torch::gelu(fc1->forward(hidden_state))); }

   private:
    torch::nn::Linear fc1{nullptr};
    torch::nn::Linear fc2{nullptr};
};
TORCH_MODULE(Dinov2Mlp);

class WindowedDinov2LayerImpl : public torch::nn::Module {
   public:
    explicit WindowedDinov2LayerImpl(int64_t num_windows)
        : num_windows_(std::max<int64_t>(1, num_windows)),
          norm1(register_module("norm1", torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))),
          attention(register_module("attention", Dinov2Attention())),
          layer_scale1(register_module("layer_scale1", Dinov2LayerScale())),
          norm2(register_module("norm2", torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))),
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
    explicit WindowedDinov2EncoderImpl(int64_t num_windows) : layer(register_module("layer", torch::nn::ModuleList())) {
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
          layernorm(register_module("layernorm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({kDinoHiddenSize}).eps(1.0e-6)))) {}

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
                    {batch_size * num_windows_, num_windows_, num_h_patches_per_window, num_w_patches_per_window, kDinoHiddenSize});
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
    explicit DinoV2WrapperImpl(const NativeRfDetrConfig& config) : encoder(register_module("encoder", WindowedDinov2Backbone(config))) {}

    std::vector<torch::Tensor> forward(const torch::Tensor& tensors) { return encoder->forward(tensors); }

   private:
    WindowedDinov2Backbone encoder{nullptr};
};
TORCH_MODULE(DinoV2Wrapper);

class NativeBackboneProjectorImpl : public torch::nn::Module {
   public:
    explicit NativeBackboneProjectorImpl(int64_t hidden_dim) : stages(register_module("stages", torch::nn::ModuleList())) {
        auto stage = torch::nn::Sequential();
        stage->push_back(C2f(kDinoHiddenSize * static_cast<int64_t>(kOutFeatureStages.size()), hidden_dim, kProjectorBlocks, true));
        stage->push_back(LayerNorm2d(hidden_dim));
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

class NativeBackboneImpl : public torch::nn::Module {
   public:
    explicit NativeBackboneImpl(const NativeRfDetrConfig& config)
        : encoder(register_module("encoder", DinoV2Wrapper(config))),
          projector(register_module("projector", NativeBackboneProjector(config.hidden_dim))) {}

    std::vector<NestedTensor> forward(const NestedTensor& samples) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_backbone{"rfdetr.model.backbone"};
        std::vector<torch::Tensor> projected = projector->forward(encoder->forward(samples.tensors));
        std::vector<NestedTensor> out;
        out.reserve(projected.size());
        for (auto& feature : projected) {
            torch::Tensor mask;
            if (samples.mask.defined()) {
                mask = resize_mask_to_feature(samples.mask, feature);
            } else {
                mask = torch::zeros({feature.size(0), feature.size(2), feature.size(3)},
                                    torch::TensorOptions().dtype(torch::kBool).device(feature.device()));
            }
            out.push_back(NestedTensor{std::move(feature), std::move(mask)});
        }
        return out;
    }

    torch::Tensor forward_features(const torch::Tensor& pixel_values) {
        auto projected = projector->forward(encoder->forward(pixel_values));
        return projected[0];
    }

   private:
    DinoV2Wrapper encoder{nullptr};
    NativeBackboneProjector projector{nullptr};
};

}  // namespace

namespace {

class FeatureLayout {
   public:
    FeatureLayout(std::vector<int64_t> shape_values, const torch::Device& device, const c10::ScalarType scalar_type)
        : shape_values_(std::move(shape_values)), device_(device), scalar_type_(scalar_type) {
        if (shape_values_.empty() || shape_values_.size() % 2U != 0U) {
            throw std::invalid_argument("RF-DETR feature layout requires height/width pairs");
        }

        spatial_shapes_ = torch::tensor(shape_values_, torch::TensorOptions().dtype(torch::kLong).device(torch::kCPU))
                              .view({static_cast<int64_t>(shape_values_.size() / 2U), 2})
                              .contiguous();
        level_start_index_ = torch::cat({spatial_shapes_.new_zeros({1}), spatial_shapes_.prod(1).cumsum(0).slice(0, 0, -1)}).contiguous();
        offset_normalizer_ = torch::stack({spatial_shapes_.select(1, 1), spatial_shapes_.select(1, 0)}, -1)
                                 .to(device_, scalar_type_, false, true)
                                 .contiguous();
    }

    void require_compatible(const std::vector<int64_t>& shape_values, const torch::Device& device,
                            const c10::ScalarType scalar_type) const {
        if (shape_values_ != shape_values) { throw std::invalid_argument("RF-DETR feature layout changed after static construction"); }
        if (device_ != device || scalar_type_ != scalar_type) {
            throw std::invalid_argument("RF-DETR feature layout has one fixed device projection");
        }
    }

    [[nodiscard]] const torch::Tensor& spatial_shapes() const noexcept { return spatial_shapes_; }

    [[nodiscard]] const torch::Tensor& level_start_index() const noexcept { return level_start_index_; }

    [[nodiscard]] const torch::Tensor& offset_normalizer() const noexcept { return offset_normalizer_; }

   private:
    std::vector<int64_t> shape_values_;
    torch::Device device_;
    c10::ScalarType scalar_type_;
    torch::Tensor spatial_shapes_;
    torch::Tensor level_start_index_;
    torch::Tensor offset_normalizer_;
};

class MSDeformAttnImpl : public torch::nn::Module {
   public:
    MSDeformAttnImpl(int64_t d_model, int64_t n_levels, int64_t n_heads, int64_t n_points)
        : d_model_(d_model),
          n_levels_(n_levels),
          n_heads_(n_heads),
          n_points_(n_points),
          sampling_offsets(register_module("sampling_offsets", torch::nn::Linear(d_model, n_heads * n_levels * n_points * 2))),
          attention_weights(register_module("attention_weights", torch::nn::Linear(d_model, n_heads * n_levels * n_points))),
          value_proj(register_module("value_proj", torch::nn::Linear(d_model, d_model))),
          output_proj(register_module("output_proj", torch::nn::Linear(d_model, d_model))) {
        if (d_model_ % n_heads_ != 0) { throw std::runtime_error("RF-DETR MSDeformAttn requires d_model divisible by n_heads"); }
        reset_parameters();
    }

    torch::Tensor forward(const torch::Tensor& query, const torch::Tensor& reference_points, const torch::Tensor& input_flatten,
                          const FeatureLayout& feature_layout, const torch::Tensor& input_padding_mask) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_ms_deform_attn{"rfdetr.model.ms_deform_attn"};
        const int64_t batch = query.size(0);
        const int64_t len_q = query.size(1);
        const int64_t len_in = input_flatten.size(1);

        auto value = value_proj->forward(input_flatten);
        if (input_padding_mask.defined()) { value = value.masked_fill(input_padding_mask.unsqueeze(-1), 0.0); }

        auto offsets = sampling_offsets->forward(query).view({batch, len_q, n_heads_, n_levels_, n_points_, 2});
        auto attention = torch::softmax(attention_weights->forward(query).view({batch, len_q, n_heads_, n_levels_ * n_points_}), -1)
                             .view({batch, len_q, n_heads_, n_levels_, n_points_});

        torch::Tensor sampling_locations;
        if (reference_points.size(-1) == 2) {
            sampling_locations =
                reference_points.unsqueeze(2).unsqueeze(4) + offsets / feature_layout.offset_normalizer().view({1, 1, 1, n_levels_, 1, 2});
        } else if (reference_points.size(-1) == 4) {
            sampling_locations = reference_points.index({Slice(), Slice(), Slice(), Slice(None, 2)}).unsqueeze(2).unsqueeze(4) +
                                 offsets / static_cast<double>(n_points_) *
                                     reference_points.index({Slice(), Slice(), Slice(), Slice(2, None)}).unsqueeze(2).unsqueeze(4) * 0.5;
        } else {
            throw std::runtime_error("RF-DETR MSDeformAttn expects reference_points last dim 2 or 4");
        }

        value = value.view({batch, len_in, n_heads_, d_model_ / n_heads_});
        torch::Tensor attended;
        if (value.is_cuda() && !force_pytorch_deformable_attn_) {
            attended = ms_deform_attn_cuda_autograd(value, feature_layout.spatial_shapes(), feature_layout.level_start_index(),
                                                    sampling_locations, attention, im2col_step_);
        } else {
            attended = ms_deform_attn_reference(value, feature_layout.spatial_shapes(), sampling_locations, attention);
        }
        return output_proj->forward(attended);
    }

   private:
    void reset_parameters() {
        torch::NoGradGuard no_grad;
        sampling_offsets->weight.zero_();
        auto thetas = torch::arange(n_heads_, torch::TensorOptions().dtype(torch::kFloat32)) * (2.0 * M_PI / static_cast<double>(n_heads_));
        auto grid_init = torch::stack({thetas.cos(), thetas.sin()}, -1);
        grid_init = (grid_init / std::get<0>(grid_init.abs().max(-1, true))).view({n_heads_, 1, 1, 2}).repeat({1, n_levels_, n_points_, 1});
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
    NativeDecoderLayerImpl(int64_t d_model, int64_t sa_nhead, int64_t ca_nhead, int64_t dim_feedforward, int64_t group_detr,
                           int64_t num_feature_levels, int64_t dec_n_points)
        : group_detr_(std::max<int64_t>(1, group_detr)),
          self_attn(register_module("self_attn",
                                    torch::nn::MultiheadAttention(torch::nn::MultiheadAttentionOptions(d_model, sa_nhead).dropout(0.0)))),
          dropout1(register_module("dropout1", torch::nn::Dropout(0.0))),
          norm1(register_module("norm1", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          cross_attn(register_module("cross_attn", MSDeformAttn(d_model, num_feature_levels, ca_nhead, dec_n_points))),
          linear1(register_module("linear1", torch::nn::Linear(d_model, dim_feedforward))),
          dropout(register_module("dropout", torch::nn::Dropout(0.0))),
          linear2(register_module("linear2", torch::nn::Linear(dim_feedforward, d_model))),
          norm2(register_module("norm2", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          norm3(register_module("norm3", torch::nn::LayerNorm(std::vector<int64_t>{d_model}))),
          dropout2(register_module("dropout2", torch::nn::Dropout(0.0))),
          dropout3(register_module("dropout3", torch::nn::Dropout(0.0))) {}

    torch::Tensor forward(const torch::Tensor& tgt, const torch::Tensor& memory, const torch::Tensor& memory_key_padding_mask,
                          const torch::Tensor& query_pos, const torch::Tensor& reference_points, const FeatureLayout& feature_layout,
                          const DecoderQueryLayout& query_layout) {
        const int64_t batch = tgt.size(0);
        const int64_t num_queries = tgt.size(1);
        if (query_layout.has_denoising()) {
            const auto tgt2 = isolated_group_self_attention(self_attn, tgt, query_pos, query_layout);
            auto output = norm1->forward(tgt + dropout1->forward(tgt2));
            auto cross_output = cross_attn->forward(output + query_pos, reference_points, memory, feature_layout, memory_key_padding_mask);
            output = norm2->forward(output + dropout2->forward(cross_output));
            auto feed_forward = linear2->forward(dropout->forward(torch::relu(linear1->forward(output))));
            return norm3->forward(output + dropout3->forward(feed_forward));
        }
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
        if (is_training() && group_detr_ > 1) { tgt2 = torch::cat(tgt2.split(batch, 1), 0); }
        tgt2 = tgt2.transpose(0, 1);

        auto output = norm1->forward(tgt + dropout1->forward(tgt2));
        tgt2 = cross_attn->forward(output + query_pos, reference_points, memory, feature_layout, memory_key_padding_mask);
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
          ref_point_head(register_module("ref_point_head",
                                         std::make_shared<RfDetrMlpImpl>(2 * config.hidden_dim, config.hidden_dim, config.hidden_dim, 2))) {
        for (int64_t index = 0; index < num_layers_; ++index) {
            layers->push_back(NativeDecoderLayer(config.hidden_dim, config.sa_nheads, config.ca_nheads, config.dim_feedforward,
                                                 config.group_detr, 1, config.dec_n_points));
        }
    }

    void set_bbox_embed(const std::shared_ptr<RfDetrMlpImpl>& bbox_embed) { bbox_embed_ = bbox_embed; }

    std::pair<torch::Tensor, torch::Tensor> forward(const torch::Tensor& tgt, const torch::Tensor& memory,
                                                    const torch::Tensor& memory_key_padding_mask, const torch::Tensor& pos,
                                                    const torch::Tensor& refpoints_unsigmoid, const FeatureLayout& feature_layout,
                                                    const torch::Tensor& valid_ratios, const DecoderQueryLayout& query_layout) {
        auto output = tgt;
        auto refpoints = refpoints_unsigmoid;

        std::vector<torch::Tensor> intermediate;
        intermediate.reserve(static_cast<size_t>(num_layers_));
        std::vector<torch::Tensor> hs_refpoints_unsigmoid;
        hs_refpoints_unsigmoid.push_back(refpoints_unsigmoid);

        auto refine = [&](const torch::Tensor& base, const torch::Tensor& delta) {
            if (bbox_reparam_) { return reparam_box_refine(base, delta); }
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

            output = layers[layer_id]->as<NativeDecoderLayer>()->forward(output, memory, memory_key_padding_mask, query_pos,
                                                                         refpoints_input, feature_layout, query_layout);

            if (!lite_refpoint_refine_ && bbox_embed_) {
                auto new_refpoints = refine(refpoints, bbox_embed_->forward(output));
                if (layer_id != num_layers_ - 1) { hs_refpoints_unsigmoid.push_back(new_refpoints); }
                refpoints = new_refpoints.detach();
            }

            intermediate.push_back(norm->forward(output));
            (void)pos;
        }

        if (bbox_embed_) { return {torch::stack(intermediate), torch::stack(hs_refpoints_unsigmoid)}; }
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

std::pair<torch::Tensor, torch::Tensor> gen_encoder_output_proposals(const torch::Tensor& memory, const torch::Tensor& memory_padding_mask,
                                                                     const torch::Tensor& spatial_shapes, bool unsigmoid) {
    const int64_t batch = memory.size(0);
    std::vector<torch::Tensor> proposals;
    proposals.reserve(static_cast<size_t>(spatial_shapes.size(0)));

    int64_t start = 0;
    for (int64_t level = 0; level < spatial_shapes.size(0); ++level) {
        const auto height = spatial_shapes.index({level, 0}).item<int64_t>();
        const auto width = spatial_shapes.index({level, 1}).item<int64_t>();

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
    // Tensor::operator& performs the required elementwise conjunction; these
    // expressions are tensors rather than scalar bool operands.
    // cppcheck-suppress bitwiseOnBoolean
    auto output_proposals_valid = ((output_proposals > 0.01) & (output_proposals < 0.99)).all(-1, true);

    if (unsigmoid) {
        output_proposals = torch::log(output_proposals / (1.0 - output_proposals));
        if (memory_padding_mask.defined()) {
            output_proposals = output_proposals.masked_fill(memory_padding_mask.unsqueeze(-1), std::numeric_limits<float>::infinity());
        }
        output_proposals = output_proposals.masked_fill(~output_proposals_valid, std::numeric_limits<float>::infinity());
    } else {
        if (memory_padding_mask.defined()) { output_proposals = output_proposals.masked_fill(memory_padding_mask.unsqueeze(-1), 0.0); }
        output_proposals = output_proposals.masked_fill(~output_proposals_valid, 0.0);
    }

    auto output_memory = memory;
    if (memory_padding_mask.defined()) { output_memory = output_memory.masked_fill(memory_padding_mask.unsqueeze(-1), 0.0); }
    output_memory = output_memory.masked_fill(~output_proposals_valid, 0.0);
    return {output_memory.to(memory.dtype()), output_proposals.to(memory.dtype())};
}

class NativeTransformerImpl : public torch::nn::Module {
   public:
    explicit NativeTransformerImpl(const NativeRfDetrConfig& config)
        : decoder(register_module("decoder", std::make_shared<NativeDecoderImpl>(config))),
          enc_output(register_module("enc_output", torch::nn::ModuleList())),
          enc_output_norm(register_module("enc_output_norm", torch::nn::ModuleList())),
          enc_out_class_embed(register_module(std::string(detail::kEncoderClassAxis.module_name()), torch::nn::ModuleList())),
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
        const auto bias_value = static_cast<float>(-std::log((1.0 - prior_prob) / prior_prob));
        for (auto& module : *enc_out_class_embed) {
            auto linear = module->as<torch::nn::Linear>();
            linear->bias.fill_(bias_value);
        }
    }

    void set_decoder_bbox_embed(const std::shared_ptr<RfDetrMlpImpl>& bbox_embed) { decoder->set_bbox_embed(bbox_embed); }

    void append_class_axes(std::vector<detail::ClassTensorAxis>& axes) const {
        append_classifier_axes(axes, *enc_out_class_embed, detail::kEncoderClassAxis);
    }

    torch::Tensor encoder_class_logits(const torch::Tensor& hs_enc, bool training_mode) {
        if (!hs_enc.defined()) { return {}; }
        const int64_t groups = training_mode ? std::max<int64_t>(1, config_.group_detr) : 1;
        auto hs_chunks = hs_enc.chunk(groups, 1);
        std::vector<torch::Tensor> logits;
        logits.reserve(static_cast<size_t>(groups));
        for (int64_t index = 0; index < groups; ++index) {
            logits.push_back(
                enc_out_class_embed->at<torch::nn::LinearImpl>(static_cast<size_t>(index)).forward(hs_chunks[static_cast<size_t>(index)]));
        }
        return torch::cat(logits, 1);
    }

    NativeTransformerOutput forward(const std::vector<torch::Tensor>& srcs, const std::vector<torch::Tensor>& masks,
                                    const std::vector<torch::Tensor>& pos_embeds, const torch::Tensor& refpoint_embed,
                                    const torch::Tensor& query_feat, bool training_mode, const DenoisingQueryBatch* denoising,
                                    TrainingSupervisionImpl* supervision) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_transformer{"rfdetr.model.transformer"};
        if (srcs.empty()) { throw std::runtime_error("RF-DETR transformer requires at least one source feature"); }

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
            mmltk::common::logging::ScopedProfile profile_rfdetr_model_transformer_flatten_inputs{
                "rfdetr.model.transformer.flatten_inputs"};
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

        const auto& layout = feature_layout(spatial_shape_values, memory.device(), memory.scalar_type());

        torch::Tensor hidden_states;
        torch::Tensor refs_unsigmoid;
        torch::Tensor enc_memory;
        torch::Tensor enc_boxes;
        torch::Tensor decoder_refpoints;

        if (config_.two_stage) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_model_transformer_two_stage{"rfdetr.model.transformer.two_stage"};
            auto [output_memory, output_proposals] =
                gen_encoder_output_proposals(memory, mask_flatten_tensor, layout.spatial_shapes(), !config_.bbox_reparam);

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

                const auto coord_delta =
                    enc_out_bbox_embed->at<RfDetrMlpImpl>(static_cast<size_t>(group_index)).forward(output_memory_group);
                const torch::Tensor enc_outputs_coord =
                    config_.bbox_reparam ? reparam_box_refine(output_proposals, coord_delta) : coord_delta + output_proposals;

                auto proposal_scores = std::get<0>(enc_outputs_class.max(-1));
                const int64_t topk = std::min<int64_t>(config_.num_queries, enc_outputs_class.size(1));
                auto topk_indices = std::get<1>(proposal_scores.topk(topk, 1));
                auto gathered_boxes = enc_outputs_coord.gather(1, topk_indices.unsqueeze(-1).expand({batch, topk, 4}));
                refpoint_embed_ts.push_back(gathered_boxes.detach());
                boxes_ts.push_back(gathered_boxes);
                memory_ts.push_back(output_memory_group.gather(1, topk_indices.unsqueeze(-1).expand({batch, topk, config_.hidden_dim})));
            }

            enc_memory = torch::cat(memory_ts, 1);
            enc_boxes = config_.bbox_reparam ? torch::cat(boxes_ts, 1) : torch::cat(boxes_ts, 1).sigmoid();

            if (config_.dec_layers > 0) {
                decoder_refpoints = refpoint_embed.unsqueeze(0).repeat({batch, 1, 1});
                auto topk_refs = torch::cat(refpoint_embed_ts, 1);
                const int64_t ts_len = topk_refs.size(1);
                auto refpoints_ts_subset = decoder_refpoints.narrow(1, 0, ts_len);
                auto refpoints_subset = decoder_refpoints.narrow(1, ts_len, decoder_refpoints.size(1) - ts_len);
                if (config_.bbox_reparam) {
                    refpoints_ts_subset = reparam_box_refine(topk_refs, refpoints_ts_subset);
                } else {
                    refpoints_ts_subset = refpoints_ts_subset + topk_refs;
                }
                decoder_refpoints = torch::cat({refpoints_ts_subset, refpoints_subset}, 1);
            }
        } else if (config_.dec_layers > 0) {
            decoder_refpoints = refpoint_embed.unsqueeze(0).repeat({batch, 1, 1});
        }

        if (config_.dec_layers > 0) {
            mmltk::common::logging::ScopedProfile profile_rfdetr_model_transformer_decoder{"rfdetr.model.transformer.decoder"};
            auto tgt = query_feat.unsqueeze(0).repeat({batch, 1, 1});
            DecoderQueryLayout query_layout;
            query_layout.ordinary = {training_mode ? std::max<int64_t>(1, config_.group_detr) : 1, config_.num_queries};
            if (denoising != nullptr) {
                if (!training_mode || denoising->content.size(0) != batch ||
                    denoising->layout.ordinary.groups != query_layout.ordinary.groups ||
                    denoising->layout.ordinary.queries_per_group != query_layout.ordinary.queries_per_group) {
                    throw std::runtime_error("DN queries require the matching training decoder layout");
                }
                query_layout = denoising->layout;
                tgt = torch::cat({tgt, denoising->content.to(tgt.dtype())}, 1);
                const auto dn_references = config_.bbox_reparam
                                               ? denoising->normalized_references.to(decoder_refpoints.dtype())
                                               : inverse_sigmoid(denoising->normalized_references).to(decoder_refpoints.dtype());
                decoder_refpoints = torch::cat({decoder_refpoints, dn_references}, 1);
            }
            if (denoising != nullptr) { supervision->begin_denoising_decoder_timing(); }
            try {
                std::tie(hidden_states, refs_unsigmoid) =
                    decoder->forward(tgt, memory, mask_flatten_tensor, lvl_pos_embed_flatten, decoder_refpoints, layout,
                                     valid_ratios_tensor.to(memory.dtype()), query_layout);
            } catch (...) {
                if (denoising != nullptr) { supervision->end_denoising_decoder_timing(); }
                throw;
            }
            if (denoising != nullptr) { supervision->end_denoising_decoder_timing(); }
        }

        return NativeTransformerOutput{
            std::move(hidden_states),
            std::move(refs_unsigmoid),
            std::move(enc_memory),
            std::move(enc_boxes),
        };
    }

   private:
    [[nodiscard]] const FeatureLayout& feature_layout(const std::vector<int64_t>& spatial_shape_values, const torch::Device& device,
                                                      const c10::ScalarType scalar_type) {
        std::lock_guard lock(feature_layout_mutex_);
        if (!feature_layout_) {
            feature_layout_ = std::make_unique<FeatureLayout>(spatial_shape_values, device, scalar_type);
        } else {
            feature_layout_->require_compatible(spatial_shape_values, device, scalar_type);
        }
        return *feature_layout_;
    }

    std::shared_ptr<NativeDecoderImpl> decoder;
    torch::nn::ModuleList enc_output{nullptr};
    torch::nn::ModuleList enc_output_norm{nullptr};
    torch::nn::ModuleList enc_out_class_embed{nullptr};
    torch::nn::ModuleList enc_out_bbox_embed{nullptr};
    NativeRfDetrConfig config_;
    std::mutex feature_layout_mutex_;
    std::unique_ptr<FeatureLayout> feature_layout_;
};

}  // namespace

NativeRfDetrModel::Impl::Impl(const NativeRfDetrConfig& config, ModelClassLayout layout)
    : config_(config.num_queries > 0 ? config : native_config_from_preset(model_presets().front())),
      backbone_(register_module("backbone", torch::nn::ModuleList())),
      class_embed(register_module(std::string(detail::kDecoderClassAxis.module_name()), torch::nn::Linear(config_.hidden_dim, std::max(1, config_.num_classes)))),
      refpoint_embed(
          register_module("refpoint_embed", torch::nn::Embedding(std::max(1, config_.num_queries * std::max(1, config_.group_detr)), 4))),
      query_feat(register_module(
          "query_feat", torch::nn::Embedding(std::max(1, config_.num_queries * std::max(1, config_.group_detr)), config_.hidden_dim))) {
    if (layout.slots.empty()) layout = unresolved_class_layout(static_cast<std::size_t>(config_.num_classes));
    layout_ = std::make_shared<const ResolvedClassLayout>(std::move(layout));
    if (layout_->output_width() != static_cast<std::size_t>(config_.num_classes))
        throw std::invalid_argument("model class layout disagrees with configured output width");
    if (training_supervision_enabled(config_.training_supervision)) layout_->require_execution(true);
    backbone_->push_back(std::make_shared<NativeBackboneImpl>(config_));
    backbone_->push_back(PositionEmbeddingSine(config_.hidden_dim / 2, 10000.0, true, 2.0 * M_PI));

    transformer_ = register_module(std::string(detail::kTransformerModule), std::make_shared<NativeTransformerImpl>(config_));
    bbox_embed_ = register_module("bbox_embed", std::make_shared<RfDetrMlpImpl>(config_.hidden_dim, config_.hidden_dim, 4, 3));
    if (config_.segmentation) {
        segmentation_head_ = register_module(
            "segmentation_head", std::make_shared<SegmentationHeadImpl>(config_.hidden_dim, std::max<int64_t>(1, config_.dec_layers)));
    }

    if (auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_)) {
        if (!config_.lite_refpoint_refine) { transformer->set_decoder_bbox_embed(std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_)); }
    }

    torch::NoGradGuard no_grad;
    refpoint_embed->weight.zero_();

    const double prior_prob = 0.01;
    const auto bias_value = static_cast<float>(-std::log((1.0 - prior_prob) / prior_prob));
    class_embed->bias.fill_(bias_value);

    if (auto bbox = std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_)) {
        auto params = bbox->named_parameters(true);
        if (auto* weight = params.find("layers.2.weight")) { weight->zero_(); }
        if (auto* bias = params.find("layers.2.bias")) { bias->zero_(); }
    }

    if (training_supervision_enabled(config_.training_supervision)) {
        auto generator = at::detail::getDefaultCPUGenerator();
        const auto rng_state = generator.get_state();
        try {
            training_supervision_ = register_module(std::string(detail::kTrainingSupervisionModule), std::make_shared<TrainingSupervisionImpl>(config_, layout_->catalog()->size()));
        } catch (...) {
            generator.set_state(rng_state);
            throw;
        }
        generator.set_state(rng_state);
    }
}

std::vector<detail::ClassTensorAxis> NativeRfDetrModel::Impl::class_axes() const {
    std::vector<detail::ClassTensorAxis> axes;
    append_classifier_axes(axes, *class_embed, detail::kDecoderClassAxis);
    std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_)->append_class_axes(axes);
    if (training_supervision_) training_supervision_->append_class_axes(axes);
    return axes;
}

ModelOutputs NativeRfDetrModel::Impl::forward(const NestedTensor& batch, bool include_masks) {
    return forward_impl(batch, include_masks, false);
}

ModelOutputs NativeRfDetrModel::Impl::forward_for_match_free(const NestedTensor& batch) {
    if (!training_supervision_ || !training_supervision_->match_free_enabled() || !training_supervision_->initialized()) {
        throw std::runtime_error("Match-Free forward requires active initialized supervision");
    }
    return forward_impl(batch, false, true);
}

ModelOutputs NativeRfDetrModel::Impl::forward_with_denoising(const NestedTensor& batch, const PreparedTargets& targets,
                                                             const TrainingStepIdentity& identity) {
    if (!is_training() || !training_supervision_ || !training_supervision_->denoising_enabled() || !training_supervision_->initialized()) {
        throw std::runtime_error("DN forward requires active initialized training supervision");
    }
    if (is_compiled_train_) { throw std::runtime_error("full-trace RF-DETR cannot consume target-dependent DN queries"); }
    auto denoising = training_supervision_->prepare_denoising(targets, identity, batch.tensors.device(), query_feat->weight.scalar_type());
    return forward_impl(batch, config_.segmentation, training_supervision_->match_free_enabled(), denoising ? &*denoising : nullptr);
}

void NativeRfDetrModel::Impl::initialize_training_supervision(const std::uint64_t request_seed) {
    if (training_supervision_) { training_supervision_->initialize(request_seed); }
}

detail::NativeModelTechnicalOwner::TrainingSupervisionRuntimeState NativeRfDetrModel::Impl::export_training_supervision_runtime() const {
    return {
        config_.training_supervision,
        static_cast<bool>(training_supervision_),
        training_supervision_ && training_supervision_->initialized(),
    };
}

void NativeRfDetrModel::Impl::import_training_supervision_runtime(const TrainingSupervisionRuntimeState& state) {
    if (state.config != config_.training_supervision || state.has_owner != static_cast<bool>(training_supervision_)) {
        throw std::runtime_error("RF-DETR model replicas have incompatible training supervision topology");
    }
    if (!training_supervision_) {
        if (state.initialized) { throw std::runtime_error("inactive RF-DETR supervision replica cannot import active runtime state"); }
        return;
    }
    if (!state.initialized) { throw std::runtime_error("RF-DETR training supervision replica source is not initialized"); }
    // Parameters and buffers are copied by the caller first. This operation
    // installs only the one-shot runtime fact and deliberately draws no RNG.
    training_supervision_->install_replicated_initialized_runtime(state.config);
}

TrainingLoss NativeRfDetrModel::Impl::supervision_loss(const ModelOutputs& outputs, const PreparedTargets& targets,
                                                       const DeviceLossNormalizer& normalizer, const bool training_mode) {
    if (!training_supervision_) { throw std::runtime_error("RF-DETR supervision loss requires an active supervision owner"); }
    return training_supervision_->loss(outputs, targets, normalizer, training_mode);
}

void NativeRfDetrModel::Impl::configure_supervision_timing(const SupervisionTimingSetup& setup) {
    if (training_supervision_) { training_supervision_->configure_timing(setup); }
}

void NativeRfDetrModel::Impl::begin_supervised_step_timing() {
    if (training_supervision_) training_supervision_->begin_supervised_step_timing();
}

void NativeRfDetrModel::Impl::end_supervised_step_timing() {
    if (training_supervision_) training_supervision_->end_supervised_step_timing();
}

void NativeRfDetrModel::Impl::begin_criterion_timing() {
    if (training_supervision_) training_supervision_->begin_criterion_timing();
}

void NativeRfDetrModel::Impl::end_criterion_timing() {
    if (training_supervision_) training_supervision_->end_criterion_timing();
}

SupervisionTimingHandoff NativeRfDetrModel::Impl::harvest_supervision_timing() {
    return training_supervision_ ? training_supervision_->harvest_timing() : SupervisionTimingHandoff{};
}

ModelOutputs NativeRfDetrModel::Impl::forward_impl(const NestedTensor& batch, const bool include_masks,
                                                   const bool capture_match_free_features, const DenoisingQueryBatch* denoising) {
    const bool is_train = is_training();
    if (capture_match_free_features && ((is_train && is_compiled_train_) || (!is_train && is_compiled_eval_))) {
        throw std::runtime_error("full-trace RF-DETR cannot capture Match-Free query features");
    }
    if ((is_train && is_compiled_train_) || (!is_train && is_compiled_eval_)) {
        std::vector<torch::jit::IValue> inputs;
        inputs.emplace_back(batch.tensors);
        if (batch.mask.defined()) {
            inputs.emplace_back(batch.mask);
        } else {
            inputs.emplace_back(torch::zeros({batch.tensors.size(0), batch.tensors.size(2), batch.tensors.size(3)},
                                             torch::TensorOptions().dtype(torch::kBool).device(batch.tensors.device())));
        }

        auto& target_model = is_train ? traced_model_train_ : traced_model_eval_;
        auto out_dict = target_model.forward(inputs).toGenericDict();
        ModelOutputs outputs;

        outputs.main.pred_logits = out_dict.at("pred_logits").toTensor();
        outputs.main.pred_boxes = out_dict.at("pred_boxes").toTensor();
        if (include_masks && out_dict.contains("pred_masks")) { outputs.main.pred_masks = out_dict.at("pred_masks").toTensor(); }
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
            if (include_masks && out_dict.contains("enc_masks")) { enc_out.pred_masks = out_dict.at("enc_masks").toTensor(); }
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
            if (!out_dict.contains(key_logits)) { break; }
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

    mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward{"rfdetr.model.forward"};
    if (!batch.tensors.defined()) { throw std::runtime_error("NativeRfDetrModel::forward requires batch.tensors"); }

    NestedTensor samples = batch;
    if (!samples.mask.defined()) {
        samples.mask = torch::zeros({samples.tensors.size(0), samples.tensors.size(2), samples.tensors.size(3)},
                                    torch::TensorOptions().dtype(torch::kBool).device(samples.tensors.device()));
    }

    std::vector<NestedTensor> features;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_backbone{"rfdetr.model.forward.backbone"};
        const bool use_traced_backbone = is_train ? has_traced_backbone_train_ : has_traced_backbone_eval_;
        if (use_traced_backbone) {
            auto& traced = is_train ? traced_backbone_train_ : traced_backbone_eval_;
            auto feature = traced.forward({samples.tensors}).toTensor();
            torch::Tensor mask = resize_mask_to_feature(samples.mask, feature);
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
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_position_embeddings{"rfdetr.model.forward.position_embeddings"};
        for (const auto& feature : features) {
            srcs.push_back(feature.tensors);
            masks.push_back(feature.mask);
            poss.push_back(backbone_->at<PositionEmbeddingSineImpl>(1).forward_full_valid(
                feature.tensors.size(0), feature.tensors.size(2), feature.tensors.size(3), feature.tensors.device()));
        }
    }

    const int64_t active_query_count = is_training() ? config_.num_queries * std::max(1, config_.group_detr) : config_.num_queries;
    const auto ref_weights = refpoint_embed->weight.narrow(0, 0, std::max<int64_t>(1, active_query_count));
    const auto query_weights = query_feat->weight.narrow(0, 0, std::max<int64_t>(1, active_query_count));

    auto transformer = std::dynamic_pointer_cast<NativeTransformerImpl>(transformer_);
    auto bbox = std::dynamic_pointer_cast<RfDetrMlpImpl>(bbox_embed_);
    if (!transformer || !bbox) { throw std::runtime_error("RF-DETR native modules are not initialized"); }

    NativeTransformerOutput transformed;
    {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_transformer{"rfdetr.model.forward.transformer"};
        transformed =
            transformer->forward(srcs, masks, poss, ref_weights, query_weights, is_training(), denoising, training_supervision_.get());
    }

    std::vector<torch::Tensor> decoder_query_features;
    std::vector<torch::Tensor> denoising_query_features;
    if (transformed.hidden_states.defined()) {
        auto states = transformed.hidden_states.unbind(0);
        decoder_query_features.reserve(states.size());
        denoising_query_features.reserve(states.size());
        for (const auto& state : states) {
            decoder_query_features.push_back(state.narrow(1, 0, active_query_count));
            if (denoising != nullptr) {
                denoising_query_features.push_back(state.narrow(1, active_query_count, denoising->layout.denoising_queries()));
            }
        }
    }

    std::vector<torch::Tensor> dense_masks;
    std::vector<OutputLayer::SparsePredMasks> sparse_masks;
    auto* segmentation_head = dynamic_cast<SegmentationHeadImpl*>(segmentation_head_.get());
    if (include_masks && segmentation_head_ && !decoder_query_features.empty()) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_segmentation{"rfdetr.model.forward.segmentation"};
        if (!segmentation_head) { throw std::runtime_error("RF-DETR segmentation head is not initialized"); }
        if (is_training()) {
            sparse_masks = segmentation_head->sparse_forward(features.front().tensors, decoder_query_features,
                                                             {samples.tensors.size(2), samples.tensors.size(3)});
        } else {
            dense_masks = segmentation_head->forward(features.front().tensors, decoder_query_features,
                                                     {samples.tensors.size(2), samples.tensors.size(3)});
        }
    }

    ModelOutputs outputs;
    if (denoising != nullptr) {
        DenoisingOutputs dn_outputs;
        dn_outputs.original_labels = denoising->original_labels;
        dn_outputs.original_boxes = denoising->original_boxes;
        dn_outputs.valid_slots = denoising->valid_slots;
        dn_outputs.target_indices = denoising->target_indices;
        dn_outputs.groups = denoising->layout.denoising_groups;
        dn_outputs.queries_per_group = denoising->layout.denoising_queries_per_group;
        outputs.denoising = std::move(dn_outputs);
    }
    if (!decoder_query_features.empty()) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_decoder_heads{"rfdetr.model.forward.decoder_heads"};
        const int64_t ref_layers = transformed.refs_unsigmoid.size(0);
        for (size_t index = 0; index < decoder_query_features.size(); ++index) {
            const int64_t ref_index = ref_layers == 1 ? 0 : static_cast<int64_t>(index);
            const auto& hs = decoder_query_features[index];
            auto all_refs = transformed.refs_unsigmoid.index({ref_index});
            auto refs = all_refs.narrow(1, 0, active_query_count);

            torch::Tensor boxes;
            if (config_.bbox_reparam) {
                boxes = reparam_box_refine(refs, bbox->forward(hs));
            } else {
                boxes = (bbox->forward(hs) + refs).sigmoid();
            }

            OutputLayer layer;
            layer.pred_logits = class_embed->forward(hs);
            layer.pred_boxes = boxes;
            const bool final_decoder_layer = index + 1 == decoder_query_features.size();
            if (capture_match_free_features && (final_decoder_layer || config_.aux_loss)) {
                const int64_t groups = is_training() ? std::max<int64_t>(1, config_.group_detr) : 1;
                const int64_t queries_per_group = hs.size(1) / groups;
                layer.query_features = hs.view({hs.size(0), groups, queries_per_group, hs.size(2)});
                layer.query_layout = SupervisedQueryLayout{groups, queries_per_group};
            }
            if (include_masks && segmentation_head_) {
                if (is_training()) {
                    layer.sparse_pred_masks = sparse_masks[index];
                } else {
                    layer.pred_masks = dense_masks[index];
                }
            }

            if (final_decoder_layer) {
                outputs.main = std::move(layer);
            } else {
                outputs.aux_outputs.push_back(std::move(layer));
            }

            if (outputs.denoising) {
                const auto& dn_hs = denoising_query_features[index];
                const auto dn_refs = all_refs.narrow(1, active_query_count, denoising->layout.denoising_queries());
                auto dn_boxes =
                    config_.bbox_reparam ? reparam_box_refine(dn_refs, bbox->forward(dn_hs)) : (bbox->forward(dn_hs) + dn_refs).sigmoid();
                DenoisingOutputLayer dn_layer;
                dn_layer.pred_logits =
                    class_embed->forward(dn_hs).view({dn_hs.size(0), denoising->layout.denoising_groups,
                                                      denoising->layout.denoising_queries_per_group, config_.num_classes});
                dn_layer.pred_boxes =
                    dn_boxes.view({dn_hs.size(0), denoising->layout.denoising_groups, denoising->layout.denoising_queries_per_group, 4});
                if (final_decoder_layer) {
                    outputs.denoising->main = std::move(dn_layer);
                } else {
                    outputs.denoising->aux_outputs.push_back(std::move(dn_layer));
                }
            }
        }
    }

    if (config_.two_stage && transformed.enc_memory.defined() && transformed.enc_boxes.defined()) {
        mmltk::common::logging::ScopedProfile profile_rfdetr_model_forward_encoder_heads{"rfdetr.model.forward.encoder_heads"};
        OutputLayer enc_output;
        enc_output.pred_logits = transformer->encoder_class_logits(transformed.enc_memory, is_training());
        enc_output.pred_boxes = transformed.enc_boxes;
        if (capture_match_free_features && config_.aux_loss) {
            const int64_t groups = is_training() ? std::max<int64_t>(1, config_.group_detr) : 1;
            const int64_t queries_per_group = transformed.enc_memory.size(1) / groups;
            enc_output.query_features =
                transformed.enc_memory.view({transformed.enc_memory.size(0), groups, queries_per_group, transformed.enc_memory.size(2)});
            enc_output.query_layout = SupervisedQueryLayout{groups, queries_per_group};
        }
        if (include_masks && segmentation_head_) {
            if (!segmentation_head) { throw std::runtime_error("RF-DETR segmentation head is not initialized"); }
            if (is_training()) {
                enc_output.sparse_pred_masks = segmentation_head->sparse_forward(
                    features.front().tensors, {transformed.enc_memory}, {samples.tensors.size(2), samples.tensors.size(3)}, true)[0];
            } else {
                enc_output.pred_masks = segmentation_head->forward(features.front().tensors, {transformed.enc_memory},
                                                                   {samples.tensors.size(2), samples.tensors.size(3)}, true)[0];
            }
        }
        outputs.enc_outputs = std::move(enc_output);
    }

    return outputs;
}

ModelStateLoadSummary NativeRfDetrModel::Impl::load_normalized_state(const std::vector<NormalizedModelStateEntry>& state,
                                                                     const bool strict, const ModelClassLayout* admitted_layout) {
    auto next_layout = admitted_layout ? std::make_shared<const ResolvedClassLayout>(*admitted_layout) : layout_;
    if (next_layout->output_width() != static_cast<std::size_t>(config_.num_classes))
        throw std::invalid_argument("checkpoint layout disagrees with model output width");
    if (training_supervision_ && next_layout->record() != layout_->record())
        throw std::invalid_argument("active supervision checkpoint requires the exact admitted layout");
    auto candidate = stage_normalized_state(state, strict ? detail::NormalizedModelStateAdmission::Exact :
        detail::NormalizedModelStateAdmission::PartialExact);
    auto summary = candidate.summary;
    commit_normalized_state(std::move(candidate));
    layout_ = std::move(next_layout);
    return summary;
}

detail::NormalizedModelStateCandidate NativeRfDetrModel::Impl::stage_normalized_state(
    const std::vector<NormalizedModelStateEntry>& state, const detail::NormalizedModelStateAdmission admission,
    const ResolvedClassLayout* source_layout) {
    auto parameters = named_parameters(true);
    auto buffers = named_buffers(true);
    const bool transfer = admission == detail::NormalizedModelStateAdmission::FreshTransfer ||
        admission == detail::NormalizedModelStateAdmission::PartialFreshTransfer;
    if (transfer && !source_layout) throw std::invalid_argument("fresh transfer requires the admitted source layout");
    if (transfer) layout_->require_execution(true);
    const auto axes = class_axes();
    std::unordered_map<std::string_view, const detail::ClassTensorAxis*> axis_by_name;
    for (const auto& axis : axes) axis_by_name.emplace(axis.name, &axis);
    std::vector<std::pair<std::int64_t, std::int64_t>> foreground_matches, output_matches;
    if (transfer && source_layout->semantic()) {
        std::vector<std::int64_t> source_slots(source_layout->catalog()->size());
        for (std::size_t slot = 0; slot < source_layout->record().slots.size(); ++slot) {
            const auto& role = source_layout->record().slots[slot];
            if (role.foreground_index) source_slots[*role.foreground_index] = static_cast<std::int64_t>(slot);
        }
        for (std::size_t destination_slot = 0; destination_slot < layout_->record().slots.size(); ++destination_slot) {
            const auto& role = layout_->record().slots[destination_slot];
            if (!role.foreground_index) continue;
            const auto destination_index = *role.foreground_index;
            const auto source_index = source_layout->catalog()->resolve(layout_->catalog()->names()[destination_index]);
            if (!source_index) continue;
            foreground_matches.emplace_back(*source_index, destination_index);
            output_matches.emplace_back(source_slots[*source_index], destination_slot);
        }
    }
    detail::NormalizedModelStateCandidate candidate;
    candidate.destinations.reserve(state.size());
    candidate.tensors.reserve(state.size());
    std::unordered_set<std::string> supplied;
    supplied.reserve(state.size());
    for (const auto& entry : state) {
        if (!supplied.insert(entry.name).second) {
            throw std::runtime_error("normalized RF-DETR model state contains duplicate name: " + entry.name);
        }
        torch::Tensor* destination = parameters.find(entry.name);
        if (destination == nullptr) { destination = buffers.find(entry.name); }
        if (destination == nullptr) {
            candidate.summary.unexpected_names.push_back(entry.name);
            continue;
        }
        torch::Tensor source = entry.tensor;
        if (transfer) {
            if (const auto found = axis_by_name.find(entry.name); found != axis_by_name.end()) {
                const auto& axis = *found->second;
                const bool classifier = axis.coordinates == detail::ClassTensorCoordinates::OutputSlots;
                const bool boxes = axis.coordinates == detail::ClassTensorCoordinates::ForegroundWithBoxes;
                const bool known_axis = classifier || source_layout->record().supervision_in_foreground_order;
                bool compatible = source.defined() && source.dim() == destination->dim();
                for (std::int64_t dimension = 0; compatible && dimension < source.dim(); ++dimension)
                    if (dimension != axis.dimension && source.size(dimension) != destination->size(dimension)) compatible = false;
                const auto source_classes = classifier ? source_layout->output_width() : source_layout->catalog()->size();
                if (compatible && known_axis && source.size(axis.dimension) != static_cast<std::int64_t>(source_classes + (boxes ? 4 : 0)))
                    throw std::invalid_argument("class-dependent tensor disagrees with admitted layout: " + entry.name);
                if (compatible) {
                    auto adapted = destination->detach().clone();
                    if (known_axis && source_layout->semantic()) {
                        const auto converted = source.to(destination->device(), destination->scalar_type());
                        for (const auto& [from, to] : classifier ? output_matches : foreground_matches)
                            adapted.select(axis.dimension, to).copy_(converted.select(axis.dimension, from));
                        if (boxes) adapted.narrow(axis.dimension, layout_->catalog()->size(), 4).copy_(
                            converted.narrow(axis.dimension, source_layout->catalog()->size(), 4));
                    }
                    source = std::move(adapted);
                }
            }
        }
        if (!source.defined() || source.sizes() != destination->sizes()) {
            if (axis_by_name.contains(entry.name))
                throw std::invalid_argument("class-dependent tensor disagrees with model topology: " + entry.name);
            candidate.summary.incompatible_names.push_back(entry.name);
            continue;
        }
        candidate.destinations.push_back(*destination);
        candidate.tensors.push_back(source.to(destination->device(), destination->scalar_type()));
        candidate.summary.loaded_names.push_back(entry.name);
    }
    for (const auto& parameter : parameters) {
        if (!supplied.contains(parameter.key())) { candidate.summary.missing_names.push_back(parameter.key()); }
    }
    for (const auto& buffer : buffers) {
        if (!supplied.contains(buffer.key())) { candidate.summary.missing_names.push_back(buffer.key()); }
    }
    std::ranges::sort(candidate.summary.loaded_names);
    std::ranges::sort(candidate.summary.missing_names);
    std::ranges::sort(candidate.summary.unexpected_names);
    std::ranges::sort(candidate.summary.incompatible_names);
    const bool missing_required = std::ranges::any_of(candidate.summary.missing_names, [&](const auto& name) {
        return !transfer || !name.starts_with("training_supervision.");
    });
    if (admission != detail::NormalizedModelStateAdmission::PartialFreshTransfer && admission != detail::NormalizedModelStateAdmission::PartialExact &&
        (missing_required || !candidate.summary.unexpected_names.empty() || !candidate.summary.incompatible_names.empty())) {
        throw std::runtime_error("normalized RF-DETR model state does not match the model");
    }
    return candidate;
}

void NativeRfDetrModel::Impl::commit_normalized_state(detail::NormalizedModelStateCandidate candidate) {
    torch::NoGradGuard no_grad;
    std::vector<torch::Tensor> rollback;
    rollback.reserve(candidate.destinations.size());
    for (const auto& destination : candidate.destinations) {
        rollback.push_back(destination.detach().clone());
    }
    std::size_t committed = 0;
    try {
        for (; committed < candidate.destinations.size(); ++committed) {
            candidate.destinations[committed].copy_(candidate.tensors[committed]);
        }
    } catch (...) {
        const std::size_t restore_count = std::min(committed + 1, candidate.destinations.size());
        for (std::size_t index = 0; index < restore_count; ++index) {
            candidate.destinations[index].copy_(rollback[index]);
        }
        throw;
    }
}

namespace {

// Tracing and ONNX export both drive the model with a zero batch shaped like real input and bound to
// the model's own parameter device and dtype; that probe input has one definition.
torch::Tensor make_dummy_pixel_values(const torch::nn::Module& model, const int64_t resolution, const int64_t batch_size) {
    const torch::Tensor reference = model.parameters().front();
    return torch::zeros({batch_size, 3, resolution, resolution},
                        torch::TensorOptions().dtype(reference.dtype()).device(reference.device()));
}

}  // namespace

void NativeRfDetrModel::Impl::optimize_for_inference(int batch_size, bool for_training, CompilationMode mode) {
    const bool already_compiled =
        for_training ? (is_compiled_train_ || has_traced_backbone_train_) : (is_compiled_eval_ || has_traced_backbone_eval_);
    if (already_compiled) return;

    if (for_training) {
        this->train();
    } else {
        this->eval();
    }

    if (mode == CompilationMode::kNone) {
        mmltk::common::logging::info([&](auto& logger) {
            logger.info("rfdetr: compilation disabled, using raw C++ forward for {}", for_training ? "training" : "evaluation");
        });
        return;
    }

    auto device = this->parameters().front().device();
    auto dummy_pixel_values = make_dummy_pixel_values(*this, config_.resolution, batch_size);

    if (mode == CompilationMode::kSelective) {
        mmltk::common::logging::info([&](auto& logger) {
            logger.info("rfdetr: selectively compiling backbone for {} via torch::jit...", for_training ? "training" : "evaluation");
        });

        auto& backbone = backbone_->at<NativeBackboneImpl>(0);

        auto cu = std::make_shared<torch::jit::CompilationUnit>();
        auto cls_name = for_training ? "__torch__.NativeRfDetrBackboneTrain" : "__torch__.NativeRfDetrBackboneEval";
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
            [&](torch::jit::Stack args) -> torch::jit::Stack { return {backbone.forward_features(args[0].toTensor())}; },
            [](const torch::autograd::Variable&) { return ""; }, false, false, target);
        target->type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));

        if (for_training) {
            has_traced_backbone_train_ = true;
        } else {
            has_traced_backbone_eval_ = true;
        }
        return;
    }

    mmltk::common::logging::info([&](auto& logger) {
        logger.info("rfdetr: compiling entire model {} graph via torch::jit...", for_training ? "training" : "evaluation");
    });

    auto dummy_mask =
        torch::zeros({batch_size, config_.resolution, config_.resolution}, torch::TensorOptions().dtype(torch::kBool).device(device));

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
        trace_res = torch::jit::tracer::trace(
            {dummy_pixel_values, dummy_mask},
            [&](torch::jit::Stack args) -> torch::jit::Stack {
                auto outputs = this->forward(NestedTensor{args[0].toTensor(), args[1].toTensor()});
                c10::Dict<std::string, torch::Tensor> dict;
                dict.insert("pred_logits", outputs.main.pred_logits);
                dict.insert("pred_boxes", outputs.main.pred_boxes);
                if (outputs.main.pred_masks) { dict.insert("pred_masks", *outputs.main.pred_masks); }
                if (outputs.main.sparse_pred_masks) {
                    dict.insert("sparse_spatial", outputs.main.sparse_pred_masks->spatial_features);
                    dict.insert("sparse_query", outputs.main.sparse_pred_masks->query_features);
                    dict.insert("sparse_bias", outputs.main.sparse_pred_masks->bias);
                }
                if (outputs.enc_outputs) {
                    dict.insert("enc_logits", outputs.enc_outputs->pred_logits);
                    dict.insert("enc_boxes", outputs.enc_outputs->pred_boxes);
                    if (outputs.enc_outputs->pred_masks) { dict.insert("enc_masks", *outputs.enc_outputs->pred_masks); }
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
                torch::jit::Stack result;
                result.emplace_back(std::move(dict));
                return result;
            },
            [](const torch::autograd::Variable&) { return ""; }, false, false, target_traced_model);
    }
    target_traced_model->type()->addMethod(cu->create_function("forward", trace_res.first->graph, true));
    if (for_training) {
        is_compiled_train_ = true;
    } else {
        is_compiled_eval_ = true;
    }
}

void NativeRfDetrModel::Impl::set_force_pytorch_deformable_attn(bool value) {
    for (auto& module : this->modules(false)) {
        if (auto* deform = dynamic_cast<MSDeformAttnImpl*>(module.get())) { deform->force_pytorch_deformable_attn_ = value; }
    }
}

NativeRfDetrModel::NativeRfDetrModel(const NativeRfDetrConfig& config, ModelClassLayout layout) : impl_(std::make_unique<Impl>(config, std::move(layout))) {}
const std::shared_ptr<const ResolvedClassLayout>& NativeRfDetrModel::class_layout() const noexcept { return impl_->layout_; }

NativeRfDetrModel::~NativeRfDetrModel() = default;
NativeRfDetrModel::NativeRfDetrModel(NativeRfDetrModel&&) noexcept = default;
NativeRfDetrModel& NativeRfDetrModel::operator=(NativeRfDetrModel&&) noexcept = default;

const NativeRfDetrConfig& NativeRfDetrModel::config() const noexcept { return impl_->config(); }

bool NativeRfDetrModel::is_compiled(const bool for_training) const noexcept { return impl_->is_compiled(for_training); }

void NativeRfDetrModel::optimize_for_inference(const std::int32_t batch_size, const bool for_training, const CompilationMode mode) {
    impl_->optimize_for_inference(batch_size, for_training, mode);
}

void* NativeRfDetrModel::technical_handle() noexcept { return static_cast<detail::NativeModelTechnicalOwner*>(impl_.get()); }

const void* NativeRfDetrModel::technical_handle() const noexcept {
    return static_cast<const detail::NativeModelTechnicalOwner*>(impl_.get());
}

}  // namespace mmltk::backend::models::rfdetr
