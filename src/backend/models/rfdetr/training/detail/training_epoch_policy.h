#pragma once
#include <algorithm>
#include <stdexcept>
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingEpochPolicyState final {
 bool encoder_unfrozen = false;
 bool augmentation_disabled = false;
 bool operator==(const TrainingEpochPolicyState&) const = default;
};
class TrainingEpochPolicy final {
public:
 TrainingEpochPolicy(int unfreeze, int disable, TrainingEpochPolicyState saved = {}) : unfreeze_(unfreeze), disable_(disable), state_(saved) {
  if (unfreeze < 0 || disable < 0) throw std::invalid_argument("final epoch policies must be nonnegative");
 }
 [[nodiscard]] TrainingEpochPolicyState enter(int epoch, int epochs) {
  if (epoch < 0 || epochs <= 0) throw std::invalid_argument("invalid training epoch");
  state_.encoder_unfrozen |= unfreeze_ > 0 && epoch >= std::max(0, epochs - unfreeze_);
  state_.augmentation_disabled |= disable_ > 0 && epoch >= std::max(0, epochs - disable_);
  return state_;
 }
 [[nodiscard]] const TrainingEpochPolicyState& state() const noexcept { return state_; }
private:
 int unfreeze_;
 int disable_;
 TrainingEpochPolicyState state_;
};
MMLTK_REFLECT_FIELDS(TrainingEpochPolicyState)
}
