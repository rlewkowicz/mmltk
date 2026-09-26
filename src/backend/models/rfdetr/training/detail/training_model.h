#pragma once
#include <memory>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <functional>
#include <exception>
#include <optional>
#include <vector>
#include "training_artifact.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::common::concurrency {
class WorkerPool;
}
namespace mmltk::backend::data {
class DatasetLoader;
}
namespace mmltk::backend::models::rfdetr {
class RuntimeContext;
class NativeRfDetrModel;
class TrainingDataPlan;
class TrainingValidationRuntime;
class TrainingSnapshotPublication;
struct DistributedContext;
struct TrainingPrecision;
struct DetectionConfig;
struct TrainingEpochDraws;
struct TrainingScheduleState;
struct EvalPassResult;
namespace detail {
struct TrainingContinuation;
}
// One trajectory owns all mutable training state. The session grants this owner
// a complete communication turn; no trajectory creates its own ordering protocol.
class TrainingModel final {
public:
 TrainingModel(TrainRequest, std::size_t model_index, RuntimeContext&, std::unique_ptr<mmltk::backend::data::DatasetLoader>, std::shared_ptr<NativeRfDetrModel>, const TrainingDataPlan&,
  DistributedContext, TrainingPrecision, DetectionConfig, std::function<void(std::uint64_t, std::exception_ptr)> failure);
 ~TrainingModel();
 TrainingModel(const TrainingModel&) = delete;
 TrainingModel& operator=(const TrainingModel&) = delete;
 void stage_resume(const DecodedNativeModelState&, const detail::TrainingContinuation&);
 void commit_resume();
 void start(std::shared_ptr<mmltk::common::concurrency::WorkerPool>);
 void begin_epoch(std::uint64_t epoch, TrainingEpochDraws);
 [[nodiscard]] bool exhausted() const;
 // Returns successful global image count; an overflow consumes the attempt but
 // contributes zero merge weight. The return boundary is physically drained.
 [[nodiscard]] std::uint64_t attempt();
 void end_epoch();
 // Starts after attempts/merges drain and lives through the session's last
 // synchronous archive callback. Temporary EMA evaluation uses frozen values.
 [[nodiscard]] TrainingSnapshotPublication begin_publication();
 [[nodiscard]] TrainingMetricProgress progress(TrainingPhase) const;
 [[nodiscard]] EvalPassResult evaluate(TrainingValidationRuntime&, EvaluatedWeights);
 [[nodiscard]] TrainingArtifactCandidate save_candidate(const NativeCheckpointMetadata&, const std::filesystem::path&, std::string_view session, std::string_view initialization,
  std::string_view configuration, std::string_view validation, std::uint64_t merge, EvaluatedWeights, const EvalSummary&);
 void remember_candidate(TrainingArtifactCandidate);
 void save_ordinary_epoch(const NativeCheckpointMetadata&, const std::filesystem::path&);
 [[nodiscard]] const std::optional<TrainingArtifactCandidate>& best() const;
 void save_resume(const std::filesystem::path&, const NativeCheckpointMetadata&, std::string_view attempt, const std::filesystem::path& original_descriptor);
 [[nodiscard]] NativeRfDetrModel& model();
 [[nodiscard]] const std::vector<NormalizedModelStateEntry>& ordinary() const;
 [[nodiscard]] std::uint64_t id() const;
 [[nodiscard]] const TrainingScheduleState& schedule() const;
 void ordinary_changed();

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::backend::models::rfdetr
