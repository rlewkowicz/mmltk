#pragma once
#include <cuda_runtime_api.h>
#include <exception>
#include <string>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
namespace mmltk::backend::models::rfdetr {
// One optional current-frame transaction. Its caller retains the actual tensor
// owners until this boundary either settles work or throws the terminal CUDA
// condition. Successful preparation inserts no CPU visibility wait.
class PredictionRawPreparation final {
public:
 using Settle = cudaError_t (*)(cudaStream_t);
 PredictionRawPreparation(
  bool requested, mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotation, std::string& failure, cudaStream_t stream, Settle settle = &cudaStreamSynchronize) noexcept
     : available_(requested), annotation_(annotation), failure_(failure), stream_(stream), settle_(settle) {}
 [[nodiscard]] bool available() const noexcept { return available_; }
 template <typename Operation>
 void Execute(Operation&& operation) {
  if (!available_) return;
  try {
   std::forward<Operation>(operation)();
  } catch (...) {
   const auto failure = std::current_exception();
   available_ = false;
   const auto region = annotation_.source_region;
   annotation_ = {};
   annotation_.source_region = region;
   const auto status = settle_(stream_);
   if (status != cudaSuccess) throw mmltk::backend::ml::runtime::CudaOperationError{status, "prediction raw preparation settlement"};
   failure_ = "Prediction raw preparation failed";
   try {
    std::rethrow_exception(failure);
   } catch (const std::exception& error) { failure_ = error.what(); } catch (...) {
   }
  }
 }

private:
 bool available_;
 mmltk::backend::ml::runtime::AnalysisAnnotationStorage& annotation_;
 std::string& failure_;
 cudaStream_t stream_;
 Settle settle_;
};
}  // namespace mmltk::backend::models::rfdetr
