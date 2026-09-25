#pragma once
#include "src/backend/models/rfdetr/core/detection_statistics.h"
#include <memory>
#include <cstdint>
#include <cstddef>
#include <torch/types.h>
#include "training_scalar_packet.h"
#include "training_metric_state.h"
#include "training_distributed.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingMetricSnapshot {
 double loss_sum = 0.0;
 double class_loss_sum = 0.0;
 double box_loss_sum = 0.0;
 double step_loss = 0.0;
 double step_class_loss = 0.0;
 double step_box_loss = 0.0;
 TrainingScalars scalars;
 bool loss_finite = true;
 bool gradients_finite = true;
};
class TrainingMetricHandoff final {
public:
 explicit TrainingMetricHandoff(int device_id);
 ~TrainingMetricHandoff();
 TrainingMetricHandoff(const TrainingMetricHandoff&) = delete;
 TrainingMetricHandoff& operator=(const TrainingMetricHandoff&) = delete;
 void reset_epoch();
 void restore_epoch(const TrainingEpochMetricState&);
 [[nodiscard]] const TrainingEpochMetricState& state() const;
 void begin_attempt(std::size_t contributions);
 void accumulate_empty();
 void accumulate(const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const TrainingScalarPacket::Tensors& scalars, const DetectionStatisticsPacket::Tensors& statistics = {});
 TrainingMetricSnapshot complete_step(const torch::Tensor& found_inf, int64_t attempt_micro_batches, int64_t epoch_micro_batches, const DistributedContext& distributed = {});
 [[nodiscard]] double epoch_average() const;
 void begin_validation();
 void accumulate_validation(const torch::Tensor& loss);
 double validation_average(std::size_t count);

private:
 friend struct testsupport::TrainingDistributedTestAccess;
 void retire() noexcept;
 struct Impl;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement_{1U};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_);
 std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
