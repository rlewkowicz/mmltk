#include "detail/training_schedule.h"
#include "src/backend/models/rfdetr/contract/execution_plan.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
namespace {
std::uint64_t half_even(double value) {
 if (!std::isfinite(value) || value < 0 || value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) throw std::invalid_argument("warmup iteration count overflows");
 const auto floor = static_cast<std::uint64_t>(value);
 const auto fraction = value - static_cast<double>(floor);
 return floor + (fraction > .5 || (fraction == .5 && floor % 2));
}
void advance(std::uint64_t& value) {
 if (value == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("training schedule clock exhausted");
 ++value;
}
}
double compute_lr_scale(const TrainRecipeSettings& config, const std::uint64_t step, const std::uint64_t steps_per_epoch, const std::uint64_t total_steps) {
 const double warmup = std::trunc(static_cast<double>(steps_per_epoch) * config.warmup_epochs);
 if (warmup > 0.0 && static_cast<double>(step) < warmup) return static_cast<double>(step) / std::max(1.0, warmup);
 if (config.lr_scheduler == TrainLrSchedulerKind::Cosine) {
  const double progress = static_cast<double>(step) - warmup;
  const double denominator = std::max(1.0, static_cast<double>(total_steps) - warmup);
  return config.lr_min_factor + (1.0 - config.lr_min_factor) * 0.5 * (1.0 + std::cos(std::numbers::pi * std::clamp(progress / denominator, 0.0, 1.0)));
 }
 return step < checked_training_product(config.lr_drop, steps_per_epoch) ? 1.0 : 0.1;
}
double compute_warmup_momentum(const TrainRecipeSettings& config, const std::uint64_t step, const std::uint64_t steps_per_epoch, const double target) {
 const double warmup = static_cast<double>(steps_per_epoch) * config.warmup_epochs;
 if (warmup <= 0.0 || config.warmup_momentum <= 0.0 || static_cast<double>(step) >= warmup) return target;
 const double alpha = static_cast<double>(step) / std::max(1.0, warmup);
 return config.warmup_momentum + (target - config.warmup_momentum) * alpha;
}
TrainingSchedule::TrainingSchedule(TrainRecipeSettings recipe, std::vector<double> lrs, std::vector<TrainingGroupRole> roles, std::uint64_t epochs, std::uint64_t nb, std::uint64_t k)
 : recipe_(std::move(recipe)), base_lrs_(std::move(lrs)), roles_(std::move(roles)), requested_epochs_(epochs), contributions_(k) {
 if (!train_recipe_valid(recipe_) || !epochs || !k || !nb || nb % k || base_lrs_.size() != roles_.size() || base_lrs_.empty() || base_lrs_.size() > 65536)
  throw std::invalid_argument("invalid training schedule admission");
 for (auto lr : base_lrs_) if (!std::isfinite(lr) || lr < 0) throw std::invalid_argument("invalid base learning rate");
 state_.steps_ref = nb / k;
 state_.nb_ref = nb;
 state_.original_epochs = epochs;
 state_.held_momentum = recipe_.momentum;
 state_.absolute_lrs = base_lrs_;
 const auto warmup = std::min(recipe_.warmup_epochs, static_cast<double>(epochs - 1));
 state_.warmup_microbatches = half_even(warmup * static_cast<double>(nb));
 (void)checked_training_product(epochs, state_.nb_ref);
 if (recipe_.lr_scheduler != TrainLrSchedulerKind::UltralyticsLinear) {
  (void)checked_training_product(recipe_.lr_drop, state_.steps_ref);
  const auto native_warmup = recipe_.warmup_epochs * static_cast<double>(state_.steps_ref);
  if (!std::isfinite(native_warmup) || native_warmup >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) throw std::invalid_argument("native warmup attempt count overflows");
 }
}
void TrainingSchedule::restore(TrainingScheduleState state) {
 if (!state.steps_ref || !state.nb_ref || state.nb_ref % contributions_ || state.steps_ref != state.nb_ref / contributions_ || state.consumed_attempts > state.consumed_microbatches / contributions_ || !state.original_epochs || state.original_epochs > requested_epochs_ || state.absolute_lrs.size() != base_lrs_.size() ||
     state.successful_updates > state.consumed_attempts || !std::isfinite(state.held_momentum) || state.held_momentum < 0 || state.held_momentum > 1)
  throw std::invalid_argument("invalid saved scheduler state");
 const auto nw = half_even(std::min(recipe_.warmup_epochs, static_cast<double>(state.original_epochs - 1)) * static_cast<double>(state.nb_ref));
 if (state.warmup_microbatches != nw) throw std::invalid_argument("saved warmup reference differs");
 for (auto lr : state.absolute_lrs) if (!std::isfinite(lr) || lr < 0) throw std::invalid_argument("invalid saved absolute learning rate");
 (void)checked_training_product(requested_epochs_, state.steps_ref);
 state_ = std::move(state);
}
double TrainingSchedule::epoch_factor() const noexcept {
 return std::max(1.0 - static_cast<double>(state_.epoch) / static_cast<double>(state_.original_epochs), 0.0) * (1 - recipe_.lr_min_factor) + recipe_.lr_min_factor;
}
void TrainingSchedule::begin_epoch(std::uint64_t epoch) {
 if (state_.epoch_event_applied && state_.epoch == epoch) return;
 if (state_.epoch_event_applied && epoch < state_.epoch) throw std::invalid_argument("training epoch moved backwards");
 state_.epoch = epoch;
 state_.epoch_event_applied = true;
 if (recipe_.lr_scheduler == TrainLrSchedulerKind::UltralyticsLinear)
  for (std::size_t index = 0; index < base_lrs_.size(); ++index) state_.absolute_lrs[index] = base_lrs_[index] * epoch_factor();
}
void TrainingSchedule::consume_microbatch() {
 if (!state_.epoch_event_applied) throw std::logic_error("microbatch precedes epoch event");
 if (recipe_.lr_scheduler == TrainLrSchedulerKind::UltralyticsLinear && state_.consumed_microbatches < state_.warmup_microbatches) {
  const auto fraction = static_cast<double>(state_.consumed_microbatches) / static_cast<double>(state_.warmup_microbatches);
  for (std::size_t index = 0; index < base_lrs_.size(); ++index)
   state_.absolute_lrs[index] = std::lerp(roles_[index] == TrainingGroupRole::Bias ? recipe_.warmup_bias_lr : 0, base_lrs_[index] * epoch_factor(), fraction);
  state_.held_momentum = std::lerp(recipe_.warmup_momentum, recipe_.momentum, fraction);
 }
 advance(state_.consumed_microbatches);
}
void TrainingSchedule::prepare_attempt() {
 if (!state_.epoch_event_applied) throw std::logic_error("optimizer attempt precedes epoch event");
 if (recipe_.lr_scheduler == TrainLrSchedulerKind::UltralyticsLinear) return;
 const auto factor = compute_lr_scale(recipe_, state_.consumed_attempts, state_.steps_ref, checked_training_product(requested_epochs_, state_.steps_ref));
 for (std::size_t index = 0; index < base_lrs_.size(); ++index) state_.absolute_lrs[index] = base_lrs_[index] * factor;
 state_.held_momentum = compute_warmup_momentum(recipe_, state_.consumed_attempts, state_.steps_ref, recipe_.momentum);
}
void TrainingSchedule::finish_attempt(bool successful) {
 advance(state_.consumed_attempts);
 if (successful) advance(state_.successful_updates);
}
}
