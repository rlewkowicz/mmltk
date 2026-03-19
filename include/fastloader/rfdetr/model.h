#pragma once

#include "fastloader/rfdetr/detection_types.h"
#include "fastloader/rfdetr/model_config.h"

#include <torch/torch.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace fastloader::rfdetr {

class SegmentationHeadImpl;
struct StateDictLoadSummary;

struct NestedTensor {
    torch::Tensor tensors;
    torch::Tensor mask;

    NestedTensor to(const torch::Device& device) const {
        NestedTensor result;
        result.tensors = tensors.to(device);
        if (mask.defined()) {
            result.mask = mask.to(device);
        }
        return result;
    }

    NestedTensor pin_memory() const {
        NestedTensor result;
        result.tensors = tensors.pin_memory();
        if (mask.defined()) {
            result.mask = mask.pin_memory();
        }
        return result;
    }

    std::pair<torch::Tensor, torch::Tensor> decompose() const {
        return {tensors, mask};
    }
};

inline NestedTensor nested_tensor_from_tensor_list(const std::vector<torch::Tensor>& tensor_list) {
    if (tensor_list.empty()) {
        throw std::runtime_error("nested_tensor_from_tensor_list requires at least one tensor");
    }
    if (tensor_list.front().dim() != 3) {
        throw std::runtime_error("nested_tensor_from_tensor_list only supports CHW tensors");
    }

    std::vector<int64_t> max_size(tensor_list.front().sizes().begin(), tensor_list.front().sizes().end());
    for (size_t index = 1; index < tensor_list.size(); ++index) {
        const auto& tensor = tensor_list[index];
        if (tensor.dim() != 3) {
            throw std::runtime_error("nested_tensor_from_tensor_list only supports CHW tensors");
        }
        for (int64_t dim = 0; dim < tensor.dim(); ++dim) {
            max_size[static_cast<size_t>(dim)] =
                std::max(max_size[static_cast<size_t>(dim)], tensor.size(dim));
        }
    }

    const int64_t batch = static_cast<int64_t>(tensor_list.size());
    const auto options = tensor_list.front().options();
    auto padded = torch::zeros(
        {batch, max_size[0], max_size[1], max_size[2]},
        options);
    auto mask = torch::ones(
        {batch, max_size[1], max_size[2]},
        torch::TensorOptions().dtype(torch::kBool).device(options.device()));

    for (int64_t index = 0; index < batch; ++index) {
        const auto& tensor = tensor_list[static_cast<size_t>(index)];
        padded[index]
            .slice(0, 0, tensor.size(0))
            .slice(1, 0, tensor.size(1))
            .slice(2, 0, tensor.size(2))
            .copy_(tensor);
        mask[index]
            .slice(0, 0, tensor.size(1))
            .slice(1, 0, tensor.size(2))
            .fill_(false);
    }

    return NestedTensor{std::move(padded), std::move(mask)};
}

struct NativeRfDetrConfig {
    std::string preset_name;
    int resolution = 0;
    bool segmentation = false;
    int num_classes = 0;
    int num_queries = 0;
    int num_select = 0;
    int dec_layers = 0;
    int group_detr = 1;
    bool two_stage = true;
    int hidden_dim = 256;
    int patch_size = 16;
    int num_windows = 2;
    int positional_encoding_size = 24;
    int sa_nheads = 8;
    int ca_nheads = 16;
    int dec_n_points = 2;
    int dim_feedforward = 2048;
    bool bbox_reparam = true;
    bool lite_refpoint_refine = true;
    double cls_loss_coef = 1.0;
    double bbox_loss_coef = 5.0;
    double giou_loss_coef = 2.0;
    double mask_ce_loss_coef = 1.0;
    double mask_dice_loss_coef = 1.0;
    bool sum_group_losses = false;
    bool use_varifocal_loss = false;
    bool use_position_supervised_loss = false;
    bool ia_bce_loss = true;
    bool aux_loss = true;
    int64_t mask_point_sample_ratio = 16;
    double focal_alpha = 0.25;
    double set_cost_class = 2.0;
    double set_cost_bbox = 5.0;
    double set_cost_giou = 2.0;
};

struct ModelArtifactRequest {
    std::filesystem::path weights_path;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
};

struct ResolvedModelArtifacts {
    NativeRfDetrConfig config;
    std::string input_kind;
    std::filesystem::path input_path;
    std::filesystem::path weights_path;
    std::filesystem::path artifact_root;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
};

ResolvedModelArtifacts resolve_model_artifacts(const ModelArtifactRequest& request);
ResolvedModelArtifacts resolve_upstream_weight_artifacts(const std::filesystem::path& weights_path);

class NativeRfDetrModel : public torch::nn::Module {
public:
    explicit NativeRfDetrModel(const NativeRfDetrConfig& config = {});

    ModelOutputs forward(const NestedTensor& batch, SegmentationHeadImpl* seg_override = nullptr);
    StateDictLoadSummary load_weights(const std::filesystem::path& weights_path, bool strict = false);

    std::shared_ptr<SegmentationHeadImpl> clone_segmentation_head() const;
    SegmentationHeadImpl* segmentation_head_ptr() const;

    void optimize_for_inference(int batch_size = 1, bool for_training = false,
                                CompilationMode mode = CompilationMode::kSelective);
    bool is_compiled(bool for_training) const { return for_training ? is_compiled_train_ : is_compiled_eval_; }

    const NativeRfDetrConfig& config() const { return config_; }
    const ResolvedModelArtifacts& artifacts() const { return artifacts_; }

private:
    void reinitialize_detection_head(int64_t output_classes);

    NativeRfDetrConfig config_;
    ResolvedModelArtifacts artifacts_;
    torch::nn::ModuleList backbone_{nullptr};
    std::shared_ptr<torch::nn::Module> transformer_;
    torch::nn::Linear class_embed{nullptr};
    std::shared_ptr<torch::nn::Module> bbox_embed_;
    torch::nn::Embedding refpoint_embed{nullptr};
    torch::nn::Embedding query_feat{nullptr};
    std::shared_ptr<torch::nn::Module> segmentation_head_;
    bool is_compiled_eval_ = false;
    bool is_compiled_train_ = false;
    torch::jit::Module traced_model_eval_;
    torch::jit::Module traced_model_train_;
    bool has_traced_backbone_eval_ = false;
    bool has_traced_backbone_train_ = false;
    torch::jit::Module traced_backbone_eval_;
    torch::jit::Module traced_backbone_train_;
};

} // namespace fastloader::rfdetr
