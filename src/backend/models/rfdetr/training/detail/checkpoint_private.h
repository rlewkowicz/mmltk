#pragma once
#include <filesystem>
#include <span>
#include <string>
#include "src/backend/ml/cuda/tensor_readback.h"
#include <vector>
#include "model_technical.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "torch_api.h"
namespace mmltk::backend::models::rfdetr::detail {
void reserve_state_archive(const std::vector<NormalizedModelStateEntry>& entries, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                           std::size_t first_slot);
void publish_native_checkpoint_archive(mmltk::backend::ml::torch_api::OutputArchive& archive, const std::filesystem::path& destination,
                                       const std::filesystem::path& explicit_descriptor = {});
void write_native_checkpoint_metadata(mmltk::backend::ml::torch_api::OutputArchive& archive, const NativeCheckpointMetadata& metadata);
void write_state_archive(mmltk::backend::ml::torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                         mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot);
void write_resume_state_archive(mmltk::backend::ml::torch_api::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                                mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot);
void write_training_supervision_config(mmltk::backend::ml::torch_api::OutputArchive& archive, const TrainingSupervisionConfig& config);
[[nodiscard]] TrainingSupervisionConfig read_training_supervision_config(mmltk::backend::ml::torch_api::InputArchive& archive);
void require_resume_training_supervision_config(mmltk::backend::ml::torch_api::InputArchive& archive, const TrainingSupervisionConfig& expected);
[[nodiscard]] std::vector<mmltk::backend::ml::torch_api::Tensor> read_ema_shadow_archive(mmltk::backend::ml::torch_api::InputArchive& ema_archive,
                                                                                         std::span<const std::string> expected_names);
}  // namespace mmltk::backend::models::rfdetr::detail
