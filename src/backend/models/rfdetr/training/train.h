#pragma once
#include <filesystem>
#include <optional>
#include <inplace_vector>
#include <cstdint>
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::backend::models::rfdetr {
class TrainingPeerCancelled final : public std::runtime_error {
public:
 TrainingPeerCancelled() : std::runtime_error("training peer cancelled") {}
};
struct TrainRunResult {
 ResolvedModelArtifacts artifacts;
 GpuAugmentationConfig gpu_augmentation;
 std::filesystem::path output_dir;
 std::filesystem::path checkpoint_path;
 int last_epoch = -1;
 std::uint64_t completed_epochs = 0;
 std::inplace_vector<TrainingMetricProgress, 32> history;
 std::optional<EvalSummary> test_summary;
 std::optional<TrainingSelection> selected;
};
[[nodiscard]] TrainRequest finalize_train_request(TrainRequest request);
TrainRunResult run_training(const TrainRequest& request);
void print_training_summary(const TrainRequest& request, const TrainRunResult& result);
}  // namespace mmltk::backend::models::rfdetr
