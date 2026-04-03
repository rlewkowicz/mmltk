#pragma once

#include "mmltk/rfdetr/artifacts.h"
#include "mmltk/rfdetr/detection_types.h"

#include <torch/torch.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mmltk::rfdetr {

class SegmentationHeadImpl;
struct StateDictLoadSummary;

struct NestedTensor {
    torch::Tensor tensors;
    torch::Tensor mask;

    [[nodiscard]] NestedTensor to(const torch::Device& device) const {
        NestedTensor result;
        result.tensors = tensors.to(device);
        if (mask.defined()) {
            result.mask = mask.to(device);
        }
        return result;
    }

    [[nodiscard]] NestedTensor pin_memory() const {
        NestedTensor result;
        result.tensors = tensors.pin_memory();
        if (mask.defined()) {
            result.mask = mask.pin_memory();
        }
        return result;
    }

    [[nodiscard]] std::pair<torch::Tensor, torch::Tensor> decompose() const {
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

    const auto batch = static_cast<int64_t>(tensor_list.size());
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

class NativeRfDetrModel : public torch::nn::Module {
public:
    explicit NativeRfDetrModel(const NativeRfDetrConfig& config = {});

    ModelOutputs forward(const NestedTensor& batch,
                         SegmentationHeadImpl* seg_override = nullptr,
                         bool include_masks = true);
    StateDictLoadSummary load_weights(const std::filesystem::path& weights_path, bool strict = false);

    std::shared_ptr<SegmentationHeadImpl> clone_segmentation_head() const;
    SegmentationHeadImpl* segmentation_head_ptr() const;

    void optimize_for_inference(int batch_size = 1, bool for_training = false,
                                CompilationMode mode = CompilationMode::kSelective);
    bool is_compiled(bool for_training) const { return for_training ? is_compiled_train_ : is_compiled_eval_; }

    void export_onnx(const std::filesystem::path& output_path,
                     int opset_version = 19, int batch_size = 1,
                     bool simplify = false);
    void set_force_pytorch_deformable_attn(bool value);

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

void export_weights_to_onnx(const std::filesystem::path& weights_path,
                            const std::filesystem::path& output_path,
                            int device_id = 0,
                            int opset_version = 19,
                            bool simplify = false);

} // namespace mmltk::rfdetr
