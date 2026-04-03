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

torch::Tensor box_cxcywh_to_xyxy(const torch::Tensor& value);

std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs,
                                           const torch::Tensor& target_sizes,
                                           int64_t num_select);

std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs,
                                                      int64_t target_height,
                                                      int64_t target_width,
                                                      int64_t num_select);

} // namespace mmltk::rfdetr
