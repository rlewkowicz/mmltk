#include "training_metrics.h"
#include <algorithm>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
TrainingSourceCatalog training_source_catalog(const TrainRequest& request) {
 TrainingSourceCatalog result;
 const auto weights = request.use_ema ? EvaluatedWeights::Ema : EvaluatedWeights::Ordinary;
 const auto& configuration = request.lane_configuration;
 if (configuration.mode == TrainLaneMode::SharedGradients)
  result.available.push_back({TrainingRecordScope::Model, 0, weights});
 else {
  if (configuration.mode == TrainLaneMode::PeriodicAveraging) result.available.push_back({TrainingRecordScope::SynchronizedSession, 0, EvaluatedWeights::Ordinary});
  for (const auto& model : configuration.models) result.available.push_back({TrainingRecordScope::Model, model.model_id, weights});
 }
 if (!result.available.empty()) {
  const auto preferred = std::ranges::min_element(result.available, [&](const auto& left, const auto& right) {
   if ((left.weights == weights) != (right.weights == weights)) return left.weights == weights;
   return left.model_id < right.model_id;
  });
  result.default_source = *preferred;
 }
 return result;
}
TrainingMetricSource training_metric_source(const TrainingRecord& record) noexcept { return {record.progress.scope, record.progress.model_id, record.evaluated_weights}; }
namespace {
void require(bool valid) {
 if (!valid) throw std::invalid_argument("invalid retained training sources");
}
void validate_observation(const TrainingRecord& observation, const TrainingRecord& current, const TrainingSourceCatalog& catalog) {
 const auto& progress = observation.progress;
 require(observation.format_version == kTrainingRunFormat && observation.run_id == current.run_id && observation.attempt_id == current.attempt_id && observation.sequence <= current.sequence &&
         observation.role == TrainingRecordRole::Epoch && progress.phase == TrainingPhase::EpochComplete && !observation.attempt_configuration && !progress.failure && !progress.test &&
         progress.artifact && progress.val && progress.session_id == current.progress.session_id && progress.epoch >= 0 &&
         (current.role == TrainingRecordRole::Terminal || progress.epoch <= current.progress.epoch));
 require(std::ranges::find(catalog.available, training_metric_source(observation)) != catalog.available.end());
 const auto& artifact = *progress.artifact;
 require(artifact.weights == observation.evaluated_weights && artifact.session_id == progress.session_id && artifact.epoch == static_cast<std::uint64_t>(progress.epoch) &&
         (!artifact.evaluation || artifact.evaluation == progress.val) && (progress.scope != TrainingRecordScope::Model || artifact.model_id == progress.model_id) &&
         (progress.scope != TrainingRecordScope::SynchronizedSession || (progress.model_id == 0 && artifact.weights == EvaluatedWeights::Ordinary)));
}
}  // namespace
void validate_training_sources(const TrainingSources& sources, const TrainingRecord& current) {
 require(sources.catalog.available.size() <= kTrainingSourceCapacity && sources.observations.size() <= sources.catalog.available.size() && sources.failures.size() <= kMaximumTrainingModels + 1);
 require(sources.catalog.available.empty() == !sources.catalog.default_source);
 if (sources.catalog.default_source) require(std::ranges::find(sources.catalog.available, *sources.catalog.default_source) != sources.catalog.available.end());
 for (std::size_t index = 0; index < sources.catalog.available.size(); ++index) {
  const auto& source = sources.catalog.available[index];
  require((source.scope == TrainingRecordScope::Model && source.weights != EvaluatedWeights::Soup) ||
          (source.scope == TrainingRecordScope::SynchronizedSession && source.model_id == 0 && source.weights == EvaluatedWeights::Ordinary));
  require(std::find(sources.catalog.available.begin(), sources.catalog.available.begin() + index, source) == sources.catalog.available.begin() + index);
 }
 for (std::size_t index = 0; index < sources.observations.size(); ++index) {
  const auto& observation = sources.observations[index];
  validate_observation(observation, current, sources.catalog);
  for (std::size_t prior = 0; prior < index; ++prior)
   require(training_metric_source(sources.observations[prior]) != training_metric_source(observation) && sources.observations[prior].sequence != observation.sequence);
 }
 for (std::size_t index = 0; index < sources.failures.size(); ++index) {
  const auto& failure = sources.failures[index];
  require(failure.session_id == current.progress.session_id && failure.first_cause != 0 && !failure.detail.empty());
  require(
   failure.model_id == 0 || std::ranges::any_of(sources.catalog.available, [&](const auto& source) { return source.scope == TrainingRecordScope::Model && source.model_id == failure.model_id; }));
  for (std::size_t prior = 0; prior < index; ++prior) require(sources.failures[prior].model_id != failure.model_id);
 }
 require(sources.distributions.size() <= kMaximumTrainingModels);
 for (std::size_t index = 0; index < sources.distributions.size(); ++index) {
  const auto& distribution = sources.distributions[index];
  require(distribution.scheduled_draws >= distribution.unique_images && distribution.unused_tail <= distribution.scheduled_draws &&
          (current.role == TrainingRecordRole::Terminal || (current.progress.epoch >= 0 && distribution.epoch <= static_cast<std::uint64_t>(current.progress.epoch))));
  require(std::ranges::any_of(sources.catalog.available, [&](const auto& source) { return source.scope == TrainingRecordScope::Model && source.model_id == distribution.model_id; }));
  for (std::size_t prior = 0; prior < index; ++prior) require(sources.distributions[prior].model_id != distribution.model_id);
 }
 if (sources.selected) {
  validate_training_selection(*sources.selected);
  require(sources.selected->artifact.session_id == current.progress.session_id);
 }
}
void validate_training_progress_document(const TrainingProgressDocument& document, const TrainingSourceCatalog& catalog) {
 const auto& current = document.record;
 require(document.format_version == kTrainingRunFormat && current.format_version == kTrainingRunFormat && !current.run_id.empty() && !current.attempt_id.empty() &&
         current.progress.session_id == current.run_id && document.sources.catalog == catalog);
 validate_training_sources(document.sources, current);
 if (document.final) { require(current.role == TrainingRecordRole::Terminal && current.progress.phase == TrainingPhase::Completed && document.final->selected == document.sources.selected); }
 if (document.sources.selected) {
  require(document.final.has_value() && current.role == TrainingRecordRole::Terminal && current.progress.phase == TrainingPhase::Completed &&
          current.progress.scope == TrainingRecordScope::SelectedOutput && current.progress.artifact == document.sources.selected->artifact &&
          current.progress.model_id == document.sources.selected->artifact.model_id && current.evaluated_weights == document.sources.selected->artifact.weights &&
          current.progress.val == document.sources.selected->validation);
 }
}
void retain_training_source(TrainingSources& sources, const TrainingRecord& record) {
 if (record.role == TrainingRecordRole::Epoch && record.progress.val && record.progress.artifact &&
     (record.progress.scope == TrainingRecordScope::Model || record.progress.scope == TrainingRecordScope::SynchronizedSession)) {
  validate_observation(record, record, sources.catalog);
  const auto identity = training_metric_source(record);
  auto found = std::ranges::find_if(sources.observations, [&](const auto& prior) { return training_metric_source(prior) == identity; });
  if (found == sources.observations.end()) {
   require(sources.observations.size() < kTrainingSourceCapacity);
   sources.observations.push_back(record);
  } else {
   require(found->run_id == record.run_id && found->attempt_id == record.attempt_id && found->sequence < record.sequence);
   *found = record;
  }
 }
 if (record.progress.distribution) {
  const auto& distribution = *record.progress.distribution;
  auto found = std::ranges::find(sources.distributions, distribution.model_id, &TrainingDistributionFacts::model_id);
  if (found == sources.distributions.end()) {
   require(sources.distributions.size() < kMaximumTrainingModels);
   sources.distributions.push_back(distribution);
  } else if (*found != distribution)
   *found = distribution;
 }
 if (record.progress.failure) {
  const auto& failure = *record.progress.failure;
  require(failure.session_id == record.progress.session_id && failure.first_cause != 0 && !failure.detail.empty());
  const auto found = std::ranges::find(sources.failures, failure.model_id, &TrainingFailure::model_id);
  if (found == sources.failures.end()) {
   require(sources.failures.size() < kMaximumTrainingModels + 1);
   sources.failures.push_back(failure);
  } else
   require(*found == failure);
 }
}
}  // namespace mmltk::backend::models::rfdetr
