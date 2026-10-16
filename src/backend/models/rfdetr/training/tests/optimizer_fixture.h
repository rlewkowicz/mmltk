#pragma once
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include "src/backend/models/rfdetr/training/detail/native_optimizer_private.h"
namespace mmltk::backend::models::rfdetr::testsupport {
[[nodiscard]] inline OptimizerBuildResult unfused_recipe_optimizer(NativeRfDetrModel& model, const TrainRecipeValues& recipe) {
 TrainRequest request;
 static_cast<TrainRecipeValues&>(request.recipe) = recipe;
 request.fused_optimizer = false;
 return build_optimizer(model.named_optimizer_parameters(), request, model.config().backbone);
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
