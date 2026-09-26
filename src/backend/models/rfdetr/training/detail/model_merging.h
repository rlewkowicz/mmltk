#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include "training_artifact.h"
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <torch/types.h>
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/contract/training_artifacts.h"
namespace mmltk::backend::models::rfdetr {
// Only named native values are merged. Optimizer, scaler, clocks and EMA never
// enter this owner. A prepared candidate is independent of all input storage.
class NativeModelAverage final {
public:
 void prepare(std::span<const std::vector<NormalizedModelStateEntry>* const>, std::span<const double> coefficients);
 void install(std::span<const std::vector<NormalizedModelStateEntry>* const> receivers) const;
 [[nodiscard]] const std::vector<NormalizedModelStateEntry>& values() const noexcept { return values_; }

private:
 std::vector<NormalizedModelStateEntry> values_;
 std::vector<at::ScalarType> source_types_;
 std::vector<torch::Tensor> checks_;
 bool prepared_ = false;
};
struct TrainingMergeState final {
 std::uint64_t round = 0;
 std::uint64_t merge = 0;
 std::uint64_t interval_rounds = 0;
};
// Called once by the session after a whole round drains, including idle slots.
class TrainingMergeSchedule final {
public:
 explicit TrainingMergeSchedule(const TrainLaneConfiguration&, TrainingMergeState = {});
 [[nodiscard]] bool finish_round(bool contained_attempts);
 [[nodiscard]] bool pending() const noexcept { return state_.interval_rounds != 0; }
 void retire_interval();
 [[nodiscard]] const TrainingMergeState& state() const noexcept { return state_; }

private:
 TrainMergeCadence cadence_;
 bool enabled_;
 std::uint64_t interval_;
 TrainingMergeState state_;
};
struct TrainingSelectionCandidate final {
 TrainingArtifact artifact;
 double coefficient = 1;
 std::shared_ptr<const TrainingArtifactAdmission> admission;
};
// The evaluator sees the exact saved native artifact. Publication of the small
// selected descriptor is the final operation and never replaces an ingredient.
[[nodiscard]] TrainingSelection select_training_artifact(
 std::span<const TrainingSelectionCandidate>, TrainFinalPolicy, bool masks, const std::filesystem::path& output, std::function_ref<EvalSummary(const std::filesystem::path&)> evaluate);
}  // namespace mmltk::backend::models::rfdetr
