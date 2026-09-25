#include "src/backend/models/rfdetr/core/detection_ops.h"
#include <torch/torch.h>
#include <cmath>
#include <ATen/cuda/CUDAContext.h>
#include <optional>
#include <numbers>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_ops_private.h"
#include "detail/training_distributed.h"
#include <ATen/cuda/CUDAEvent.h>
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
struct TrainingTargetCounts::State final {
 State(std::size_t capacity, int device, const DistributedContext&);
 void begin(std::size_t);
 void publish(std::size_t, std::int64_t);
 void resolve();
 DeviceLossNormalizer consume(std::size_t, cudaStream_t);
 void fail(std::exception_ptr) noexcept;
 void rethrow_failure_locked() const;
 torch_cuda::TorchCudaStream launch_;
 std::vector<std::optional<torch_cuda::TorchCudaStream>> consumers_;
 std::mutex mutex_;
 std::condition_variable condition_;
 std::vector<std::int64_t> host_counts_;
 std::vector<bool> published_;
 torch::Tensor device_counts_;
 std::exception_ptr failure_;
 std::size_t slots_ = 0;
 int device_id_ = -1;
 DistributedContext distributed_;
 TrainingCollectiveWork work_;
 std::vector<at::cuda::CUDAEvent> ready_, consumed_;
 std::vector<bool> resolved_;
 bool distributed_abort_requested_ = false;
};
TrainingTargetCounts::State::State(const std::size_t capacity, const int device_id, const DistributedContext& distributed)
 : launch_(torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(device_id))), consumers_(capacity), host_counts_(capacity, 0), published_(capacity, false), device_id_(device_id), distributed_(distributed), work_(device_id, capacity), ready_(capacity), consumed_(capacity), resolved_(capacity, false) {
 if (!capacity) throw std::invalid_argument("target count capacity must be positive");
 device_counts_ = torch::empty({static_cast<std::int64_t>(capacity)}, torch::TensorOptions().dtype(torch::kInt64).device(torch_cuda::cuda_device(device_id)));
 begin(capacity);
}
void TrainingTargetCounts::State::begin(std::size_t slots) {
 std::lock_guard lock(mutex_); rethrow_failure_locked();
 if (!slots || slots > host_counts_.size()) throw std::invalid_argument("invalid count slot admission");
 // Previous waves have drained their futures. Settle collective slots once,
 // then order any delayed count-to-float reads before overwriting scalar storage.
 work_.settle();
 for (std::size_t lane = 0; lane < consumers_.size(); ++lane) if (consumers_[lane]) consumed_[lane].block(launch_);
 slots_ = slots; std::fill(published_.begin(), published_.end(), false); std::fill(resolved_.begin(), resolved_.end(), false);
}
void TrainingTargetCounts::State::publish(const std::size_t lane, const std::int64_t count) {
 std::lock_guard lock(mutex_); rethrow_failure_locked();
 if (lane >= slots_ || published_[lane] || count < 0) throw std::runtime_error("invalid logical target count publication");
 host_counts_[lane] = count; published_[lane] = true; condition_.notify_all();
}
void TrainingTargetCounts::State::resolve() {
 torch_cuda::TorchCudaStreamGuard guard(launch_);
 try {
  // Exactly one scalar collective per logical microbatch, in admission order.
  // A slot's worker resumes immediately; it never waits for another wave.
  for (std::size_t lane = 0; lane < slots_; ++lane) {
   {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] { return failure_ || published_[lane]; }); rethrow_failure_locked();
    if (resolved_[lane]) continue;
   }
   auto count = device_counts_.narrow(0, static_cast<std::int64_t>(lane), 1);
   count.fill_(host_counts_[lane]);
   work_.join(work_.all_reduce(distributed_, count));
   ready_[lane].record(torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(device_id_)));
   { std::lock_guard lock(mutex_); rethrow_failure_locked(); resolved_[lane] = true; }
   condition_.notify_all();
  }
  work_.record_completion();
 } catch (...) { fail(std::current_exception()); std::lock_guard lock(mutex_); rethrow_failure_locked(); }
}
DeviceLossNormalizer TrainingTargetCounts::State::consume(const std::size_t lane, const cudaStream_t stream) {
 {
  std::unique_lock lock(mutex_);
  if (lane >= slots_) throw std::runtime_error("target count slot out of range");
  condition_.wait(lock, [&] { return failure_ || resolved_[lane]; }); rethrow_failure_locked();
  consumers_[lane] = torch_cuda::getStreamFromExternal(stream, torch_cuda::checked_device_index(device_id_));
 }
 const auto consumer = *consumers_[lane];
 torch_cuda::TorchCudaStreamGuard stream_guard(consumer);
 ready_[lane].block(consumer); device_counts_.record_stream(consumer);
 auto value = device_counts_.select(0, static_cast<std::int64_t>(lane)).to(torch::kFloat32);
 consumed_[lane].record(consumer);
 return {std::move(value)};
}
void TrainingTargetCounts::State::fail(std::exception_ptr failure) noexcept {
 bool abort = false;
 { std::lock_guard lock(mutex_); if (!failure_) { failure_ = failure; abort = !distributed_abort_requested_; distributed_abort_requested_ = true; } }
 condition_.notify_all();
 if (abort) distributed_abort(distributed_);
}
void TrainingTargetCounts::State::rethrow_failure_locked() const { if (failure_) std::rethrow_exception(failure_); }
TrainingTargetCounts::TrainingTargetCounts(std::size_t capacity, int device, const DistributedContext& group)
 : state_(std::make_shared<State>(capacity, device, group)) {}
