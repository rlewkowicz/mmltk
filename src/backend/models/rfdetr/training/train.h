#pragma once
#include <filesystem>
#include <optional>
#include <inplace_vector>
#include <cstdint>
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
namespace mmltk::backend::models::rfdetr {
struct TrainEpochSummary {
    int epoch = 0;
    double train_loss = 0.0;
    std::optional<double> val_loss;
    EvalSummary val_summary;
    bool evaluated_ema = false;
};
struct TrainRunResult {
    ResolvedModelArtifacts artifacts;
    GpuAugmentationConfig gpu_augmentation;
    std::filesystem::path output_dir;
    std::filesystem::path checkpoint_path;
    std::optional<std::filesystem::path> best_checkpoint_path;
    std::optional<std::filesystem::path> best_regular_checkpoint_path;
    std::optional<std::filesystem::path> best_ema_checkpoint_path;
    bool best_is_ema = false;
    bool best_is_fallback = false;
    int last_epoch = -1;
    std::uint64_t completed_epochs = 0;
    std::inplace_vector<TrainEpochSummary, 32> history;
    std::optional<EvalSummary> test_summary;
};
[[nodiscard]] TrainRequest finalize_train_request(TrainRequest request);
TrainRunResult run_training(const TrainRequest& request);
void print_training_summary(const TrainRequest& request, const TrainRunResult& result);
}  // namespace mmltk::backend::models::rfdetr
