#pragma once
#include <stop_token>
#include <filesystem>
#include <span>
#include <string>
#include "src/backend/ml/cuda/tensor_readback.h"
#include <vector>
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/training_supervision.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include <torch/types.h>
#include <torch/serialize.h>
namespace mmltk::backend::models::rfdetr::detail {
void reserve_state_archive(const std::vector<NormalizedModelStateEntry>& entries, mmltk::backend::ml::cuda::TensorReadbackBuffers& readback,
                           std::size_t first_slot);
void publish_native_checkpoint_archive(torch::serialize::OutputArchive& archive, const std::filesystem::path& destination,
                                       const std::filesystem::path& explicit_descriptor = {});
void write_native_checkpoint_metadata(torch::serialize::OutputArchive& archive, const NativeCheckpointMetadata& metadata);
void write_state_archive(torch::serialize::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                         mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot);
void write_resume_state_archive(torch::serialize::OutputArchive& archive, const char* key, const std::vector<NormalizedModelStateEntry>& entries,
                                mmltk::backend::ml::cuda::TensorReadbackBuffers& readback, std::size_t first_slot);
void write_training_supervision_config(torch::serialize::OutputArchive& archive, const TrainingSupervisionConfig& config);
[[nodiscard]] TrainingSupervisionConfig read_training_supervision_config(torch::serialize::InputArchive& archive);
void require_resume_training_supervision_config(torch::serialize::InputArchive& archive, const TrainingSupervisionConfig& expected);
[[nodiscard]] std::vector<torch::Tensor> read_ema_shadow_archive(torch::serialize::InputArchive& ema_archive, std::span<const std::string> expected_names, std::stop_token stop = {});
}  // namespace mmltk::backend::models::rfdetr::detail
