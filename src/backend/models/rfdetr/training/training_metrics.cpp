#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_metrics.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/frameworks/gpu/cuda_device_scope.h"
#include "src/frameworks/gpu/cuda_error.h"
#include <atomic>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include "src/backend/ml/cuda/shared_cuda_event.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
using mmltk::frameworks::gpu::ensure_cuda_ok;
struct TrainingMetricHandoff::Impl {
public:
 explicit Impl(int device_id) : device_id_(device_id), event_pool_(mmltk::frameworks::gpu::make_cuda_device_owner<Impl, &Impl::record_failure>(this, device_id), 1U, retirement_owner_) {
  const auto device_options = torch::TensorOptions().dtype(torch::kFloat32).device(mmltk::backend::ml::cuda::cuda_device(device_id_));
  device_values_ = torch::zeros({10 + static_cast<int64_t>(scalar_packet::size)}, device_options);
  device_values_.select(0, 6).fill_(1.0f);
  scalar_values_ = torch::empty({static_cast<int64_t>(scalar_packet::size)}, device_options);
  unavailable_ = torch::zeros({}, device_options);
  attempt_values_ = torch::zeros({3 + 2 * static_cast<int64_t>(scalar_packet::size)}, device_options);
  control_ = torch::zeros({2}, device_options);
  host_values_ = torch_cuda::numa_empty({10 + static_cast<int64_t>(scalar_packet::size)}, torch::kFloat32, device_id_);
  torch_cuda::TorchCudaDeviceGuard device_guard(torch_cuda::checked_device_index(device_id_));
  ensure_cuda_ok(cudaStreamCreateWithFlags(&settlement_stream_, cudaStreamNonBlocking), "create training metric settlement stream");
 }
 ~Impl() noexcept {
  if (settlement_stream_ != nullptr) {
   mmltk::frameworks::gpu::CudaDeviceScope scope(device_id_);
   const cudaError_t status = scope ? cudaStreamDestroy(settlement_stream_) : scope.status();
   static_cast<void>(scope.FinalizeStatus(status));
  }
 }
 Impl(const Impl&) = delete;
 Impl& operator=(const Impl&) = delete;
 void record_failure(const cudaError_t failure) noexcept { mmltk::frameworks::gpu::record_first_cuda_failure(first_failure_, failure); }
 void reset_epoch() {
  device_values_.zero_();
  device_values_.select(0, 6).fill_(1.0f);
  epoch_state_ = {};
 }
 void restore_epoch(const TrainingEpochMetricState& state) {
  reset_epoch();
  if (!state.microbatches) return;
  host_values_.zero_();
  auto* values = host_values_.data_ptr<float>();
  values[0] = static_cast<float>(state.loss_sum); values[1] = static_cast<float>(state.class_loss_sum); values[2] = static_cast<float>(state.box_loss_sum); values[6] = 1;
  mmltk::frameworks::reflection::visit_materialized_members<TrainingScalars>([&]<class Declaration>(const auto&) {
   constexpr auto index = mmltk::frameworks::reflection::member_index<Declaration::pointer>(mmltk::frameworks::reflection::field_declarations<TrainingScalars>());
   values[10 + index] = static_cast<float>((state.scalar_sums.*Declaration::pointer).value_or(std::numeric_limits<double>::quiet_NaN()));
  });
  device_values_.copy_(host_values_, false); epoch_state_ = state;
 }
 const TrainingEpochMetricState& state() const noexcept { return epoch_state_; }
 void begin_attempt(std::size_t contributions) {
  attempt_values_.zero_(); control_.zero_(); contribution_ = 0;
  if (!diagnostics_.defined() || diagnostics_.size(0) != static_cast<std::int64_t>(contributions)) diagnostics_ = torch::zeros({static_cast<std::int64_t>(contributions), 4}, device_values_.options());
  else diagnostics_.zero_();
 }
 void accumulate_empty() { ++contribution_; }
 void accumulate(const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const scalar_packet::Tensors& scalars, const TrainingDiagnosticTensors& diagnostics) {
  for (std::size_t i = 0; i < scalar_packet::size; ++i) scalar_sources_[i] = scalars[i].defined() ? scalars[i] : unavailable_;
  at::stack_out(scalar_values_, scalar_sources_);
  attempt_values_.narrow(0, 3, scalar_packet::size).add_(scalar_values_);
  for (std::size_t i = 0; i < scalar_packet::size; ++i) if (scalars[i].defined()) attempt_values_.select(0, 3 + scalar_packet::size + i).fill_(1);
  for (std::size_t i = 0; i < diagnostics.size(); ++i) if (diagnostics[i].defined()) diagnostics_.select(0, contribution_).select(0, i).copy_(diagnostics[i].detach());
  ++contribution_;
  const auto detached_loss = loss.detach();
  const auto detached_class_loss = class_loss.detach();
  const auto detached_box_loss = box_loss.detach();
  attempt_values_.select(0, 0).add_(detached_loss);
  attempt_values_.select(0, 1).add_(detached_class_loss);
  attempt_values_.select(0, 2).add_(detached_box_loss);
  control_.select(0, 0).add_(torch::isfinite(detached_loss).logical_not().to(torch::kFloat32));
 }
 TrainingMetricSnapshot complete_step(const torch::Tensor& found_inf, int64_t wave_micro_batches, int64_t epoch_micro_batches, const DistributedContext& distributed) {
  control_.select(0, 1).copy_(found_inf);
  distributed_all_reduce_tensor(distributed, control_);
  device_values_.select(0, 6).copy_(control_.select(0, 0).eq(0));
  device_values_.select(0, 7).copy_(control_.select(0, 1));
  distributed_all_reduce_tensor(distributed, attempt_values_);
  distributed_all_reduce_tensor(distributed, diagnostics_);
  device_values_.narrow(0, 0, 3).add_(attempt_values_.narrow(0, 0, 3));
  device_values_.narrow(0, 3, 3).copy_(attempt_values_.narrow(0, 0, 3));
  auto scalar_sums = attempt_values_.narrow(0, 3, scalar_packet::size);
  const auto available = attempt_values_.narrow(0, 3 + scalar_packet::size, scalar_packet::size).gt(0);
  constexpr auto class_error = scalar_packet::index<^^TrainingScalars::class_error>();
  constexpr auto cardinality = scalar_packet::index<^^TrainingScalars::cardinality_error>();
  scalar_sums.select(0, class_error).copy_(torch::where(diagnostics_.select(1, 1).gt(0), diagnostics_.select(1, 0) / diagnostics_.select(1, 1).clamp_min(1), 100.0).sum());
  scalar_sums.select(0, cardinality).copy_((diagnostics_.select(1, 2) / diagnostics_.select(1, 3).clamp_min(1)).sum());
  device_values_.narrow(0, 10, scalar_packet::size).add_(torch::where(available, scalar_sums, std::numeric_limits<float>::quiet_NaN()));
  const auto* values = complete_values();
  const double wave_divisor = static_cast<double>(std::max<int64_t>(1, wave_micro_batches));
  TrainingMetricSnapshot snapshot;
  snapshot.scalars = scalar_packet::project(values + 10, static_cast<double>(epoch_micro_batches));
  snapshot.loss_sum = static_cast<double>(values[0]);
  snapshot.class_loss_sum = static_cast<double>(values[1]);
  snapshot.box_loss_sum = static_cast<double>(values[2]);
  snapshot.step_loss = static_cast<double>(values[3]) / wave_divisor;
  snapshot.step_class_loss = static_cast<double>(values[4]) / wave_divisor;
  snapshot.step_box_loss = static_cast<double>(values[5]) / wave_divisor;
  snapshot.loss_finite = values[6] != 0.0f;
  snapshot.gradients_finite = values[7] == 0.0f;
  epoch_state_ = {static_cast<std::uint64_t>(epoch_micro_batches), snapshot.loss_sum, snapshot.class_loss_sum, snapshot.box_loss_sum, scalar_packet::project(values + 10, 1)};
  device_values_.select(0, 6).fill_(1.0f);
  device_values_.select(0, 7).zero_();
  return snapshot;
 }
 [[nodiscard]] torch::Tensor loss_sum() const { return device_values_.select(0, 0); }
 [[nodiscard]] torch::Tensor epoch_count(std::int64_t count) {
  auto value = device_values_.select(0, 9);
  value.fill_(static_cast<float>(count));
  return value;
 }
 [[nodiscard]] double epoch_average() {
  const auto* values = complete_values();
  return static_cast<double>(values[0]) / std::max(1.0, static_cast<double>(values[9]));
 }
 void begin_validation() { device_values_.select(0, 8).zero_(); }
 void accumulate_validation(const torch::Tensor& loss) { device_values_.select(0, 8).add_(loss.detach()); }
 [[nodiscard]] double validation_average(std::size_t count) {
  const auto* values = complete_values();
  return count ? static_cast<double>(values[8]) / static_cast<double>(count) : 0.0;
 }

private:
 const float* complete_values() {
  host_values_.copy_(device_values_, true);
  const auto device_index = torch_cuda::checked_device_index(device_id_);
  const auto stream = torch_cuda::current_torch_cuda_stream_object(device_index);
  auto completion = event_pool_.record(reinterpret_cast<std::uintptr_t>(stream.stream()), "record training metric handoff");
  if (!completion) { throw std::runtime_error("training metric event pool is unavailable"); }
  completion->wait(reinterpret_cast<std::uintptr_t>(settlement_stream_), "wait for training metric handoff");
  ensure_cuda_ok(cudaStreamSynchronize(settlement_stream_), "synchronize training metric settlement stream");
  completion->retire();
  return host_values_.data_ptr<float>();
 }
 TrainingEpochMetricState epoch_state_;
 int device_id_ = 0;
 torch::Tensor device_values_;
 torch::Tensor host_values_;
 torch::Tensor scalar_values_;
 torch::Tensor unavailable_;
 torch::Tensor attempt_values_, control_, diagnostics_;
 std::int64_t contribution_ = 0;
 scalar_packet::Tensors scalar_sources_;
 std::atomic<cudaError_t> first_failure_{cudaSuccess};
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_owner_{1U};
 mmltk::backend::ml::cuda::CudaEventPool event_pool_;
 cudaStream_t settlement_stream_ = nullptr;
};
TrainingMetricHandoff::TrainingMetricHandoff(int device_id) : impl_(std::make_unique<Impl>(device_id)) {}
TrainingMetricHandoff::~TrainingMetricHandoff() = default;
void TrainingMetricHandoff::reset_epoch() { impl_->reset_epoch(); }
void TrainingMetricHandoff::restore_epoch(const TrainingEpochMetricState& state) { impl_->restore_epoch(state); }
const TrainingEpochMetricState& TrainingMetricHandoff::state() const { return impl_->state(); }
void TrainingMetricHandoff::begin_attempt(std::size_t contributions) { impl_->begin_attempt(contributions); }
void TrainingMetricHandoff::accumulate_empty() { impl_->accumulate_empty(); }
void TrainingMetricHandoff::accumulate(const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const scalar_packet::Tensors& scalars, const TrainingDiagnosticTensors& diagnostics) {
 impl_->accumulate(loss, class_loss, box_loss, scalars, diagnostics);
}
TrainingMetricSnapshot TrainingMetricHandoff::complete_step(const torch::Tensor& found_inf, int64_t wave_micro_batches, int64_t epoch_micro_batches, const DistributedContext& distributed) {
 return impl_->complete_step(found_inf, wave_micro_batches, epoch_micro_batches, distributed);
}
torch::Tensor TrainingMetricHandoff::loss_sum() const { return impl_->loss_sum(); }
torch::Tensor TrainingMetricHandoff::epoch_count(std::int64_t count) { return impl_->epoch_count(count); }
double TrainingMetricHandoff::epoch_average() { return impl_->epoch_average(); }
void TrainingMetricHandoff::begin_validation() { impl_->begin_validation(); }
void TrainingMetricHandoff::accumulate_validation(const torch::Tensor& loss) { impl_->accumulate_validation(loss); }
double TrainingMetricHandoff::validation_average(std::size_t count) { return impl_->validation_average(count); }
}  // namespace mmltk::backend::models::rfdetr