TrainingTargetCounts::~TrainingTargetCounts() noexcept { retire(); }
void TrainingTargetCounts::retire() noexcept {
 if (!state_) return;
 // Waves drain their futures before the final borrower releases this owner.
 // Count conversions may have been queued before a worker failed, so settle
 // both the publisher and the bounded consumer-stream inventory.
 cudaError_t failure = state_->work_.retire();
 if (failure != cudaSuccess) {
  std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state_)), failure);
  return;
 }
 try {
  torch_cuda::TorchCudaDeviceGuard device(torch_cuda::checked_device_index(state_->device_id_));
  failure = cudaStreamSynchronize(state_->launch_.stream());
  for (const auto& stream : state_->consumers_) if (stream) {
   const auto status = cudaStreamSynchronize(stream->stream());
   if (failure == cudaSuccess) failure = status;
  }
 } catch (...) { if (failure == cudaSuccess) failure = cudaErrorUnknown; }
 if (failure != cudaSuccess) std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(state_)), failure);
 else state_.reset();
}
void TrainingTargetCounts::begin(std::size_t slots) { state_->begin(slots); }
void TrainingTargetCounts::publish(std::size_t lane, std::int64_t count) { state_->publish(lane, count); }
void TrainingTargetCounts::resolve() { state_->resolve(); }
DeviceLossNormalizer TrainingTargetCounts::consume(std::size_t lane, cudaStream_t stream) { return state_->consume(lane, stream); }
void TrainingTargetCounts::fail(std::exception_ptr failure) noexcept { state_->fail(std::move(failure)); }
GradScaler::GradScaler(const bool enabled, const float init_scale, const float growth_factor, const float backoff_factor, const int growth_interval)
    : enabled_(enabled), scale_(init_scale), growth_factor_(growth_factor), backoff_factor_(backoff_factor), growth_interval_(growth_interval) {}
