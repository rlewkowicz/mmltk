#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
inline constexpr std::size_t kMaximumTrainingModels = 16;
inline constexpr std::size_t kMaximumTrainRequestJsonBytes = 64U * 1024U;
enum class TrainLaneMode : std::uint8_t { SharedGradients, Independent, PeriodicAveraging };
enum class TrainBalancing : std::uint8_t { Off, Stratified, RareRepeatsStratified };
enum class TrainMergeCadence : std::uint8_t { Epoch, Rounds };
enum class TrainFinalPolicy : std::uint8_t { Off, Uniform, Explicit, ValidationGreedy };
struct TrainModelSettings final {
 MMLTK_MINIMUM(std::uint64_t, 1) std::uint64_t model_id = 1;
 std::uint64_t seed = 42;
 TrainRecipeSettings recipe;
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE double coefficient = 1;
 bool operator==(const TrainModelSettings&) const = default;
};
struct TrainLaneConfiguration final {
 TrainLaneMode mode = TrainLaneMode::SharedGradients;
 MMLTK_MAX_ITEMS(kMaximumTrainingModels) std::vector<TrainModelSettings> models;
 MMLTK_MINIMUM(std::uint64_t, 1) std::uint64_t next_model_id = 1;
 TrainMergeCadence merge_cadence = TrainMergeCadence::Epoch;
 MMLTK_MINIMUM(std::uint64_t, 1) std::uint64_t merge_rounds = 1;
 std::optional<TrainFinalPolicy> final_policy;
 bool operator==(const TrainLaneConfiguration&) const = default;
};
struct TrainDataPolicy final {
 TrainBalancing balancing = TrainBalancing::Off;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE double rare_threshold = .001;
 MMLTK_MINIMUM(double, 1.0) MMLTK_MAXIMUM(double, 64.0) MMLTK_FINITE double maximum_repeat_factor = 10;
 MMLTK_MINIMUM(double, 1.0) MMLTK_MAXIMUM(double, 16.0) MMLTK_FINITE double maximum_draw_multiplier = 4;
 bool operator==(const TrainDataPolicy&) const = default;
};
enum class ExecutionLimitation : std::uint8_t { None, ExperimentalIndependentModels, ExperimentalPeriodicAveraging, SourceCapacity, BackendCapacity };
struct ExecutionFacts final {
 std::uint64_t settings_revision = 0;
 std::uint64_t operation_generation = 0;
 std::uint64_t logical_models = 1;
 std::uint64_t microbatches_per_attempt = 1;
 std::uint64_t effective_batch_per_model = 1;
 std::uint64_t aggregate_round_images = 1;
 std::uint64_t configured_capacity = 1;
 std::uint64_t admitted_capacity = 0;
 ExecutionLimitation limitation = ExecutionLimitation::None;
 bool operator==(const ExecutionFacts&) const = default;
};
struct TrainRequest;
struct ValidateRequest;
struct PredictRequest;
struct TrainingBatchDistribution final {
 std::uint64_t ranks = 1;
 std::uint64_t minimum_rank_batch = 1;
 std::uint64_t maximum_rank_batch = 1;
 bool operator==(const TrainingBatchDistribution&) const = default;
};
MMLTK_REFLECT_FIELDS(TrainingBatchDistribution)
[[nodiscard]] TrainingBatchDistribution derive_training_batch_distribution(const TrainRequest&);
struct TrainingRankSlice final {
 std::uint64_t begin = 0;
 std::uint64_t count = 0;
};
[[nodiscard]] TrainingRankSlice training_rank_slice(std::uint64_t batch, std::uint32_t rank, std::uint32_t world);
[[nodiscard]] std::uint64_t checked_training_product(std::uint64_t, std::uint64_t);
[[nodiscard]] std::uint64_t training_stochastic_key(std::uint64_t seed, std::uint64_t model, std::uint64_t epoch, std::uint64_t occurrence, std::uint64_t purpose = 0) noexcept;
void resize_training_models(TrainLaneConfiguration&, std::size_t count, const TrainRecipeSettings&, std::uint64_t session_seed);
void validate_training_configuration(const TrainRequest&);
[[nodiscard]] inline constexpr TrainFinalPolicy effective_final_policy(const TrainLaneConfiguration& configuration) noexcept {
 return configuration.final_policy.value_or(configuration.mode == TrainLaneMode::Independent ? TrainFinalPolicy::ValidationGreedy : TrainFinalPolicy::Off);
}
[[nodiscard]] ExecutionFacts derive_execution_facts(const TrainRequest&, std::uint64_t revision);
[[nodiscard]] ExecutionFacts derive_training_validation_facts(const TrainRequest&, std::uint64_t revision);
[[nodiscard]] ExecutionFacts derive_execution_facts(const ValidateRequest&, std::uint64_t revision);
[[nodiscard]] ExecutionFacts derive_execution_facts(const PredictRequest&, std::uint64_t revision);
MMLTK_REFLECT_ENUM(TrainLaneMode)
MMLTK_REFLECT_ENUM(TrainBalancing)
MMLTK_REFLECT_ENUM(TrainMergeCadence)
MMLTK_REFLECT_ENUM(TrainFinalPolicy)
MMLTK_REFLECT_ENUM(ExecutionLimitation)
MMLTK_REFLECT_FIELDS(TrainModelSettings)
MMLTK_REFLECT_FIELDS(TrainLaneConfiguration)
MMLTK_REFLECT_FIELDS(TrainDataPolicy)
MMLTK_REFLECT_FIELDS(ExecutionFacts)
}  // namespace mmltk::backend::models::rfdetr
