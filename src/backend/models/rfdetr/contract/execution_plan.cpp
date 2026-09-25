#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <algorithm>
#include "src/common/math/deterministic_sampling.h"
#include <cmath>
#include <limits>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
std::uint64_t checked_training_product(const std::uint64_t left, const std::uint64_t right) {
 if (right && left > std::numeric_limits<std::uint64_t>::max() / right) throw std::invalid_argument("training logical count overflows");
 return left * right;
}
std::uint64_t training_stochastic_key(std::uint64_t seed, const std::uint64_t model, const std::uint64_t epoch, const std::uint64_t occurrence, const std::uint64_t purpose) noexcept {
 using mmltk::common::math::deterministic_mix64;
 for (const auto value : {model, epoch, occurrence, purpose}) seed = deterministic_mix64(seed ^ deterministic_mix64(value));
 return seed;
}
void resize_training_models(TrainLaneConfiguration& configuration, const std::size_t count, const TrainRecipeSettings& recipe, const std::uint64_t session_seed) {
 if (count == 0 || count > kMaximumTrainingModels) throw std::invalid_argument("model count must be between one and sixteen");
 if (configuration.next_model_id == 0 || count - std::min(count, configuration.models.size()) > std::numeric_limits<std::uint64_t>::max() - configuration.next_model_id)
  throw std::invalid_argument("training model identity exhausted");
 configuration.models.resize(std::min(count, configuration.models.size()));
 while (configuration.models.size() < count) {
  const auto id = configuration.next_model_id++;
  configuration.models.push_back({id, training_stochastic_key(session_seed, id, 0, 0), recipe, 1});
 }
}
TrainFinalPolicy effective_final_policy(const TrainLaneConfiguration& config) noexcept {
 return config.final_policy.value_or(config.mode == TrainLaneMode::Independent ? TrainFinalPolicy::ValidationGreedy : TrainFinalPolicy::Off);
}
void validate_training_configuration(const TrainRequest& request) {
 if (!train_recipe_valid(request.recipe)) throw std::invalid_argument("invalid training recipe: UltralyticsLinear requires SGD; Nesterov requires SGD with positive momentum");
 if (request.data_policy.rare_threshold <= 0 || mmltk::frameworks::reflection::validate_reflected_fields(request.data_policy))
  throw std::invalid_argument("invalid training data policy");
 const auto& config = request.lane_configuration;
 if (mmltk::frameworks::reflection::validate_reflected_fields(config) || request.lanes < 1 || request.grad_accum_steps < 1 || request.batch_size == 0)
  throw std::invalid_argument("invalid training lane configuration");
 if (config.mode != TrainLaneMode::SharedGradients && config.models.size() != static_cast<std::size_t>(request.lanes))
  throw std::invalid_argument("independent training lane count must match model entries");
 double coefficients = 0;
 for (std::size_t index = 0; index < config.models.size(); ++index) {
  const auto& model = config.models[index];
  if (model.model_id >= config.next_model_id || !train_recipe_valid(model.recipe)) throw std::invalid_argument("invalid training model identity or recipe");
  for (std::size_t prior = 0; prior < index; ++prior)
   if (config.models[prior].model_id == model.model_id) throw std::invalid_argument("duplicate training model identity");
  coefficients += model.coefficient;
 }
 if (effective_final_policy(config) == TrainFinalPolicy::Explicit && (!(coefficients > 0) || !std::isfinite(coefficients)))
  throw std::invalid_argument("explicit final coefficients require a finite positive sum");
}
ExecutionFacts derive_execution_facts(const TrainRequest& request, const std::uint64_t revision) {
 validate_training_configuration(request);
 const bool shared = request.lane_configuration.mode == TrainLaneMode::SharedGradients;
 const auto k = checked_training_product(request.grad_accum_steps, shared ? request.lanes : 1);
 const auto effective = checked_training_product(request.batch_size, k);
 return {.settings_revision = revision, .logical_models = shared ? 1U : static_cast<std::uint64_t>(request.lanes),
  .microbatches_per_attempt = k, .effective_batch_per_model = effective,
  .aggregate_round_images = checked_training_product(checked_training_product(request.batch_size, request.grad_accum_steps), request.lanes),
  .configured_capacity = static_cast<std::uint64_t>(request.lanes),
  .limitation = shared ? ExecutionLimitation::None : request.lane_configuration.mode == TrainLaneMode::Independent ? ExecutionLimitation::ExperimentalIndependentModels : ExecutionLimitation::ExperimentalPeriodicAveraging};
}
namespace {
ExecutionFacts inference_facts(std::size_t batch, int lanes, std::uint64_t revision) {
 if (!batch || lanes < 1) throw std::invalid_argument("inference batch and lanes must be positive");
 const auto effective = checked_training_product(batch, lanes);
 return {.settings_revision = revision, .effective_batch_per_model = effective, .aggregate_round_images = effective, .configured_capacity = static_cast<std::uint64_t>(lanes)};
}
}
ExecutionFacts derive_training_validation_facts(const TrainRequest& request, std::uint64_t revision) { return inference_facts(request.val_batch_size ? request.val_batch_size : request.batch_size, request.validation_lanes, revision); }
ExecutionFacts derive_execution_facts(const ValidateRequest& request, std::uint64_t revision) { return inference_facts(request.batch_size, request.lanes, revision); }
ExecutionFacts derive_execution_facts(const PredictRequest& request, std::uint64_t revision) { return inference_facts(request.batch_size, request.lanes, revision); }
}
