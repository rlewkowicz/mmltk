#pragma once
#include <numeric>
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
#if defined(USE_C10D_NCCL)
#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#endif
#include "src/backend/models/rfdetr/core/detection_types.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
struct DistributedContext {
 bool enabled = false;
 int rank = 0;
 int world_size = 1;
#if defined(USE_C10D_NCCL)
 c10::intrusive_ptr<c10d::Store> store;
 c10::intrusive_ptr<c10d::Backend> process_group;
#endif
};
void distributed_all_reduce_tensor(const DistributedContext& distributed, torch::Tensor& tensor);
class WaveTargetNormalizer final {
public:
 WaveTargetNormalizer(std::size_t lanes, int device_id, const DistributedContext& distributed);
 ~WaveTargetNormalizer() noexcept;
 WaveTargetNormalizer(const WaveTargetNormalizer&) = delete;
 WaveTargetNormalizer& operator=(const WaveTargetNormalizer&) = delete;
 void publish(std::size_t lane, std::int64_t target_count);
 void resolve(const DistributedContext& distributed, const torch::Device& device);
 [[nodiscard]] DeviceLossNormalizer consume(std::size_t lane, cudaStream_t stream);
 void fail(std::exception_ptr failure) noexcept;

private:
 void rethrow_failure_locked() const;
 std::mutex mutex_;
 std::condition_variable condition_;
 std::vector<std::int64_t> host_counts_;
 std::vector<bool> published_;
 torch::Tensor device_counts_;
 std::exception_ptr failure_;
 std::size_t published_count_ = 0;
 int device_id_ = -1;
 const DistributedContext* distributed_ = nullptr;
 cudaEvent_t ready_ = nullptr;
 bool resolved_ = false;
 bool distributed_abort_requested_ = false;
};
template <class Result>
class ParallelTrainingWave final {
public:
 ParallelTrainingWave(const std::size_t lane_count, const bool active, const int device_id, const DistributedContext& distributed) : distributed_(&distributed) {
  futures_.reserve(lane_count);
  results_.reserve(lane_count);
  if (active) { normalizer_ = std::make_shared<WaveTargetNormalizer>(lane_count, device_id, distributed); }
 }
 ~ParallelTrainingWave() noexcept {
  if (!settled_) {
   fail(std::make_exception_ptr(std::runtime_error("RF-DETR parallel training wave was cancelled")));
   drain();
  }
 }
 ParallelTrainingWave(const ParallelTrainingWave&) = delete;
 ParallelTrainingWave& operator=(const ParallelTrainingWave&) = delete;
 [[nodiscard]] const std::shared_ptr<WaveTargetNormalizer>& normalizer() const noexcept { return normalizer_; }
 void add(std::future<Result> future) { futures_.push_back(std::move(future)); }
 template <class Consumer>
 void settle(const torch::Device& device, Consumer&& consume) {
  try {
   if (normalizer_) { normalizer_->resolve(*distributed_, device); }
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
 const DistributedContext* distributed_;
 std::shared_ptr<WaveTargetNormalizer> normalizer_;
 std::vector<std::future<Result>> futures_;
 std::vector<Result> results_;
 std::exception_ptr failure_;
 bool settled_ = false;
};
class GradScaler {
public:
 explicit GradScaler(bool enabled, float init_scale = 65536.0f, float growth_factor = 2.0f, float backoff_factor = 0.5f, int growth_interval = 2000);
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
struct LrScheduleConfig {
 double warmup_epochs = 0.0;
 double warmup_momentum = 0.0;
 TrainLrSchedulerKind lr_scheduler = TrainLrSchedulerKind::Cosine;
 int64_t lr_drop = 1;
 double lr_min_factor = 0.0;
};
double compute_lr_scale(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch, int64_t total_training_steps);
double compute_warmup_momentum(const LrScheduleConfig& config, int64_t current_step, int64_t steps_per_epoch, double target_momentum);
template <typename OptimizerLike>
inline void set_optimizer_lrs(OptimizerLike& optimizer, const std::vector<double>& base_lrs, const double scale) {
 optimizer.set_lrs(base_lrs, scale);
}
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
 scalar_packet::Tensors scalars;
};
torch::Tensor loss_value_or_zero(const TensorMap&, const torch::Device&, std::string_view);
scalar_packet::Tensors ordinary_scalar_tensors(const TensorMap&, const torch::Tensor&, const torch::Tensor&);
RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel&, TrainingSupervisionRoute, const ModelOutputs&, const PreparedTargets&, const DeviceLossNormalizer&, const DetectionConfig&);
}  // namespace mmltk::backend::models::rfdetr
