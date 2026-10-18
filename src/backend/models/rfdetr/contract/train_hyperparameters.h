#pragma once
#include <cstddef>
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/frameworks/reflection/reflected_declarations.h"
namespace mmltk::backend::models::rfdetr {
// Shared by executable requests and the model catalog, preserving flat request identities.
struct TrainHyperparameters {
 bool operator==(const TrainHyperparameters&) const = default;
 MMLTK_MINIMUM(std::size_t, 1U) std::size_t batch_size = 1U;
 std::size_t val_batch_size = 0U;
 std::size_t num_queries = 0U;
 std::size_t eval_max_dets = 0U;
 MMLTK_MINIMUM(int, 1) int epochs = 1;
 MMLTK_MINIMUM(int, 1) int grad_accum_steps = 1;
 MMLTK_MINIMUM(int, 0) int ema_tau = 100;
 MMLTK_MINIMUM(double, 0.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Norm) double clip_max_norm = 0.1;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0) MMLTK_FINITE MMLTK_PRESENTATION(mmltk::frameworks::reflection::PresentationKind::Decay) double ema_decay = 0.993;
 bool use_ema = false;
 bool amp = true;
 bool freeze_encoder = false;
 bool fused_optimizer = true;
};
MMLTK_REFLECT_FIELDS(TrainHyperparameters)
struct PresetTrainingDefaults final : TrainHyperparameters {
 constexpr PresetTrainingDefaults() {
  batch_size = 4U;
  eval_max_dets = 500U;
  epochs = 100;
  use_ema = true;
 }
 // A catalog publishes values; mutable override state belongs to the request.
 TrainRecipeValues recipe = train_recipe(TrainOptimizerKind::AdamW);
 constexpr bool operator==(const PresetTrainingDefaults&) const = default;
};
MMLTK_REFLECT_FIELDS(PresetTrainingDefaults)
}  // namespace mmltk::backend::models::rfdetr
