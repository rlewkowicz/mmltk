#pragma once
#include <optional>
#include <string>
#include <vector>

#include "detection_geometry.h"
#include "detection_types.h"
#include "torch_api.h"

namespace mmltk::backend::models::rfdetr {
namespace postprocess_detail = mmltk::backend::ml::torch_api;
}

namespace mmltk::backend::models::rfdetr {

struct PostprocessedBatch {
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    std::optional<torch::Tensor> masks;

    [[nodiscard]] int64_t size() const { return scores.defined() ? scores.size(0) : 0; }
};

std::vector<TensorMap> postprocess_outputs(const OutputTensors& outputs, const torch::Tensor& target_sizes, int64_t num_select);
std::vector<TensorMap> postprocess_outputs(const ModelOutputs& outputs, const torch::Tensor& target_sizes, int64_t num_select);

std::vector<TensorMap> postprocess_outputs_fixed_size(const OutputTensors& outputs, int64_t target_height, int64_t target_width,
                                                      int64_t num_select);

PostprocessedBatch postprocess_output_batch_fixed_size(const OutputTensors& outputs, int64_t target_height, int64_t target_width,
                                                       int64_t num_select);
PostprocessedBatch postprocessed_batch_from_result(const TensorMap& result);
std::vector<TensorMap> split_postprocessed_batch(const PostprocessedBatch& batch);

}  // namespace mmltk::backend::models::rfdetr
