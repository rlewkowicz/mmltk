#pragma once
#include <memory>
#include <torch/types.h>
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/backend/ml/runtime/analysis_provider.h"
namespace mmltk::backend::models::rfdetr {
class PredictionCountStorage final {
 public:
    explicit PredictionCountStorage(int device) : host_(mmltk::backend::ml::cuda::numa_empty({1}, at::kLong, device)) {}
    void Publish(const torch::Tensor& count, mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output) {
        device_ = count;
        host_.copy_(device_, true);
        output.value_count = 0;
        output.device_value_count = device_.data_ptr<std::int64_t>();
        output.completed_value_count = host_.data_ptr<std::int64_t>();
    }
 private:
    torch::Tensor host_, device_;
};
inline void publish_prediction_count(std::shared_ptr<PredictionCountStorage>& storage, const torch::Tensor& count,
                                     mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output, int device) {
    output.count_custody.reset();
    if (!storage || storage.use_count() != 1) storage = std::make_shared<PredictionCountStorage>(device);
    storage->Publish(count, output);
    output.count_custody = storage;
}
}
