#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "evaluation_metrics.h"
#include "class_layout.h"
#include "workflow_requests.h"
namespace mmltk::backend::models::rfdetr {
inline constexpr std::uint32_t kTrainingRunFormat = 2;
inline constexpr std::size_t kTrainingHistoryPageSize = 32;
inline constexpr std::size_t kTrainingRecordBytes = 512U * 1024U;
inline constexpr std::size_t kTrainingManifestBytes = 4U * 1024U * 1024U;
inline constexpr std::string_view kTrainingPersistenceFailureLine = "\nMMLTK_TRAIN_PERSISTENCE_FAILED_V1\n";
enum class EvaluatedWeights : std::uint8_t { Ordinary, Ema };
enum class TrainingPhase : std::uint8_t { Starting, Train, Validate, EpochComplete, Completed, Error };
enum class TrainingRecordRole : std::uint8_t { Live, Boundary, Epoch, Terminal };
MMLTK_REFLECT_ENUM(EvaluatedWeights)
MMLTK_REFLECT_ENUM(TrainingPhase)
MMLTK_REFLECT_ENUM(TrainingRecordRole)
// Hungarian components are raw main-output losses; Match-Free components are
// weighted main-output losses. TrainingRun.configuration selects the convention.
// Auxiliary and DN are separate weighted groups, excluded from main components.
// Dense correspondence is an explicitly overlapping diagnostic breakdown.
// Total alone represents the complete optimized objective; raw main components
// require their criterion coefficients before any comparison with weighted groups.
struct TrainingScalars final {
    bool operator==(const TrainingScalars&) const = default;
    std::optional<double> total;
    std::optional<double> classification;
    std::optional<double> l1;
    std::optional<double> giou;
    std::optional<double> mask_ce;
    std::optional<double> mask_dice;
    std::optional<double> auxiliary_weighted;
    std::optional<double> denoising_weighted;
    std::optional<double> correspondence_weighted;
    std::optional<double> class_error;
    std::optional<double> cardinality_error;
    std::optional<double> learning_rate;
    std::optional<double> learning_rate_min;
    std::optional<double> learning_rate_max;
    std::optional<double> images_per_second;
};
MMLTK_REFLECT_FIELDS(TrainingScalars)
struct TrainingMetricProgress final {
    bool operator==(const TrainingMetricProgress&) const = default;
    TrainingPhase phase = TrainingPhase::Starting;
    int epoch = 0;
    int total_epochs = 1;
    std::int64_t completed_batches = 0;
    std::int64_t total_batches = 0;
    std::uint64_t completed_images = 0;
    std::uint64_t total_images = 0;
    std::int64_t completed_waves = 0;
    std::int64_t optimizer_steps = 0;
    std::int64_t global_optimizer_step = 0;
    std::int64_t steps_per_epoch = 0;
    int train_lanes = 1;
    bool rank_local = true;
    double train_loss = 0.0;
    double class_loss = 0.0;
    double box_loss = 0.0;
    double step_loss = 0.0;
    double step_class_loss = 0.0;
    double step_box_loss = 0.0;
    double batches_per_second = 0.0;
    double images_per_second = 0.0;
    double elapsed_seconds = 0.0;
    TrainingScalars scalars{};
    std::optional<double> epoch_global_loss;
    std::optional<double> val_loss;
    std::optional<EvalSummary> val;
    std::optional<EvalSummary> test;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path checkpoint_path;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path full_checkpoint_path;
};
MMLTK_REFLECT_FIELDS(TrainingMetricProgress)
struct TrainingRecord final {
    bool operator==(const TrainingRecord&) const = default;
    std::uint32_t format_version = kTrainingRunFormat;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string run_id;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string attempt_id;
    std::uint64_t sequence = 0;
    std::uint64_t dropped_before = 0;
    TrainingRecordRole role = TrainingRecordRole::Live;
    EvaluatedWeights evaluated_weights = EvaluatedWeights::Ordinary;
    TrainingMetricProgress progress{};
    std::optional<TrainRequest> attempt_configuration;
};
MMLTK_REFLECT_FIELDS(TrainingRecord)
struct TrainingDatasetLimits final {
    std::uint32_t train_max_instances = 0;
    std::uint32_t val_max_instances = 0;
    std::optional<std::uint32_t> test_max_instances;
    std::uint32_t largest_max_instances = 0;
    std::size_t resolved_num_queries = 0;
    std::size_t required_num_queries = 0;
    std::size_t automatic_num_queries_cap = 0;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string query_source;
    bool requested_override = false;
    bool automatic = false;
};
MMLTK_REFLECT_FIELDS(TrainingDatasetLimits)
struct TrainingExecutionFacts final {
    int eval_lanes = 1;
    std::size_t effective_batch_per_rank = 1;
    std::size_t effective_batch_global = 1;
    TrainingDatasetLimits dataset_limits;
};
MMLTK_REFLECT_FIELDS(TrainingExecutionFacts)
struct TrainingRun final {
    std::uint32_t format_version = kTrainingRunFormat;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string run_id;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string attempt_id;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string checkpoint_attempt_id;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string source_checkpoint_attempt_id;
    TrainRequest configuration{};
    TrainingExecutionFacts execution;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string original_weights;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path original_class_descriptor;
    EvaluatedWeights evaluated_weights = EvaluatedWeights::Ordinary;
    ModelClassLayout class_layout{};
    int resume_epoch = -1;
    std::uint64_t resume_optimizer_step = 0;
};
MMLTK_REFLECT_FIELDS(TrainingRun)
struct TrainingOpenedRun final {
    std::uint64_t generation = 0;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path directory;
    std::optional<TrainingRun> run;
};
MMLTK_REFLECT_FIELDS(TrainingOpenedRun)
struct TrainingPersistence final {
    bool operator==(const TrainingPersistence&) const = default;
    bool degraded = false;
    std::uint64_t dropped_records = 0;
    [[= mmltk::frameworks::reflection::MaxBytes{1024}]] std::string error;
};
MMLTK_REFLECT_FIELDS(TrainingPersistence)
struct TrainingCheckpoint final {
    bool operator==(const TrainingCheckpoint&) const = default;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path path;
    [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string attempt_id;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string original_weights;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path original_class_descriptor;
    bool resumable = false;
    int epoch = -1;
    std::optional<TrainRequest> configuration;
    std::optional<ModelClassLayout> class_layout;
    EvaluatedWeights evaluated_weights = EvaluatedWeights::Ordinary;
};
MMLTK_REFLECT_FIELDS(TrainingCheckpoint)
enum class TrainingInspectionStatus : std::uint8_t { Idle, Running, Ready, Failed, Cancelled };
MMLTK_REFLECT_ENUM(TrainingInspectionStatus)
struct TrainingCheckpointInspection final {
    bool operator==(const TrainingCheckpointInspection&) const = default;
    std::uint64_t generation = 0;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path path;
    TrainingInspectionStatus status = TrainingInspectionStatus::Idle;
    std::optional<TrainingCheckpoint> checkpoint;
    [[= mmltk::frameworks::reflection::MaxBytes{1024}]] std::string error;
};
MMLTK_REFLECT_FIELDS(TrainingCheckpointInspection)
struct TrainingDirectoryQuery final {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path directory;
};
MMLTK_REFLECT_FIELDS(TrainingDirectoryQuery)
struct TrainingCheckpointQuery final {
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path path;
};
MMLTK_REFLECT_FIELDS(TrainingCheckpointQuery)
struct TrainingHistoryQuery final {
    std::uint64_t generation = 0;
    std::uint64_t cursor = 0;
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        1}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{kTrainingHistoryPageSize}]] std::uint32_t count = kTrainingHistoryPageSize;
};
MMLTK_REFLECT_FIELDS(TrainingHistoryQuery)
struct TrainingHistoryPage final {
    std::uint64_t generation = 0;
    std::uint64_t next_cursor = 0;
    bool more = false;
    [[= mmltk::frameworks::reflection::MaxItems{kTrainingHistoryPageSize}]] std::vector<TrainingRecord> records;
};
MMLTK_REFLECT_FIELDS(TrainingHistoryPage)
}  // namespace mmltk::backend::models::rfdetr
