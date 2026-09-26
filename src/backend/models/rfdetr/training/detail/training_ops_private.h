#pragma once
#include "src/backend/models/rfdetr/core/detection_statistics.h"
#include "training_schedule.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include <array>
#include <numeric>
#include <span>
#include <string_view>
#include "src/backend/models/rfdetr/core/model.h"
#include "training_scalar_packet.h"
#include <cuda_runtime_api.h>
#include <torch/types.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
struct DistributedContext;
namespace testsupport {
struct TrainingDistributedTestAccess;
}
class TrainingTargetCounts final {
public:
 TrainingTargetCounts(std::size_t lanes, int device_id, const DistributedContext& distributed);
 ~TrainingTargetCounts() noexcept;
 TrainingTargetCounts(const TrainingTargetCounts&) = delete;
 TrainingTargetCounts& operator=(const TrainingTargetCounts&) = delete;
 void begin(std::size_t slots);
 void publish(std::size_t lane, std::int64_t target_count);
 void resolve();
 [[nodiscard]] DeviceLossNormalizer consume(std::size_t lane, cudaStream_t stream);
 void fail(std::exception_ptr failure) noexcept;

private:
 friend struct testsupport::TrainingDistributedTestAccess;
 void retire() noexcept;
 struct State;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
 std::shared_ptr<State> state_;
};
template <class Result>
class ParallelTrainingWave final {
public:
 ParallelTrainingWave(const std::size_t lane_count, const bool active, const int device_id, const DistributedContext& distributed, std::shared_ptr<TrainingTargetCounts> counts = {}) {
  futures_.reserve(lane_count);
  results_.reserve(lane_count);
  if (active) {
   normalizer_ = counts ? std::move(counts) : std::make_shared<TrainingTargetCounts>(lane_count, device_id, distributed);
   normalizer_->begin(lane_count);
  }
 }
 ~ParallelTrainingWave() noexcept {
  if (!settled_) {
   fail(std::make_exception_ptr(std::runtime_error("RF-DETR parallel training wave was cancelled")));
   drain();
  }
 }
 ParallelTrainingWave(const ParallelTrainingWave&) = delete;
 ParallelTrainingWave& operator=(const ParallelTrainingWave&) = delete;
 [[nodiscard]] const std::shared_ptr<TrainingTargetCounts>& normalizer() const noexcept { return normalizer_; }
 void add(std::future<Result> future) { futures_.push_back(std::move(future)); }
 void resolve_counts() {
  if (normalizer_) normalizer_->resolve();
  counts_resolved_ = true;
 }
 template <class Consumer>
 void settle(Consumer&& consume) {
  try {
   if (!counts_resolved_) resolve_counts();
  } catch (...) { fail(std::current_exception()); }
  drain();
  settled_ = true;
  if (failure_) { std::rethrow_exception(failure_); }
  try {
   for (auto& result : results_) { consume(result); }
  } catch (...) {
   fail(std::current_exception());
   std::rethrow_exception(failure_);
  }
 }

private:
 void fail(std::exception_ptr failure) noexcept {
  if (!failure_) { failure_ = std::move(failure); }
  if (normalizer_) { normalizer_->fail(failure_); }
 }
 void drain() noexcept {
  for (auto& future : futures_) {
   if (!future.valid()) { continue; }
   try {
    results_.push_back(future.get());
   } catch (...) { fail(std::current_exception()); }
  }
  futures_.clear();
 }
 std::shared_ptr<TrainingTargetCounts> normalizer_;
 std::vector<std::future<Result>> futures_;
 std::vector<Result> results_;
 std::exception_ptr failure_;
 bool settled_ = false;
 bool counts_resolved_ = false;
};
class GradScaler {
public:
 static constexpr float kInitialScale = 65536.0f;
 static constexpr int kGrowthInterval = 2000;
 explicit GradScaler(bool enabled, float init_scale = kInitialScale, float growth_factor = 2.0f, float backoff_factor = 0.5f, int growth_interval = kGrowthInterval);
 torch::Tensor scale(const torch::Tensor& loss);
 template <typename OptimizerLike>
 torch::Tensor check_and_unscale_(OptimizerLike& optimizer) {
  gradient_scratch_.clear();
  gradient_scratch_.reserve(optimizer.parameters().size());
  for (auto& param : optimizer.parameters()) {
   if (!param.grad().defined()) { continue; }
   gradient_scratch_.push_back(param.mutable_grad());
  }
  const auto& parameters = optimizer.parameters();
  if (parameters.empty()) { throw std::runtime_error("gradient finite check requires optimizer parameters"); }
  ensure_device_state(parameters.front().device());
  found_inf_device_.zero_();
  inverse_scale_device_.fill_(enabled_ ? 1.0f / scale_ : 1.0f);
  if (!gradient_scratch_.empty()) { at::_amp_foreach_non_finite_check_and_unscale_(gradient_scratch_, found_inf_device_, inverse_scale_device_); }
  gradient_scratch_.clear();
  return found_inf_device_;
 }
 template <typename OptimizerLike>
 void step(OptimizerLike& optimizer, bool found_inf) {
  if (!found_inf) { optimizer.step(); }
 }
 void update(bool found_inf);
 [[nodiscard]] bool enabled() const noexcept;
 [[nodiscard]] float current_scale() const noexcept;
 [[nodiscard]] int growth_tracker() const noexcept;
 void load_state(float scale, int growth_tracker) noexcept;

private:
 void ensure_device_state(const torch::Device& device);
 bool enabled_;
 float scale_;
 float growth_factor_;
 float backoff_factor_;
 int growth_interval_;
 int growth_tracker_ = 0;
 torch::Tensor found_inf_device_;
 torch::Tensor inverse_scale_device_;
 std::vector<torch::Tensor> gradient_scratch_;
};
enum class TrainingSupervisionRoute : std::uint8_t {
 Hungarian,
 MatchFree,
 HungarianDenoising,
 MatchFreeDenoising,
};
[[nodiscard]] inline TrainingSupervisionRoute supervision_route(const TrainingSupervisionConfig& config) noexcept {
 const bool match_free = config.assignment == TrainAssignmentKind::MatchFree;
 const bool denoising = config.denoising.enabled;
 if (match_free && denoising) return TrainingSupervisionRoute::MatchFreeDenoising;
 if (match_free) return TrainingSupervisionRoute::MatchFree;
 if (denoising) return TrainingSupervisionRoute::HungarianDenoising;
 return TrainingSupervisionRoute::Hungarian;
}
[[nodiscard]] inline bool route_is_active(const TrainingSupervisionRoute route) noexcept { return route != TrainingSupervisionRoute::Hungarian; }
[[nodiscard]] inline bool route_uses_match_free(const TrainingSupervisionRoute route) noexcept {
 return route == TrainingSupervisionRoute::MatchFree || route == TrainingSupervisionRoute::MatchFreeDenoising;
}
[[nodiscard]] inline bool route_uses_denoising(const TrainingSupervisionRoute route) noexcept {
 return route == TrainingSupervisionRoute::HungarianDenoising || route == TrainingSupervisionRoute::MatchFreeDenoising;
}
class SupervisionTimingLease final {
public:
 enum class Kind : std::uint8_t { Step, Criterion };
 SupervisionTimingLease(NativeRfDetrModel& owner, const bool active, const Kind kind) : owner_(active ? &owner : nullptr), kind_(kind) {
  if (owner_ != nullptr) { begin(); }
 }
 ~SupervisionTimingLease() noexcept {
  if (owner_ != nullptr) {
   try {
    end();
   } catch (...) {}
  }
 }
 SupervisionTimingLease(const SupervisionTimingLease&) = delete;
 SupervisionTimingLease& operator=(const SupervisionTimingLease&) = delete;
 void finish() {
  if (owner_ != nullptr) {
   end();
   owner_ = nullptr;
  }
 }

private:
 void begin() {
  if (kind_ == Kind::Step) {
   owner_->begin_supervised_step_timing();
  } else {
   owner_->begin_criterion_timing();
  }
 }
 void end() {
  if (kind_ == Kind::Step) {
   owner_->end_supervised_step_timing();
  } else {
   owner_->end_criterion_timing();
  }
 }
 NativeRfDetrModel* owner_;
 Kind kind_;
};
inline int64_t prepared_target_count(const PreparedTargets& targets) { return std::accumulate(targets.counts.begin(), targets.counts.end(), int64_t{0}); }
struct RoutedTrainingLoss {
 torch::Tensor total;
 torch::Tensor classification;
 torch::Tensor box;
 TensorMap ordinary_terms;
 TrainingScalarPacket::Tensors scalars;
 DetectionStatisticsPacket::Tensors statistics{};
};
torch::Tensor loss_value_or_zero(const TensorMap&, const torch::Device&, std::string_view);
TrainingScalarPacket::Tensors ordinary_scalar_tensors(const TensorMap&, const torch::Tensor&, const torch::Tensor&);
RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel&, TrainingSupervisionRoute, const ModelOutputs&, const PreparedTargets&, const DeviceLossNormalizer&, const DetectionConfig&);
}  // namespace mmltk::backend::models::rfdetr
