#pragma once
#include <torch/types.h>
#include <torch/csrc/autograd/autograd.h>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/backend/ml/cuda/torch_autocast_scope.h"
namespace mmltk::backend::models::rfdetr {
// One admitted microbatch's forward/criterion and backward policy, shared by
// the main owner and worker lanes. Callers retain target and stream custody.
class TrainingStep final {
public:
 TrainingStep(std::size_t admitted_microbatches, double gradient_scale, bool amp_enabled, at::ScalarType dtype)
  : factor_(gradient_scale), amp_(amp_enabled), dtype_(dtype) {
  if (admitted_microbatches == 0) throw std::invalid_argument("training step requires an admitted effective batch");
  const auto count=static_cast<double>(admitted_microbatches);
  factor_/=count*count;
 }
 template<typename Forward>
 void forward(Forward&& run) const {
  mmltk::backend::ml::cuda::TorchAutocastScope precision(amp_,dtype_);
  std::forward<Forward>(run)();
 }
 void backward(const torch::Tensor& loss) const { scaled_loss(loss).backward(); }
 [[nodiscard]] std::vector<torch::Tensor> gradients(const torch::Tensor& loss, const std::vector<torch::Tensor>& parameters) const {
  return torch::autograd::grad({scaled_loss(loss)},parameters,{},std::nullopt,false,true);
 }
private:
 [[nodiscard]] torch::Tensor scaled_loss(const torch::Tensor& loss) const { return loss*factor_; }
 // Stock user loss / K and closure / K, fused with AMP scaling into one
 // scalar tensor multiplication; distributed averaging/unscale/clip follow.
 double factor_;
 bool amp_;
 at::ScalarType dtype_;
};
}  // namespace mmltk::backend::models::rfdetr
