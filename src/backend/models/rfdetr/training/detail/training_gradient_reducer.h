#pragma once
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include <torch/types.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
namespace mmltk::backend::models::rfdetr {
namespace testsupport {
struct TrainingGradientReducerTestAccess;
}
struct DistributedContext;
// One trajectory's gradients. The session supplies its sole collective launch
// stream and drains an attempt before admitting another model's communication.
class TrainingGradientReducer final {
public:
 TrainingGradientReducer(const DistributedContext&, int device, mmltk::backend::ml::cuda::TorchCudaStream launch_stream, const std::vector<std::string>& names,
  const std::vector<torch::Tensor>& master, const std::vector<std::vector<torch::Tensor>>& leaves, std::size_t bucket_bytes = 4U * 1024U * 1024U);
 ~TrainingGradientReducer();
 TrainingGradientReducer(const TrainingGradientReducer&) = delete;
 TrainingGradientReducer& operator=(const TrainingGradientReducer&) = delete;
 void rebuild(const std::vector<std::string>& names, const std::vector<torch::Tensor>& master, const std::vector<std::vector<torch::Tensor>>& leaves);
 void begin_attempt(std::size_t logical_contributions);
 void arm(std::size_t slot);
 // Call only after autograd::grad returns; closes unused parameters and releases
 // the slot. Hooks close fully used buckets earlier, during backward itself.
 void collect(std::size_t slot);
 void contribute_empty();
 void enable_gradient_launch_after_counts();
 void finish_attempt();
 void abort(std::exception_ptr first_error) noexcept;
 [[nodiscard]] std::size_t launched_buckets() const;
 [[nodiscard]] std::size_t bucket_count() const;

private:
 friend struct testsupport::TrainingGradientReducerTestAccess;
 void retire() noexcept;
 struct Impl;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
