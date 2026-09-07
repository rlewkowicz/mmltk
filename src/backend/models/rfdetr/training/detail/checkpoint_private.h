#pragma once

#include <filesystem>
#include <vector>

#include "model_technical.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "torch_api.h"

namespace mmltk::backend::models::rfdetr::detail {

mmltk::backend::ml::torch_api::Tensor prepare_tensor_for_checkpoint_write(const mmltk::backend::ml::torch_api::Tensor& tensor);
void write_native_checkpoint_metadata(mmltk::backend::ml::torch_api::OutputArchive& archive, const NativeCheckpointMetadata& metadata);
void write_state_archive(mmltk::backend::ml::torch_api::OutputArchive& archive, const char* key,
                         const std::vector<NormalizedModelStateEntry>& entries);
void write_resume_state_archive(mmltk::backend::ml::torch_api::OutputArchive& archive, const char* key,
                                const std::vector<NormalizedModelStateEntry>& entries);
void write_training_supervision_config(mmltk::backend::ml::torch_api::OutputArchive& archive, const TrainingSupervisionConfig& config);
[[nodiscard]] TrainingSupervisionConfig read_training_supervision_config(mmltk::backend::ml::torch_api::InputArchive& archive);
void require_resume_training_supervision_config(const std::filesystem::path& checkpoint_path, const TrainingSupervisionConfig& expected);

}  // namespace mmltk::backend::models::rfdetr::detail
