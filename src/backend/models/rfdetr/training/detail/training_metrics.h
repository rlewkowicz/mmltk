#pragma once
#include <memory>
#include <cstdint>
#include "training_scalar_packet.h"
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
    void begin_wave();
    void accumulate(const torch::Tensor& loss, const torch::Tensor& class_loss, const torch::Tensor& box_loss, const scalar_packet::Tensors& scalars);
    TrainingMetricSnapshot complete_step(const torch::Tensor& found_inf, int64_t wave_micro_batches, int64_t epoch_micro_batches);
    torch::Tensor loss_sum() const;
    torch::Tensor epoch_count(std::int64_t count);
    double epoch_average();
    void begin_validation();
    void accumulate_validation(const torch::Tensor& loss);
    double validation_average(std::size_t count);
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
