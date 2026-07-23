#pragma once

#include <torch/torch.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

struct OutputTensors {
    torch::Tensor pred_logits;
    torch::Tensor pred_boxes;
    std::optional<torch::Tensor> pred_masks;
};

using TensorMap = std::map<std::string, torch::Tensor>;

struct PostprocessedBatch {
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    std::optional<torch::Tensor> masks;

    [[nodiscard]] int64_t size() const {
        return scores.defined() ? scores.size(0) : 0;
    }
};

torch::Tensor box_cxcywh_to_xyxy(const torch::Tensor& value);

std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs, const torch::Tensor& target_sizes,
                                           int64_t num_select);

std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs, int64_t target_height,
                                                      int64_t target_width, int64_t num_select);

PostprocessedBatch postprocess_output_batch_fixed_size(const OutputTensors& outputs, int64_t target_height,
                                                       int64_t target_width, int64_t num_select);
PostprocessedBatch postprocessed_batch_from_result(const TensorMap& result);
std::vector<TensorMap> split_postprocessed_batch(const PostprocessedBatch& batch);

}  
