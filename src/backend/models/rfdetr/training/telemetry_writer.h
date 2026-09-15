#pragma once

#include <memory>
#include "src/backend/models/rfdetr/contract/training_metrics.h"

namespace mmltk::backend::models::rfdetr {
struct TrainingFinalFacts final {
    std::optional<double> best_regular;
    std::optional<double> best_ema;
    bool fallback = false;
    std::uint64_t history_size = 0;
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
    void Close() noexcept;
    [[nodiscard]] TrainingPersistence persistence() const;
   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
