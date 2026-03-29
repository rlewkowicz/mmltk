#pragma once

#include "annotation_core.h"
#include "still_image_preview.h"
#include "train_command.h"
#include "view_state.h"

#include "fastloader/rfdetr/live_predict.h"
#include "fastloader/rfdetr/predict.h"
#include "fastloader/rfdetr/validate.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastloader::gui::rfdetr_workflows {

std::size_t require_positive_size(int value, const char* field_name);
std::size_t require_non_negative_size(int value, const char* field_name);
int require_non_negative_int(int value, const char* field_name);
int require_positive_int(int value, const char* field_name);

fastloader::rfdetr::CompilationMode compilation_mode_from_index(int index);
std::string compilation_mode_cli_value(int index);

fastloader::rfdetr::PredictOptions build_annotate_predict_options(
    const AnnotateViewState& state,
    const std::string& preset_name,
    fastloader::rfdetr::PredictImageInput input);

fastloader::rfdetr::ValidationOptions build_validate_options(const ValidateViewState& state);
fastloader::rfdetr::PredictOptions build_predict_options(const PredictViewState& state,
                                                         const std::string& preset_name);
fastloader::rfdetr::LivePredictOptions build_live_predict_options(const PredictViewState& state,
                                                                  const std::string& preset_name);
TrainCommandConfig build_train_command_config(const TrainViewState& state,
                                              const std::vector<int>& device_ids);

std::string summarize_annotation_save_result(const AnnotationSaveResult& result);
std::string summarize_validation_result(const fastloader::rfdetr::ValidationRunResult& result);
std::string summarize_prediction_result(const fastloader::rfdetr::PredictionRunResult& result);

std::optional<StillImagePreview> maybe_make_single_image_preview(
    const PredictViewState& state,
    const fastloader::rfdetr::PredictOptions& options,
    const fastloader::rfdetr::PredictionRunResult& result);

AnnotationBox prediction_to_annotation_box(const fastloader::rfdetr::Prediction& prediction,
                                           std::uint32_t width,
                                           std::uint32_t height);

} // namespace fastloader::gui::rfdetr_workflows
