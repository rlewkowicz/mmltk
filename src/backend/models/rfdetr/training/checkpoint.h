#pragma once
#include "src/backend/models/rfdetr/contract/training_metrics.h"

namespace mmltk::backend::models::rfdetr {
// CPU archive admission only: no model, zero-state allocation, or CUDA initialization.
[[nodiscard]] TrainingCheckpoint inspect_training_checkpoint(const std::filesystem::path&);
}  // namespace mmltk::backend::models::rfdetr
