#pragma once
#include <memory>
#include <torch/types.h>
#include "src/backend/ml/runtime/analysis_provider.h"
namespace mmltk::backend::models::rfdetr {
class PredictionCountStorage final {
   public:
    explicit PredictionCountStorage(int device);
    PredictionCountStorage(const PredictionCountStorage&) = delete;
    PredictionCountStorage& operator=(const PredictionCountStorage&) = delete;

   private:
    torch::Tensor host_, device_;
    int device_index_;
    friend void publish_prediction_count(std::shared_ptr<PredictionCountStorage>& storage, const torch::Tensor& count,
                                         mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output, int device);
};
void publish_prediction_count(std::shared_ptr<PredictionCountStorage>& storage, const torch::Tensor& count,
                              mmltk::backend::ml::runtime::AnalysisAnnotationStorage& output, int device);
}  // namespace mmltk::backend::models::rfdetr
