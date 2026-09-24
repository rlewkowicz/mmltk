#pragma once
#include <memory>
#include <cstddef>
#include <vector>
#include <torch/types.h>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
namespace mmltk::backend::models::rfdetr {
// One registered allocation per batch. Pending scalar views retain this exact
// batch, including its source tensors, until their final borrower settles.
class PredictionCountStorage final : public std::enable_shared_from_this<PredictionCountStorage> {
public:
 PredictionCountStorage(int device, std::size_t capacity,
  std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> = {}, mmltk::frameworks::gpu::PinnedHostBuffer::Operations = {});
 PredictionCountStorage(const PredictionCountStorage&) = delete;
 PredictionCountStorage& operator=(const PredictionCountStorage&) = delete;
 static void Prepare(std::shared_ptr<PredictionCountStorage>&, std::size_t count, int device,
  std::shared_ptr<mmltk::frameworks::gpu::TerminalCudaRetirementOwner> = {}, mmltk::frameworks::gpu::PinnedHostBuffer::Operations = {});
 void Publish(std::size_t index, const torch::Tensor&, mmltk::backend::ml::runtime::AnalysisAnnotationStorage&);
 [[nodiscard]] CUresult ReleaseSettled() noexcept;
private:
 mmltk::backend::ml::cuda::NumaHostTensor storage_;
 torch::Tensor host_;
 std::vector<torch::Tensor> devices_;
 int device_index_;
};
}  // namespace mmltk::backend::models::rfdetr