torch::Tensor GradScaler::scale(const torch::Tensor& loss) { return enabled_ ? loss * scale_ : loss; }
void GradScaler::update(const bool found_inf) {
 if (!enabled_) return;
 if (found_inf) {
  scale_ *= backoff_factor_;
  growth_tracker_ = 0;
 } else if (++growth_tracker_ >= growth_interval_) {
  scale_ *= growth_factor_;
  growth_tracker_ = 0;
 }
}
bool GradScaler::enabled() const noexcept { return enabled_; }
float GradScaler::current_scale() const noexcept { return scale_; }
int GradScaler::growth_tracker() const noexcept { return growth_tracker_; }
void GradScaler::load_state(const float scale, const int growth_tracker) noexcept {
 if (enabled_) {
  scale_ = scale;
  growth_tracker_ = std::max(0, growth_tracker);
 }
}
void GradScaler::ensure_device_state(const torch::Device& device) {
 if (found_inf_device_.defined() && found_inf_device_.device() == device) return;
 const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
 found_inf_device_ = torch::zeros({}, options);
 inverse_scale_device_ = torch::ones({}, options);
}
torch::Tensor loss_value_or_zero(const TensorMap& loss_dict, const torch::Device& device, std::string_view key) {
 const auto found = loss_dict.find(std::string(key));
 if (found != loss_dict.end()) { return found->second; }
 return torch::zeros({}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
}
TrainingScalarPacket::Tensors ordinary_scalar_tensors(const TensorMap& losses, const torch::Tensor& total, const torch::Tensor& auxiliary) {
 TrainingScalarPacket::Tensors values;
 TrainingScalarPacket::set<^^TrainingScalars::total>(values, total);
 TrainingScalarPacket::set<^^TrainingScalars::auxiliary_weighted>(values, auxiliary);
 const auto assign = [&]<std::meta::info Member>(std::string_view name) {
  const auto found = losses.find(std::string(name));
  if (found != losses.end()) TrainingScalarPacket::set<Member>(values, found->second);
 };
 assign.operator()<^^TrainingScalars::classification>("loss_ce");
 assign.operator()<^^TrainingScalars::l1>("loss_bbox");
 assign.operator()<^^TrainingScalars::giou>("loss_giou");
 assign.operator()<^^TrainingScalars::mask_ce>("loss_mask_ce");
 assign.operator()<^^TrainingScalars::mask_dice>("loss_mask_dice");
 assign.operator()<^^TrainingScalars::class_error>("class_error");
 assign.operator()<^^TrainingScalars::cardinality_error>("cardinality_error");
 return values;
}
RoutedTrainingLoss compute_routed_training_loss(NativeRfDetrModel& model, const TrainingSupervisionRoute route, const ModelOutputs& outputs, const PreparedTargets& targets,
 const DeviceLossNormalizer& normalizer, const DetectionConfig& detection_config) {
 if (route_uses_match_free(route)) {
  const auto loss = model.supervision_loss(outputs, targets, normalizer, true);
  TrainingScalarPacket::Tensors scalars;
  TrainingScalarPacket::set<^^TrainingScalars::total>(scalars, loss.total);
  TrainingScalarPacket::set<^^TrainingScalars::classification>(scalars, loss.main.classification);
  TrainingScalarPacket::set<^^TrainingScalars::l1>(scalars, loss.main.box);
  TrainingScalarPacket::set<^^TrainingScalars::giou>(scalars, loss.main.giou);
  TrainingScalarPacket::set<^^TrainingScalars::mask_ce>(scalars, loss.main.mask_ce);
  TrainingScalarPacket::set<^^TrainingScalars::mask_dice>(scalars, loss.main.mask_dice);
  TrainingScalarPacket::set<^^TrainingScalars::correspondence_weighted>(scalars, loss.correspondence);
  TrainingScalarPacket::set<^^TrainingScalars::auxiliary_weighted>(scalars, loss.auxiliary);
  if (route_uses_denoising(route)) TrainingScalarPacket::set<^^TrainingScalars::denoising_weighted>(scalars, loss.denoising);
  return {loss.total, loss.classification, loss.box + loss.giou, {}, std::move(scalars)};
 }
 const double group_divisor = detection_config.sum_group_losses ? 1.0 : static_cast<double>(detection_config.group_detr);
 const auto num_boxes = torch::clamp_min(normalizer.target_count * group_divisor, 1.0);
 DetectionStatisticsPacket::Tensors statistics;
 auto ordinary_terms = detection_loss_dict(outputs, targets, detection_config, true, num_boxes, &statistics);
 torch::Tensor auxiliary;
 auto total = weighted_detection_loss(ordinary_terms, detection_config, outputs.main.pred_logits.device(), &auxiliary);
 auto scalars = ordinary_scalar_tensors(ordinary_terms, total, auxiliary);
 auto classification = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_ce");
 auto box = loss_value_or_zero(ordinary_terms, outputs.main.pred_logits.device(), "loss_bbox");
 if (route_uses_denoising(route)) {
  const auto denoising = model.supervision_loss(outputs, targets, normalizer, true);
  total = total + denoising.total;
  TrainingScalarPacket::set<^^TrainingScalars::denoising_weighted>(scalars, denoising.total);
  classification = classification + denoising.classification;
  box = box + denoising.box + denoising.giou;
 }
 TrainingScalarPacket::set<^^TrainingScalars::total>(scalars, total);
 return {std::move(total), std::move(classification), std::move(box), std::move(ordinary_terms), std::move(scalars), std::move(statistics)};
}
}  // namespace mmltk::backend::models::rfdetr
