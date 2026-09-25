#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "evaluation_metrics.h"
#include "execution_plan.h"
#include "workflow_requests.h"
namespace mmltk::backend::models::rfdetr {
inline constexpr std::uint32_t kTrainingSessionFormat = 1;
inline constexpr std::uint32_t kTrainingSelectionFormat = 1;
inline constexpr std::size_t kTrainingSelectionBytes = 64U * 1024U;
enum class EvaluatedWeights : std::uint8_t { Ordinary, Ema, Soup };
enum class TrainingRecordScope : std::uint8_t { Model, SynchronizedSession, SelectedOutput, Session };
enum class TrainingPrecisionKind : std::uint8_t { Float32, Float16, BFloat16 };
enum class TrainingMergeBoundary : std::uint8_t { Rounds, Epoch, Terminal };
struct TrainingArtifact final {
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string session_id;
 std::uint64_t model_id = 0;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string initialization;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string configuration;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string content;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string sha256;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path path;
 std::uint64_t epoch = 0;
 std::uint64_t attempt = 0;
 std::uint64_t merge = 0;
 EvaluatedWeights weights = EvaluatedWeights::Ordinary;
 std::optional<double> selection_metric;
 std::optional<EvalSummary> evaluation;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string validation;
 bool operator==(const TrainingArtifact&) const = default;
};
struct TrainingIngredient final {
 std::uint64_t model_id = 0;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string sha256;
 [[= mmltk::frameworks::reflection::Finite{}]] double coefficient = 0;
 bool operator==(const TrainingIngredient&) const = default;
};
struct TrainingSelection final {
 std::uint32_t format_version = kTrainingSelectionFormat;
 TrainFinalPolicy method = TrainFinalPolicy::Off;
 TrainingArtifact artifact;
 [[= mmltk::frameworks::reflection::MaxItems{kMaximumTrainingModels}]] std::vector<TrainingIngredient> ingredients;
 EvalSummary validation;
 double best_individual_metric = 0;
 bool operator==(const TrainingSelection&) const = default;
};
struct TrainingModelFile final {
 std::uint64_t model_id = 0;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path path;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string sha256;
 std::uint64_t successful_images = 0;
 std::optional<TrainingArtifact> best;
 bool operator==(const TrainingModelFile&) const = default;
};
struct TrainingSessionManifest final {
 std::uint32_t format_version = kTrainingSessionFormat;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string session_id;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string attempt_id;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string generation;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string previous_generation;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string initialization;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string configuration;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string validation;
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string plan_sha256;
 std::uint64_t epoch = 0;
 std::uint64_t round = 0;
 std::uint64_t merge = 0;
 std::uint64_t interval_rounds = 0;
 TrainingPrecisionKind precision = TrainingPrecisionKind::Float32;
 TrainRequest request;
 [[= mmltk::frameworks::reflection::MaxItems{kMaximumTrainingModels}]] std::vector<TrainingModelFile> models;
 bool operator==(const TrainingSessionManifest&) const = default;
};
struct TrainingFailure final {
 [[= mmltk::frameworks::reflection::MaxBytes{64}]] std::string session_id;
 std::uint64_t model_id = 0;
 std::uint64_t first_cause = 0;
 [[= mmltk::frameworks::reflection::MaxBytes{65536}]] std::string detail;
 bool operator==(const TrainingFailure&) const = default;
};
[[nodiscard]] double training_selection_metric(const EvalSummary&, bool masks);
void validate_training_selection(const TrainingSelection&);
void validate_training_session_manifest(const TrainingSessionManifest&);
MMLTK_REFLECT_ENUM(EvaluatedWeights)
MMLTK_REFLECT_ENUM(TrainingRecordScope)
MMLTK_REFLECT_ENUM(TrainingMergeBoundary)
MMLTK_REFLECT_ENUM(TrainingPrecisionKind)
MMLTK_REFLECT_FIELDS(TrainingArtifact)
MMLTK_REFLECT_FIELDS(TrainingIngredient)
MMLTK_REFLECT_FIELDS(TrainingSelection)
MMLTK_REFLECT_FIELDS(TrainingModelFile)
MMLTK_REFLECT_FIELDS(TrainingSessionManifest)
MMLTK_REFLECT_FIELDS(TrainingFailure)
}  // namespace mmltk::backend::models::rfdetr
