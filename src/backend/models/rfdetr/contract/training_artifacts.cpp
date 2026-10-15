#include "training_artifacts.h"
#include <cmath>
#include <stdexcept>
#include <unordered_set>
namespace mmltk::backend::models::rfdetr {
namespace {
bool digest(const std::string& value) { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool identity(const std::string& value) {
 return !value.empty() && value.size() <= 64 && value.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") == std::string::npos;
}
}  // namespace
double training_selection_metric(const EvalSummary& summary, ModelTask task) {
 if (!model_task_name(task)) throw std::invalid_argument("selection model task is invalid");
 const MetricSummary* selected = &summary.bbox;
 if (task == ModelTask::Segmentation) {
  if (!summary.mask) throw std::invalid_argument("segmentation selection requires mask AP");
  selected = &*summary.mask;
 } else if (task == ModelTask::Keypoints) {
  if (!summary.keypoint) throw std::invalid_argument("keypoint selection requires OKS AP");
  selected = &*summary.keypoint;
 }
 if (!selected->available) throw std::invalid_argument("selection requires an available task metric");
 const double value = selected->ap;
 if (!std::isfinite(value)) throw std::invalid_argument("selection requires a finite validation metric");
 return value;
}
void validate_training_selection(const TrainingSelection& value) {
 const auto& artifact = value.artifact;
 if (value.format_version != kTrainingSelectionFormat || !identity(artifact.session_id) || !digest(artifact.initialization) || !digest(artifact.configuration) || !digest(artifact.content) ||
     !digest(artifact.sha256) || !digest(artifact.validation) || !artifact.evaluation || *artifact.evaluation != value.validation || artifact.path.empty() || !artifact.selection_metric ||
     !std::isfinite(*artifact.selection_metric) || !std::isfinite(value.best_individual_metric) || value.ingredients.empty() || value.ingredients.size() > kMaximumTrainingModels)
  throw std::invalid_argument("invalid selected training artifact");
 std::unordered_set<std::uint64_t> ids;
 double total = 0;
 for (const auto& ingredient : value.ingredients) {
  if (!ids.insert(ingredient.model_id).second || !digest(ingredient.sha256) || !std::isfinite(ingredient.coefficient) || ingredient.coefficient < 0)
   throw std::invalid_argument("invalid selected ingredient");
  total += ingredient.coefficient;
 }
 if (std::abs(total - 1.0) > 1e-12) throw std::invalid_argument("selected coefficients are not normalized");
}
void validate_training_session_manifest(const TrainingSessionManifest& value) {
 if ((value.identity_schema != 1 && value.identity_schema != 2) || value.format_version != kTrainingSessionFormat || !identity(value.session_id) || !identity(value.attempt_id) || !identity(value.generation) ||
     (!value.previous_generation.empty() && (!identity(value.previous_generation) || value.previous_generation == value.generation)) || !digest(value.initialization) || !digest(value.configuration) ||
     !digest(value.validation) || !digest(value.plan_sha256) || value.models.empty() || value.models.size() > kMaximumTrainingModels)
  throw std::invalid_argument("invalid training session manifest");
 validate_train_request(value.request);
 if (derive_execution_facts(value.request, 0).logical_models != value.models.size()) throw std::invalid_argument("session model count differs from its admitted configuration");
 std::unordered_set<std::uint64_t> ids;
 for (const auto& model : value.models) {
  if (!ids.insert(model.model_id).second || !digest(model.sha256) || model.path != std::filesystem::path("generations") / value.generation / ("model-" + std::to_string(model.model_id) + ".pt"))
   throw std::invalid_argument("mixed or invalid training session generation");
  if (model.best &&
      (model.best->model_id != model.model_id || model.best->session_id != value.session_id || model.best->initialization != value.initialization || model.best->configuration != value.configuration ||
       !digest(model.best->content) || model.best->validation != value.validation || !model.best->selection_metric || !std::isfinite(*model.best->selection_metric) || !digest(model.best->sha256)))
   throw std::invalid_argument("invalid session candidate identity");
 }
}
}  // namespace mmltk::backend::models::rfdetr
