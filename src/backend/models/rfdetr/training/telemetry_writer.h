#pragma once
#include <memory>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::backend::models::rfdetr {
// Preparation precedes run/optimizer admission and therefore carries no metrics.
// Only rank zero publishes each stage transition.
class TrainingPreparationWriter final {
public:
 explicit TrainingPreparationWriter(const TrainRequest&);
 void Stage(TrainingPreparationStage);

private:
 void Publish() noexcept;
 std::filesystem::path path_;
 TrainingPreparationProgress progress_;
};
// Serialization, file I/O and product failure reporting belong to its worker.
// Publication copies bounded CPU facts and never waits for queue or disk space.
class TrainingTelemetryWriter final {
public:
 explicit TrainingTelemetryWriter(TrainingRun);
 ~TrainingTelemetryWriter() noexcept;
 TrainingTelemetryWriter(const TrainingTelemetryWriter&) = delete;
 TrainingTelemetryWriter& operator=(const TrainingTelemetryWriter&) = delete;
 [[nodiscard]] const std::string& attempt_id() const noexcept;
 void Submit(TrainingMetricProgress, TrainingRecordRole) noexcept;
 void Finish(TrainingMetricProgress, TrainingFinalFacts) noexcept;
 void Fail(TrainingMetricProgress) noexcept;
 void Close() noexcept;
 [[nodiscard]] TrainingPersistence persistence() const;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
