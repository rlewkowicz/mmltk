#pragma once

#include "annotation_core.h"
#include "still_image_preview.h"
#include "view_state.h"

#include "mmltk/rfdetr/live_predict.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/validate.h"
#include "mmltk/rfdetr/workflow_requests.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui::rfdetr_workflows {

std::size_t require_positive_size(int value, const char* field_name);
std::size_t require_non_negative_size(int value, const char* field_name);
int require_non_negative_int(int value, const char* field_name);
int require_positive_int(int value, const char* field_name);

mmltk::rfdetr::CompilationMode compilation_mode_from_index(int index);

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(const ExportViewState& state);
mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(const ExportViewState& state);

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const ExportViewState& state,
    const std::filesystem::path& onnx_path_override);

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(
    const ExportViewState& state,
    const std::filesystem::path& output_path_override);

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const mmltk::rfdetr::ExportOnnxRequest& export_request,
    const ExportViewState& state);

mmltk::rfdetr::BuildEngineRequest build_build_engine_request(
    const mmltk::rfdetr::ExportOnnxRequest& export_request,
    const std::filesystem::path& output_path,
    int device_id,
    bool allow_fp16);

mmltk::rfdetr::ExportOnnxRequest build_export_onnx_request(
    const std::string& weights_path,
    const std::string& output_path,
    int device_id,
    int opset_version,
    bool simplify);

void apply_build_engine_request(ExportViewState& state,
                                const mmltk::rfdetr::BuildEngineRequest& request);
void apply_export_onnx_request(ExportViewState& state,
                               const mmltk::rfdetr::ExportOnnxRequest& request);
void apply_predict_request(PredictViewState& state,
                           const mmltk::rfdetr::PredictRequest& request);
void apply_validate_request(ValidateViewState& state,
                            const mmltk::rfdetr::ValidateRequest& request);
void apply_train_request(TrainViewState& state,
                         const mmltk::rfdetr::TrainRequest& request);

mmltk::rfdetr::PredictOptions build_annotate_predict_options(
    const AnnotateViewState& state,
    const std::string& preset_name,
    mmltk::rfdetr::PredictImageInput input);

mmltk::rfdetr::ValidateRequest build_validate_request(const ValidateViewState& state);
mmltk::rfdetr::PredictRequest build_predict_request(
    const PredictViewState& state,
    std::vector<mmltk::rfdetr::PredictImageInput> image_inputs = {});
mmltk::rfdetr::LivePredictOptions build_live_predict_options(const PredictViewState& state,
                                                             const std::string& preset_name);
mmltk::rfdetr::TrainRequest build_train_request(const TrainViewState& state,
                                                const std::vector<int>& device_ids);

std::string summarize_annotation_save_result(const AnnotationSaveResult& result);
std::string summarize_validation_result(const mmltk::rfdetr::ValidationRunResult& result);
std::string summarize_prediction_result(const mmltk::rfdetr::PredictionRunResult& result);

std::optional<StillImagePreview> maybe_make_single_image_preview(
    const PredictViewState& state,
    const mmltk::rfdetr::PredictOptions& options,
    const mmltk::rfdetr::PredictionRunResult& result);

AnnotationBox prediction_to_annotation_box(const mmltk::rfdetr::Prediction& prediction,
                                           std::uint32_t width,
                                           std::uint32_t height);

} // namespace mmltk::gui::rfdetr_workflows
