#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <cstdint>
#include <vector>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
namespace mmltk::backend::models::rfdetr {
enum class TrainingGroupRole : std::uint8_t { Ordinary, Bias };
struct TrainingScheduleState final {
 std::uint64_t consumed_attempts = 0;
 std::uint64_t successful_updates = 0;
 std::uint64_t consumed_microbatches = 0;
 std::uint64_t steps_ref = 0;
 std::uint64_t nb_ref = 0;
 std::uint64_t original_epochs = 0;
 std::uint64_t warmup_microbatches = 0;
 std::uint64_t epoch = 0;
 bool epoch_event_applied = false;
 double held_momentum = 0;
 MMLTK_MAX_ITEMS(65536) std::vector<double> absolute_lrs;
 bool operator==(const TrainingScheduleState&) const = default;
};
[[nodiscard]] double compute_lr_scale(const TrainRecipeSettings&, std::uint64_t current_attempt, std::uint64_t reference_attempts, std::uint64_t total_attempts);
[[nodiscard]] double compute_warmup_momentum(const TrainRecipeSettings&, std::uint64_t current_attempt, std::uint64_t reference_attempts, double target);
class TrainingSchedule final {
public:
 TrainingSchedule(
  TrainRecipeSettings, std::vector<double> base_lrs, std::vector<TrainingGroupRole>, std::uint64_t epochs, std::uint64_t first_epoch_microbatches, std::uint64_t microbatches_per_attempt);
 void restore(TrainingScheduleState);
 void begin_epoch(std::uint64_t epoch);
 void consume_microbatch();
 void prepare_attempt();
 void finish_attempt(bool successful);
 [[nodiscard]] const TrainingScheduleState& state() const noexcept { return state_; }

private:
 [[nodiscard]] double epoch_factor() const noexcept;
 TrainRecipeSettings recipe_;
 std::vector<double> base_lrs_;
 std::vector<TrainingGroupRole> roles_;
 std::uint64_t requested_epochs_;
 std::uint64_t contributions_;
 TrainingScheduleState state_;
};
MMLTK_REFLECT_ENUM(TrainingGroupRole)
MMLTK_REFLECT_FIELDS(TrainingScheduleState)
}  // namespace mmltk::backend::models::rfdetr
