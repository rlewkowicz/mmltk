#pragma once
#include <torch/torch.h>
#include <memory>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/core/model.h"
namespace mmltk::backend::models::rfdetr {
torch::Tensor inverse_sigmoid(const torch::Tensor& x, double eps = 1e-5);
class MlpImpl : public torch::nn::Module {
   public:
    MlpImpl(int64_t input_dim, int64_t hidden_dim, int64_t output_dim, int64_t num_layers);
    torch::Tensor forward(torch::Tensor x);

   private:
    int64_t num_layers_ = 0;
    torch::nn::ModuleList layers{nullptr};
};
TORCH_MODULE(Mlp);
using RfDetrMlpImpl = MlpImpl;
using RfDetrMlp = Mlp;
class DetectionHeadImpl : public torch::nn::Module {
   public:
    DetectionHeadImpl(int64_t hidden_dim, int64_t num_classes);
    std::pair<torch::Tensor, torch::Tensor> forward(const torch::Tensor& hs);
    torch::nn::Linear class_embed{nullptr};
    Mlp bbox_embed{nullptr};
};
TORCH_MODULE(DetectionHead);
class PositionEmbeddingSineImpl : public torch::nn::Module {
   public:
    PositionEmbeddingSineImpl(int64_t num_pos_feats = 64, double temperature = 10000.0, bool normalize = false, c10::optional<double> scale = c10::nullopt);
    ~PositionEmbeddingSineImpl() override;
    torch::Tensor forward(const NestedTensor& tensor_list, bool align_dim_orders = false) const;
    torch::Tensor forward_mask(const torch::Tensor& mask, bool align_dim_orders = false) const;
    torch::Tensor forward_full_valid(int64_t batch_size, int64_t height, int64_t width, const torch::Device& device, bool align_dim_orders = false) const;

   private:
    struct FullValidCache;
    torch::Tensor build_full_valid_base(int64_t height, int64_t width, const torch::Device& device, bool align_dim_orders) const;
    int64_t num_pos_feats_ = 64;
    double temperature_ = 10000.0;
    bool normalize_ = false;
    double scale_ = 0.0;
    mutable std::unique_ptr<FullValidCache> full_valid_cache_;
};
TORCH_MODULE(PositionEmbeddingSine);
class DepthwiseConvBlockImpl : public torch::nn::Module {
   public:
    explicit DepthwiseConvBlockImpl(int64_t dim, double layer_scale_init_value = 0.0);
    torch::Tensor forward(torch::Tensor x);
    torch::nn::Conv2d dwconv{nullptr};
    torch::nn::LayerNorm norm{nullptr};
    torch::nn::Linear pwconv1{nullptr};
    torch::nn::GELU act{nullptr};
    torch::Tensor gamma;
};
TORCH_MODULE(DepthwiseConvBlock);
class MlpBlockImpl : public torch::nn::Module {
   public:
    explicit MlpBlockImpl(int64_t dim, double layer_scale_init_value = 0.0);
    torch::Tensor forward(torch::Tensor x);
    torch::nn::LayerNorm norm_in{nullptr};
    torch::nn::ModuleList layers{nullptr};
    torch::Tensor gamma;
};
TORCH_MODULE(MlpBlock);
using SegmentationMlpBlockImpl = MlpBlockImpl;
using SegmentationMlpBlock = MlpBlock;
class SegmentationHeadImpl : public torch::nn::Module {
   public:
    SegmentationHeadImpl(int64_t in_dim, int64_t num_blocks, c10::optional<int64_t> bottleneck_ratio = 1, int64_t downsample_ratio = 4);
    std::vector<torch::Tensor> forward(const torch::Tensor& spatial_features, const std::vector<torch::Tensor>& query_features,
                                       std::pair<int64_t, int64_t> image_size, bool skip_blocks = false);
    std::vector<OutputLayer::SparsePredMasks> sparse_forward(const torch::Tensor& spatial_features, const std::vector<torch::Tensor>& query_features,
                                                             std::pair<int64_t, int64_t> image_size, bool skip_blocks = false);
    torch::nn::ModuleList blocks{nullptr};
    torch::nn::Conv2d spatial_features_proj{nullptr};
    MlpBlock query_features_block{nullptr};
    torch::nn::Linear query_features_proj{nullptr};
    torch::Tensor bias;

   private:
    // Shared driver for forward and sparse_forward: walks the depthwise blocks (or the skip path) and builds one
    // output per query feature tensor via make_output(projected_spatial, projected_queries).
    template <typename Output, typename MakeOutput>
    std::vector<Output> collect_head_outputs(const torch::Tensor& spatial_features, const std::vector<torch::Tensor>& query_features,
                                             std::pair<int64_t, int64_t> image_size, bool skip_blocks, MakeOutput&& make_output);
    torch::Tensor project_spatial_features(const torch::Tensor& spatial_features);
    torch::Tensor project_query_features(const torch::Tensor& query_features);
    torch::Tensor resize_spatial_features(const torch::Tensor& spatial_features, std::pair<int64_t, int64_t> image_size) const;
    int64_t downsample_ratio_ = 4;
    int64_t interaction_dim_ = 0;
    bool use_spatial_identity_ = false;
    bool use_query_identity_ = false;
};
TORCH_MODULE(SegmentationHead);
}  // namespace mmltk::backend::models::rfdetr
