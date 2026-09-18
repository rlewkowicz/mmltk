#pragma once
#include <filesystem>
#include <stop_token>
#include <memory>
#include <variant>
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::backend::models::rfdetr {
class NativeRfDetrModel;
class DecodedNativeModelState;
struct ModelStateLoadSummary;
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const DecodedNativeModelState& checkpoint, bool strict = true);
ModelStateLoadSummary apply_checkpoint_to_module(NativeRfDetrModel& module, const std::filesystem::path& checkpoint_path, bool strict = true);
void save_native_checkpoint(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& checkpoint,
                            const std::filesystem::path& explicit_descriptor = {});
DecodedNativeModelState normalize_checkpoint_to_native(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                                       const std::filesystem::path& class_layout_path = {});
ModelStateLoadSummary load_model_weights(NativeRfDetrModel& model, const std::filesystem::path& weights_path, bool strict = false);
// Immutable facts and exact evidence; no decoded archive or tensor custody.
class TrainingCheckpointAdmission final {
   public:
    TrainingCheckpointAdmission(TrainingCheckpoint, std::shared_ptr<const ClassArtifactAdmission>);
    TrainingCheckpointAdmission(TrainingCheckpoint, mmltk::common::io::FileSnapshot);
    [[nodiscard]] const TrainingCheckpoint& checkpoint() const noexcept { return checkpoint_; }
    void RequireUnchanged(std::stop_token stop = {}) const;

   private:
    TrainingCheckpoint checkpoint_;
    std::variant<std::shared_ptr<const ClassArtifactAdmission>, mmltk::common::io::FileSnapshot> evidence_;
};
// CPU archive admission only: no model, zero-state allocation, or CUDA initialization.
[[nodiscard]] TrainingCheckpointAdmission inspect_training_checkpoint(const std::filesystem::path&, std::stop_token stop = {});
}  // namespace mmltk::backend::models::rfdetr
