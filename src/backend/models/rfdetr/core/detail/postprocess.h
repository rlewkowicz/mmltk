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

// Top-k products retain their originating query until the consumer selects survivors.
// Mask logits remain at model resolution; selection never expands them.
struct PostprocessedSelection {
    torch::Tensor scores;
    torch::Tensor labels;
    torch::Tensor boxes;
    torch::Tensor query_indices;
    std::optional<torch::Tensor> mask_logits;
};
PostprocessedSelection select_output_batch_fixed_size(const OutputTensors&, int64_t height, int64_t width, int64_t count, bool require_masks = false);
class SelectedMaskWorkspace final {
   public:
    torch::Tensor Materialize(const torch::Tensor& logits, const torch::Tensor& query_indices, int64_t height, int64_t width);
   private:
    torch::Tensor gathered_, expanded_, masks_;
};
torch::Tensor materialize_selected_masks(const torch::Tensor& logits, const torch::Tensor& query_indices,
                                         int64_t height, int64_t width);

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
