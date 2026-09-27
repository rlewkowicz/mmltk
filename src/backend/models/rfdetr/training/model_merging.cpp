#include "detail/model_merging.h"
#include "detail/training_artifact.h"
#include "checkpoint.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/pch_std.h"
namespace mmltk::backend::models::rfdetr {
namespace {
void compatible(std::span<const NormalizedModelStateEntry> first, std::span<const NormalizedModelStateEntry> next) {
 if (first.size() != next.size()) throw std::invalid_argument("native merge tensor inventory differs");
 for (std::size_t index = 0; index < first.size(); ++index) {
  const auto& a = first[index];
  const auto& b = next[index];
  if (a.name != b.name || a.tensor.sizes() != b.tensor.sizes() || a.tensor.scalar_type() != b.tensor.scalar_type() || a.tensor.device() != b.tensor.device())
   throw std::invalid_argument("native merge tensor identity differs: " + a.name);
 }
}
// One scalar per actual device; tensor checks never request a host decision.
void accumulate_check(std::vector<torch::Tensor>& checks, const torch::Tensor& condition) {
 auto found = std::ranges::find_if(checks, [&](const auto& check) { return check.device() == condition.device(); });
 if (found == checks.end()) {
  checks.push_back(torch::ones({}, condition.options().dtype(torch::kBool)));
  found = std::prev(checks.end());
 }
 found->logical_and_(condition);
}
void check_values(std::span<const NormalizedModelStateEntry> first, std::span<const NormalizedModelStateEntry> next, std::vector<torch::Tensor>& checks) {
 for (std::size_t index = 0; index < first.size(); ++index) {
  const auto& tensor = next[index].tensor;
  accumulate_check(checks, tensor.is_floating_point() ? torch::isfinite(tensor).all() : tensor.eq(first[index].tensor).all());
 }
}
void require_checks(const std::vector<torch::Tensor>& checks) {
 for (const auto& check : checks)
  if (!check.item<bool>()) throw std::invalid_argument("native merge contains nonfinite values or unequal nonfloating buffers");
}
// All paths created by this selection remain unpublished until its descriptor
// commits. Prior selections and ingredient paths never enter this custody.
class SelectionTrials final {
public:
 ~SelectionTrials() {
  for (const auto& path : paths_) {
   std::error_code ignored;
   std::filesystem::remove(path, ignored);
  }
 }
 void own(const std::filesystem::path& path) { paths_.push_back(path); }
 void commit(const std::filesystem::path& path) { std::erase(paths_, path); }
 void reject(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  commit(path);
 }

private:
 std::vector<std::filesystem::path> paths_;
};
}  // namespace
void NativeModelAverage::prepare(std::span<const std::vector<NormalizedModelStateEntry>* const> inputs, std::span<const double> coefficients) {
 prepared_ = false;
 if (inputs.empty() || coefficients.size() != inputs.size()) throw std::invalid_argument("native merge needs complete coefficients");
 if (!inputs.front()) throw std::invalid_argument("invalid native merge input");
 double total = 0;
 for (std::size_t index = 0; index < inputs.size(); ++index) {
  if (!inputs[index] || !std::isfinite(coefficients[index]) || coefficients[index] < 0) throw std::invalid_argument("invalid native merge coefficient");
  total += coefficients[index];
  compatible(*inputs.front(), *inputs[index]);
 }
 if (!(total > 0) || !std::isfinite(total)) throw std::invalid_argument("native merge requires positive finite coefficient sum");
 torch::NoGradGuard guard;
 for (auto& check : checks_) check.fill_(true);
 for (const auto* input : inputs) check_values(*inputs.front(), *input, checks_);
 const auto& reference = *inputs.front();
 if (reference.empty()) throw std::invalid_argument("native merge has no tensor values");
 if (values_.size() != reference.size()) {
  values_.resize(reference.size());
  source_types_.resize(reference.size());
 }
 for (std::size_t index = 0; index < reference.size(); ++index) {
  const auto& source = reference[index];
  auto& destination = values_[index];
  destination.name = source.name;
  source_types_[index] = source.tensor.scalar_type();
  const auto dtype = source.tensor.is_floating_point() && source.tensor.scalar_type() != torch::kFloat64 ? torch::kFloat32 : source.tensor.scalar_type();
  if (!destination.tensor.defined() || destination.tensor.sizes() != source.tensor.sizes() || destination.tensor.scalar_type() != dtype || destination.tensor.device() != source.tensor.device())
   destination.tensor = torch::empty_like(source.tensor, source.tensor.options().dtype(dtype));
  if (!source.tensor.is_floating_point()) {
   destination.tensor.copy_(source.tensor);
   continue;
  }
  destination.tensor.zero_();
  for (std::size_t input = 0; input < inputs.size(); ++input)
   if (coefficients[input] > 0) destination.tensor.add_((*inputs[input])[index].tensor, coefficients[input] / total);
  accumulate_check(checks_, torch::isfinite(destination.tensor).all());
 }
 require_checks(checks_);
 prepared_ = true;
}
void NativeModelAverage::install(std::span<const std::vector<NormalizedModelStateEntry>* const> receivers) const {
 if (!prepared_) throw std::logic_error("native merge has no admitted average");
 // Validate the entire destination set before its first write.
 for (const auto* receiver : receivers) {
  if (!receiver || receiver->size() != values_.size()) throw std::invalid_argument("invalid native merge receiver");
  for (std::size_t i = 0; i < values_.size(); ++i)
   if ((*receiver)[i].name != values_[i].name || (*receiver)[i].tensor.sizes() != values_[i].tensor.sizes() || (*receiver)[i].tensor.scalar_type() != source_types_[i] ||
       (*receiver)[i].tensor.device() != values_[i].tensor.device())
    throw std::invalid_argument("native merge receiver identity differs");
 }
 torch::NoGradGuard guard;
 for (const auto* receiver : receivers)
  for (std::size_t i = 0; i < values_.size(); ++i) (*receiver)[i].tensor.copy_(values_[i].tensor);
}
TrainingMergeSchedule::TrainingMergeSchedule(const TrainLaneConfiguration& configuration, TrainingMergeState state)
    : cadence_(configuration.merge_cadence), enabled_(configuration.mode == TrainLaneMode::PeriodicAveraging), interval_(configuration.merge_rounds), state_(state) {
 if (!interval_ || (!enabled_ && state_.interval_rounds) || (enabled_ && cadence_ == TrainMergeCadence::Rounds && state_.interval_rounds >= interval_))
  throw std::invalid_argument("invalid merge interval state");
}
bool TrainingMergeSchedule::finish_round(bool contained_attempts) {
 if (!contained_attempts) return false;
 state_.round = mmltk::common::math::checked_add(state_.round, std::uint64_t{1}, "training round overflow");
 if (!enabled_) return false;
 state_.interval_rounds = mmltk::common::math::checked_add(state_.interval_rounds, std::uint64_t{1}, "merge interval overflow");
 return cadence_ == TrainMergeCadence::Rounds && state_.interval_rounds == interval_;
}
void TrainingMergeSchedule::retire_interval() {
 if (!pending()) return;
 state_.merge = mmltk::common::math::checked_add(state_.merge, std::uint64_t{1}, "merge index overflow");
 state_.interval_rounds = 0;
}
TrainingSelection select_training_artifact(std::span<const TrainingSelectionCandidate> candidates, TrainFinalPolicy policy, bool masks, const std::filesystem::path& output,
 std::function_ref<EvalSummary(const std::filesystem::path&)> evaluate) {
 switch (policy) {
  case TrainFinalPolicy::Off:
  case TrainFinalPolicy::Uniform:
  case TrainFinalPolicy::Explicit:
  case TrainFinalPolicy::ValidationGreedy: break;
  default: throw std::invalid_argument("invalid final training selection policy");
 }
 if (candidates.empty() || candidates.size() > kMaximumTrainingModels) throw std::invalid_argument("final selection requires one candidate per model");
 const auto& reference = candidates.front().artifact;
 std::unordered_set<std::uint64_t> ids;
 std::vector<std::shared_ptr<const TrainingArtifactAdmission>> admitted, proofs;
 std::vector<std::shared_ptr<const DecodedNativeModelState>> states;
 admitted.reserve(candidates.size());
 proofs.reserve(candidates.size());
 states.reserve(candidates.size());
 const auto decoded = [&](std::size_t index) -> const DecodedNativeModelState& {
  auto& state = states[index];
  if (!state) {
   // A retained proof already admitted the content. Only arithmetic or a
   // comparison with different content needs decoded values again.
   for (std::size_t other = 0; other < states.size(); ++other) {
    if (states[other] && proofs[other]->content() == proofs[index]->content()) {
     state = states[other];
     break;
    }
   }
   if (!state) state = proofs[index]->decode();
  }
  return *state;
 };
 std::vector<torch::Tensor> compatibility_checks;
 std::vector<std::size_t> order(candidates.size());
 std::iota(order.begin(), order.end(), 0);
 for (const auto& candidate : candidates) {
  const auto& artifact = candidate.artifact;
  if (!ids.insert(artifact.model_id).second || artifact.session_id != reference.session_id || artifact.initialization != reference.initialization ||
      artifact.configuration != reference.configuration || artifact.validation != reference.validation || artifact.weights != reference.weights || !artifact.selection_metric || !artifact.evaluation ||
      training_selection_metric(*artifact.evaluation, masks) != *artifact.selection_metric || artifact.initialization.empty() || artifact.validation.empty())
   throw std::invalid_argument("incompatible final model candidate");
  const auto path = std::filesystem::absolute(artifact.path).lexically_normal();
  const auto found = std::ranges::find_if(admitted, [&](const auto& file) { return file->evidence()->artifact_path() == path; });
  std::shared_ptr<const TrainingArtifactAdmission> admission;
  if (found != admitted.end()) {
   admission = *found;
   admission->require_matches(artifact);
   const auto first = std::ranges::find(proofs, admission);
   states.push_back(states[static_cast<std::size_t>(first - proofs.begin())]);
  } else {
   std::shared_ptr<const DecodedNativeModelState> state;
   if (candidate.admission) {
    admission = candidate.admission;
    admission->require_matches(artifact);
   } else {
    auto fresh = std::make_shared<TrainingArtifactAdmission>(artifact);
    state = fresh->decode();
    fresh->release_decoded_state();
    admission = std::move(fresh);
   }
   if (!states.empty() && !same_native_model_semantics(admission->metadata(), admitted.front()->metadata())) throw std::invalid_argument("final native model semantics differ");
   // Every distinct named file completed admission before values may be shared.
   const auto same = std::ranges::find_if(proofs, [&](const auto& file) { return file->content() == admission->content(); });
   if (same != proofs.end()) {
    auto& shared = states[static_cast<std::size_t>(same - proofs.begin())];
    if (shared)
     state = shared;
    else if (state)
     shared = state;
   } else if (!states.empty()) {
    if (!state) state = admission->decode();
    const auto& first = decoded(0);
    compatible(first.entries(), state->entries());
    check_values(first.entries(), state->entries(), compatibility_checks);
   }
   admitted.push_back(admission);
   states.push_back(std::move(state));
  }
  proofs.push_back(std::move(admission));
 }
 require_checks(compatibility_checks);
 std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
  const auto& x = candidates[a].artifact;
  const auto& y = candidates[b].artifact;
  if (*x.selection_metric != *y.selection_metric) return *x.selection_metric > *y.selection_metric;
  if (policy != TrainFinalPolicy::ValidationGreedy && x.epoch != y.epoch) return x.epoch < y.epoch;
  return x.model_id < y.model_id;
 });
 TrainingSelection selected;
 selected.method = policy;
 selected.best_individual_metric = *candidates[order.front()].artifact.selection_metric;
 selected.artifact = candidates[order.front()].artifact;
 auto selected_admission = proofs[order.front()];
 selected.validation = *selected.artifact.evaluation;
 std::vector<std::size_t> accepted{order.front()};
 NativeModelAverage average;
 SelectionTrials trials;
 const auto produce = [&](std::span<const std::size_t> indices, bool explicit_weights) {
  std::vector<const std::vector<NormalizedModelStateEntry>*> inputs;
  std::vector<double> weights;
  for (auto index : indices) {
   inputs.push_back(&decoded(index).entries());
   weights.push_back(explicit_weights ? candidates[index].coefficient : 1.0);
  }
  average.prepare(inputs, weights);
  DecodedNativeModelState state;
  state.metadata = admitted.front()->metadata();
  std::vector<NormalizedModelStateEntry> values = average.values();
  const auto& source = decoded(0).entries();
  for (std::size_t index = 0; index < values.size(); ++index) values[index].tensor = values[index].tensor.to(source[index].tensor.scalar_type());
  state.replace_entries(std::move(values));
  // Every trial owns a fresh immutable path. Rejected trials are not selections.
  const auto path = std::filesystem::absolute(output) / ("selection-" + training_artifact_identity() + ".pt");
  if (std::filesystem::exists(path)) throw std::runtime_error("native selection trial identity already exists");
  trials.own(path);
  save_native_checkpoint(path, state);
  auto admission = std::make_shared<TrainingArtifactAdmission>(path);
  admission->release_decoded_state();
  auto summary = evaluate(path);
  admission->require_unchanged();
  const auto metric = training_selection_metric(summary, masks);
  return std::tuple{path, std::move(summary), metric, std::move(admission)};
 };
 const auto accept_trial = [&](std::filesystem::path path, EvalSummary summary, double metric, std::shared_ptr<TrainingArtifactAdmission> admission) {
  selected.artifact.path = std::move(path);
  selected.validation = std::move(summary);
  selected.artifact.selection_metric = metric;
  selected.artifact.weights = EvaluatedWeights::Soup;
  selected.artifact.content = admission->content();
  selected.artifact.sha256 = admission->sha256();
  selected_admission = std::move(admission);
 };
 if (policy == TrainFinalPolicy::ValidationGreedy) {
  for (std::size_t position = 1; position < order.size(); ++position) {
   const auto next = order[position];
   if (std::ranges::any_of(accepted, [&](auto index) { return candidates[index].artifact.content == candidates[next].artifact.content; })) continue;
   auto trial = accepted;
   trial.push_back(next);
   auto [path, summary, metric, admission] = produce(trial, false);
   if (metric > *selected.artifact.selection_metric) {
    accepted = std::move(trial);
    accept_trial(std::move(path), std::move(summary), metric, std::move(admission));
   } else
    trials.reject(path);
  }
 } else if (policy == TrainFinalPolicy::Uniform || policy == TrainFinalPolicy::Explicit) {
  accepted = order;
  auto [path, summary, metric, admission] = produce(accepted, policy == TrainFinalPolicy::Explicit);
  accept_trial(std::move(path), std::move(summary), metric, std::move(admission));
 }
 selected.artifact.evaluation = selected.validation;
 double total = 0;
 for (auto index : accepted) total += policy == TrainFinalPolicy::Explicit ? candidates[index].coefficient : 1;
 for (auto index : accepted)
  selected.ingredients.push_back({candidates[index].artifact.model_id, candidates[index].artifact.sha256, (policy == TrainFinalPolicy::Explicit ? candidates[index].coefficient : 1) / total});
 for (const auto& file : admitted) file->require_unchanged();
 if (selected.artifact.weights == EvaluatedWeights::Soup) {
  selected.artifact.model_id = 0;
  for (auto index : accepted) {
   selected.artifact.epoch = std::max(selected.artifact.epoch, candidates[index].artifact.epoch);
   selected.artifact.attempt = std::max(selected.artifact.attempt, candidates[index].artifact.attempt);
   selected.artifact.merge = std::max(selected.artifact.merge, candidates[index].artifact.merge);
  }
 }
 validate_training_selection(selected);
 publish_training_selection(output / "selected.json", selected, *selected_admission);
 trials.commit(selected.artifact.path);
 return selected;
}
}  // namespace mmltk::backend::models::rfdetr
