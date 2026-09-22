#pragma once
#include <limits>
#include <optional>
#include <unordered_map>
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "native_optimizer_private.h"
#include "model_ema.h"
#include "training_continuation.h"
#include "training_ops_private.h"
namespace mmltk::backend::models::rfdetr {
struct ResumeState {
 std::string attempt_id;
 int start_epoch = 0;
 double best_regular = -std::numeric_limits<double>::infinity();
 double best_ema = -std::numeric_limits<double>::infinity();
 std::optional<ModelEma> restored_ema;
 std::optional<NativeOptimizer> optimizer_candidate;
 std::optional<float> scaler_scale;
 std::optional<int> scaler_growth_tracker;
};
ModelStateLoadSummary load_training_model_weights(NativeRfDetrModel&, const DecodedNativeModelState&, TrainingSupervisionRoute);
ResumeState load_resume_checkpoint_state(const std::filesystem::path&, DecodedNativeModelState&, const detail::TrainingContinuation&, NativeOptimizer&, const TrainRequest&,
 const std::vector<std::string>&, const std::vector<torch::Tensor>&, bool);
void save_collected_checkpoint(const std::filesystem::path&, const NativeCheckpointMetadata&, const NativeRfDetrModel&, const std::unordered_map<std::string, torch::Tensor>*, const char*, const char*,
 const std::filesystem::path&);
std::unordered_map<std::string, torch::Tensor> ema_override_map(const std::vector<std::string>&, const ModelEma&);
class TrainingSnapshot final {
public:
 void begin(const NativeRfDetrModel& model);
 void prepare_ema(const std::vector<std::string>& names, const ModelEma* ema);
 void save_weights(const std::filesystem::path&, const NativeCheckpointMetadata&, bool selected, const std::filesystem::path&);
 void save_resume(const std::filesystem::path&, const NativeCheckpointMetadata&, const NativeOptimizer&, const GradScaler&, const TrainRequest&, int epoch, double best_regular, double best_ema,
  int64_t ema_completed_updates, std::string_view attempt_id, const std::filesystem::path& descriptor);
 void release();

private:
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback_;
 std::vector<NormalizedModelStateEntry> ordinary_;
 std::vector<NormalizedModelStateEntry> ema_;
};
}  // namespace mmltk::backend::models::rfdetr
