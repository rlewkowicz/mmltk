#include "src/backend/ml/cuda/torch_cuda_utils.h"
#include "detail/training_metrics.h"
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/frameworks/gpu/cuda/terminal_cuda_retirement_owner.h"
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace torch_cuda = mmltk::backend::ml::cuda;
struct TrainingMetricHandoff::Impl {
 static constexpr std::int64_t scalar_offset = 9;
 static constexpr std::int64_t packet_size = scalar_offset + static_cast<std::int64_t>(TrainingScalarPacket::size);
 static constexpr std::int64_t attempt_size = 3 + 2 * static_cast<std::int64_t>(TrainingScalarPacket::size);
 static constexpr std::int64_t control_size = 2;

public:
 explicit Impl(int device_id) : device_id_(device_id), launch_(torch_cuda::getCurrentCUDAStream(torch_cuda::checked_device_index(device_id))), work_(device_id) {}
 void initialize() {
  const auto device_options = torch::TensorOptions().dtype(torch::kFloat32).device(mmltk::backend::ml::cuda::cuda_device(device_id_));
  device_values_ = torch::zeros({packet_size}, device_options);
  device_values_.select(0, 6).fill_(1.0f);
  scalar_values_ = torch::empty({static_cast<int64_t>(TrainingScalarPacket::size)}, device_options);
  unavailable_ = torch::zeros({}, device_options);
  host_values_ = torch_cuda::numa_empty({packet_size}, torch::kFloat32, device_id_);
 }
 cudaError_t retire() noexcept {
  if (failure_ != cudaSuccess || work_.retire() != cudaSuccess) return cudaErrorUnknown;
  try {
   launch_.synchronize();
   return cudaSuccess;
  } catch (...) { return cudaErrorUnknown; }
 }
 void check() const {
  if (failure_ != cudaSuccess || work_.uncertain()) throw std::runtime_error("training metrics cannot reuse unproved device work");
 }
 void reset_epoch() {
  check();
  device_values_.zero_();
  device_values_.select(0, 6).fill_(1.0f);
  epoch_state_ = {};
 }
 void restore_epoch(const TrainingEpochMetricState& state) {
  reset_epoch();
  if (!state.microbatches) return;
  host_values_.zero_();
  auto* values = host_values_.data_ptr<float>();
  values[0] = static_cast<float>(state.loss_sum);
  values[1] = static_cast<float>(state.class_loss_sum);
  values[2] = static_cast<float>(state.box_loss_sum);
  values[6] = 1;
  TrainingScalarPacket::visit(
   [&]<auto Member, std::size_t Index>() { values[scalar_offset + Index] = static_cast<float>((state.scalar_sums.*Member).value_or(std::numeric_limits<double>::quiet_NaN())); });
  device_values_.copy_(host_values_, false);
  epoch_state_ = state;
 }
 const TrainingEpochMetricState& state() const noexcept { return epoch_state_; }
 void begin_attempt(std::size_t contributions) {
  check();
  constexpr auto maximum_rows = (std::numeric_limits<std::int64_t>::max() - control_size - attempt_size) / static_cast<std::int64_t>(DetectionStatisticsPacket::size);
  if (contributions > static_cast<std::size_t>(maximum_rows)) throw std::overflow_error("training metric contribution extent overflow");
  const auto rows = static_cast<std::int64_t>(contributions);
  if (!statistics_.defined() || statistics_.size(0) != rows) {
   const auto size = control_size + attempt_size + rows * static_cast<std::int64_t>(DetectionStatisticsPacket::size);
   if (!reduction_storage_.defined() || reduction_storage_.numel() < size) reduction_storage_ = torch::empty({size}, device_values_.options());
   reduction_values_ = reduction_storage_.narrow(0, 0, size);
   control_ = reduction_values_.narrow(0, 0, control_size);
   attempt_values_ = reduction_values_.narrow(0, control_size, attempt_size);
   statistics_ = reduction_values_.narrow(0, control_size + attempt_size, rows * static_cast<std::int64_t>(DetectionStatisticsPacket::size))
                  .view({rows, static_cast<std::int64_t>(DetectionStatisticsPacket::size)});
  }
  reduction_values_.zero_();
  contribution_ = 0;
 }
 void accumulate_empty() { ++contribution_; }
 void accumulate(
  const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const TrainingScalarPacket::Tensors& scalars, const DetectionStatisticsPacket::Tensors& statistics) {
  for (std::size_t i = 0; i < TrainingScalarPacket::size; ++i) scalar_sources_[i] = scalars[i].defined() ? scalars[i] : unavailable_;
  at::stack_out(scalar_values_, scalar_sources_);
  attempt_values_.narrow(0, 3, TrainingScalarPacket::size).add_(scalar_values_);
  for (std::size_t i = 0; i < TrainingScalarPacket::size; ++i)
   if (scalars[i].defined()) attempt_values_.select(0, 3 + TrainingScalarPacket::size + i).fill_(1);
  for (std::size_t i = 0; i < statistics.size(); ++i)
   if (statistics[i].defined()) statistics_.select(0, contribution_).select(0, i).copy_(statistics[i].detach());
  ++contribution_;
  const auto detached_loss = loss.detach();
  const auto detached_class_loss = class_loss.detach();
  const auto detached_box_loss = box_loss.detach();
  attempt_values_.select(0, 0).add_(detached_loss);
  attempt_values_.select(0, 1).add_(detached_class_loss);
  attempt_values_.select(0, 2).add_(detached_box_loss);
  control_.select(0, 0).add_(torch::isfinite(detached_loss).logical_not().to(torch::kFloat32));
 }
 TrainingMetricSnapshot complete_step(const torch::Tensor& found_inf, int64_t attempt_micro_batches, int64_t epoch_micro_batches, const DistributedContext& distributed) {
  check();
  control_.select(0, 1).copy_(found_inf);
  // All controls, sums, availability flags and per-microbatch sufficient
  // statistics use the same FP32 SUM. Keep row ratios after this single SUM.
  if (distributed.enabled) work_.join(work_.all_reduce(distributed, reduction_values_));
  device_values_.select(0, 6).copy_(control_.select(0, 0).eq(0));
  device_values_.select(0, 7).copy_(control_.select(0, 1));
  device_values_.narrow(0, 0, 3).add_(attempt_values_.narrow(0, 0, 3));
  device_values_.narrow(0, 3, 3).copy_(attempt_values_.narrow(0, 0, 3));
  auto scalar_sums = attempt_values_.narrow(0, 3, TrainingScalarPacket::size);
  const auto available = attempt_values_.narrow(0, 3 + TrainingScalarPacket::size, TrainingScalarPacket::size).gt(0);
  // Each row is a global logical microbatch, including zero local slices.
  // Divide after rank SUM, then sum ratios before the temporal mean.
  const auto matched = DetectionStatisticsPacket::select<^^DetectionSufficientStatistics::matched_count>(statistics_, 1);
  const auto class_errors = DetectionStatisticsPacket::select<^^DetectionSufficientStatistics::class_error_sum>(statistics_, 1);
  const auto images = DetectionStatisticsPacket::select<^^DetectionSufficientStatistics::image_count>(statistics_, 1);
  const auto cardinality_errors = DetectionStatisticsPacket::select<^^DetectionSufficientStatistics::cardinality_error_sum>(statistics_, 1);
  TrainingScalarPacket::select<^^TrainingScalars::class_error>(scalar_sums, 0).copy_(torch::where(matched.gt(0), class_errors / matched.clamp_min(1), 100.0).sum());
  TrainingScalarPacket::select<^^TrainingScalars::cardinality_error>(scalar_sums, 0).copy_((cardinality_errors / images.clamp_min(1)).sum());
  device_values_.narrow(0, scalar_offset, TrainingScalarPacket::size).add_(torch::where(available, scalar_sums, std::numeric_limits<float>::quiet_NaN()));
  const auto* values = complete_values();
  const double attempt_divisor = static_cast<double>(std::max<int64_t>(1, attempt_micro_batches));
  TrainingMetricSnapshot snapshot;
  snapshot.scalars = TrainingScalarPacket::project(values + scalar_offset, static_cast<double>(epoch_micro_batches));
  snapshot.loss_sum = static_cast<double>(values[0]);
  snapshot.class_loss_sum = static_cast<double>(values[1]);
  snapshot.box_loss_sum = static_cast<double>(values[2]);
  snapshot.step_loss = static_cast<double>(values[3]) / attempt_divisor;
  snapshot.step_class_loss = static_cast<double>(values[4]) / attempt_divisor;
  snapshot.step_box_loss = static_cast<double>(values[5]) / attempt_divisor;
  snapshot.loss_finite = values[6] != 0.0f;
  snapshot.gradients_finite = values[7] == 0.0f;
  epoch_state_ = {static_cast<std::uint64_t>(epoch_micro_batches), snapshot.loss_sum, snapshot.class_loss_sum, snapshot.box_loss_sum, TrainingScalarPacket::project(values + scalar_offset, 1)};
  device_values_.select(0, 6).fill_(1.0f);
  device_values_.select(0, 7).zero_();
  return snapshot;
 }
 [[nodiscard]] double epoch_average() const { return epoch_state_.microbatches ? epoch_state_.loss_sum / static_cast<double>(epoch_state_.microbatches) : 0.0; }
 void begin_validation() {
  check();
  device_values_.select(0, 8).zero_();
 }
 void accumulate_validation(const torch::Tensor& loss) { device_values_.select(0, 8).add_(loss.detach()); }
 [[nodiscard]] double validation_average(std::size_t count) {
  const auto* values = complete_values();
  return count ? static_cast<double>(values[8]) / static_cast<double>(count) : 0.0;
 }

private:
 const float* complete_values() {
  try {
   host_values_.copy_(device_values_, true);
   // This event settles the complete metric pipeline, including its pinned
   // host destination. A stream join alone never makes that storage reclaimable.
   work_.settle();
   return host_values_.data_ptr<float>();
  } catch (...) {
   failure_ = cudaErrorUnknown;
   throw;
  }
 }
 TrainingEpochMetricState epoch_state_;
 int device_id_ = 0;
 torch::Tensor device_values_;
 torch::Tensor host_values_;
 torch::Tensor scalar_values_;
 torch::Tensor unavailable_;
 torch::Tensor reduction_storage_, reduction_values_, attempt_values_, control_, statistics_;
 std::int64_t contribution_ = 0;
 TrainingScalarPacket::Tensors scalar_sources_;
 torch_cuda::TorchCudaStream launch_;
 TrainingCollectiveWork work_;
 cudaError_t failure_ = cudaSuccess;
};
TrainingMetricHandoff::TrainingMetricHandoff(int device_id) : impl_(std::make_shared<Impl>(device_id)) {
 try {
  impl_->initialize();
  // CLEANUP-IGNORE: Constructor rollback retires this metric pipeline; adjacent owner boilerplate settles different resources in other classes.
 } catch (...) {
  retire();
  throw;
 }
}
TrainingMetricHandoff::~TrainingMetricHandoff() { retire(); }
void TrainingMetricHandoff::retire() noexcept {
 if (!impl_) return;
 const auto status = impl_->retire();
 // CLEANUP-IGNORE: Shared terminal custody is already implemented by Install; metric and gradient retirement establish different physical completion proofs.
 if (status != cudaSuccess)
  std::move(terminal_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), status);
 else
  impl_.reset();
}
void TrainingMetricHandoff::reset_epoch() { impl_->reset_epoch(); }
void TrainingMetricHandoff::restore_epoch(const TrainingEpochMetricState& state) { impl_->restore_epoch(state); }
const TrainingEpochMetricState& TrainingMetricHandoff::state() const { return impl_->state(); }
void TrainingMetricHandoff::begin_attempt(std::size_t contributions) { impl_->begin_attempt(contributions); }
void TrainingMetricHandoff::accumulate_empty() { impl_->accumulate_empty(); }
void TrainingMetricHandoff::accumulate(
 const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const TrainingScalarPacket::Tensors& scalars, const DetectionStatisticsPacket::Tensors& statistics) {
 impl_->accumulate(loss, class_loss, box_loss, scalars, statistics);
}
TrainingMetricSnapshot TrainingMetricHandoff::complete_step(const torch::Tensor& found_inf, int64_t attempt_micro_batches, int64_t epoch_micro_batches, const DistributedContext& distributed) {
 return impl_->complete_step(found_inf, attempt_micro_batches, epoch_micro_batches, distributed);
}
double TrainingMetricHandoff::epoch_average() const { return impl_->epoch_average(); }
void TrainingMetricHandoff::begin_validation() { impl_->begin_validation(); }
void TrainingMetricHandoff::accumulate_validation(const torch::Tensor& loss) { impl_->accumulate_validation(loss); }
double TrainingMetricHandoff::validation_average(std::size_t count) { return impl_->validation_average(count); }
}  // namespace mmltk::backend::models::rfdetr
