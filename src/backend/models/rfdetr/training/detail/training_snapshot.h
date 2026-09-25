#pragma once
#include <span>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <limits>
#include <optional>
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/ml/cuda/tensor_readback.h"
#include "native_optimizer_private.h"
#include "model_ema.h"
#include "training_continuation.h"
#include "training_ops_private.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] std::vector<NormalizedModelStateEntry> collect_module_state(const NativeRfDetrModel&);
struct ResumeState {
 std::string attempt_id;
 int start_epoch = 0;
 // Retained CPU admission; materialize only after the complete model broadcast.
 std::optional<std::vector<torch::Tensor>> ema_cpu_shadow;
 std::optional<NativeOptimizer> optimizer_candidate;
 std::optional<float> scaler_scale;
 std::optional<int> scaler_growth_tracker;
};
ModelStateLoadSummary load_training_model_weights(NativeRfDetrModel&, const DecodedNativeModelState&, TrainingSupervisionRoute);
ResumeState load_resume_checkpoint_state(const std::filesystem::path&, const DecodedNativeModelState&, const detail::TrainingContinuation&, NativeOptimizer&,
 const std::vector<std::string>&, const std::vector<torch::Tensor>&, std::span<const std::uint8_t> active = {});
class TrainingSnapshot final {
public:
 void begin(const NativeRfDetrModel& model);
 void prepare_ema(const std::vector<std::string>& names, const ModelEma* ema);
 void save_weights(const std::filesystem::path&, const NativeCheckpointMetadata&, bool selected, const std::filesystem::path&);
 void save_resume(const std::filesystem::path&, const NativeCheckpointMetadata&, const NativeOptimizer&, const GradScaler&, const TrainRequest&, int epoch,
  int64_t ema_completed_updates, std::string_view attempt_id, const std::filesystem::path& descriptor, detail::TrainingContinuationValues& continuation);
 void release();

private:
 mmltk::backend::ml::cuda::TensorReadbackBuffers readback_;
 std::vector<NormalizedModelStateEntry> ordinary_;
 std::vector<NormalizedModelStateEntry> ema_;
};
}  // namespace mmltk::backend::models::rfdetr
