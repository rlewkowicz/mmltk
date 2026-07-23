#pragma once

#include "gui/source_runtime.h"
#include "gui/view_state.h"
#include "mmltk/rfdetr/live_predict.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace mmltk::gui {

enum class ModelInputBrowseRequest : std::uint8_t {
    None = 0,
    Weights = 1,
    Onnx = 2,
    TensorRt = 3,
};

enum class PredictActionFooterResult : std::uint8_t {
    None = 0,
    StartLive = 1,
    StopLive = 2,
    RunBatchPrediction = 3,
};

struct PredictUiState {
    bool live_video = false;
    std::string source_error;
    std::string combo_error;
    std::string live_blocker;
    bool block_live_start = false;
    bool block_batch_prediction = false;
};

struct DatasetPathSectionConfig {
    const char* heading = "Datasets";
    const char* compiled_label = nullptr;
    const char* source_dir_label = nullptr;
    const char* train_compiled_label = nullptr;
    const char* val_compiled_label = nullptr;
    const char* test_compiled_label = nullptr;
};

struct ModelInputSectionConfig {
    const char* heading = "Model";
    bool allow_none = false;
};

struct ExecutionTuningSectionConfig {
    const char* heading = "Execution";
    float input_width = 120.0f;
    bool show_cpu_affinity = true;
    bool show_device_id = true;
    bool show_workers = true;
    bool show_lanes = true;
    bool show_allow_fp16 = true;
    bool show_progress_bar = false;
    bool show_compile_mode = true;
    const char* device_id_label = "Device ID";
    const char* workers_label = "Workers";
    const char* lanes_label = "Lanes";
    const char* allow_fp16_label = "Mixed Precision";
    const char* progress_bar_label = "Progress";
    const char* compile_mode_label = "Compile Mode";
};

[[nodiscard]] const char* compilation_mode_label(int index) noexcept;
[[nodiscard]] const char* compilation_mode_label(mmltk::rfdetr::CompilationMode mode) noexcept;
[[nodiscard]] const char* model_input_label(ModelInputMode mode) noexcept;
[[nodiscard]] bool draw_compile_mode_combo(const char* label, mmltk::rfdetr::CompilationMode& mode);
[[nodiscard]] ExecutionTuningSectionConfig train_execution_tuning_section_config(
    const char* heading = nullptr) noexcept;
[[nodiscard]] ExecutionTuningSectionConfig export_execution_tuning_section_config(
    const char* heading = nullptr) noexcept;
void draw_dataset_paths_section(DatasetPathState& state, const DatasetPathSectionConfig& config = {});
void draw_validate_execution_fields(ExecutionTuningState& execution_state, std::string& split, int& resolution,
                                    int& batch_size, int& limit_images, int& alignment_images, int& eval_max_dets,
                                    int& prefetch_factor, bool& recompile, bool& profile, bool& write_report_json,
                                    const char* heading = "Execution");
[[nodiscard]] ModelInputBrowseRequest draw_predict_model_fields(
    ModelInputMode& model_input, ModelArtifactSelectionState& artifact_state, std::string& backend,
    std::string& output_path, bool live_video, bool controls_disabled, bool weights_browse_busy, bool onnx_browse_busy,
    bool tensorrt_browse_busy, const ModelInputSectionConfig& config = {});
[[nodiscard]] ModelInputBrowseRequest draw_model_input_section(ModelInputMode& mode, ModelArtifactSelectionState& state,
                                                               bool weights_browse_busy, bool onnx_browse_busy,
                                                               bool tensorrt_browse_busy,
                                                               const ModelInputSectionConfig& config = {});
[[nodiscard]] ModelInputBrowseRequest draw_model_input_selector(ModelInputMode& mode, std::string& weights_path,
                                                                std::string& onnx_path, std::string& tensorrt_path,
                                                                bool weights_browse_busy, bool onnx_browse_busy,
                                                                bool tensorrt_browse_busy, bool allow_none = false);
void draw_execution_tuning_section(ExecutionTuningState& state, const ExecutionTuningSectionConfig& config = {});
void draw_predict_execution_fields(ExecutionTuningState& execution_state, int& batch_size, int& live_split_count,
                                   int& max_dets_per_image, float& threshold, bool live_video, bool single_image,
                                   bool controls_disabled, const char* heading = "Execution");
[[nodiscard]] ModelInputBrowseRequest draw_predict_workflow_sections(
    ModelInputMode& model_input, ModelArtifactSelectionState& artifact_state, ExecutionTuningState& execution_state,
    std::string& backend, std::string& output_path, int& batch_size, int& live_split_count, int& max_dets_per_image,
    float& threshold, bool live_video, bool controls_disabled, bool single_image, bool weights_browse_busy,
    bool onnx_browse_busy, bool tensorrt_browse_busy);

[[nodiscard]] inline std::string predict_combo_error(const PredictViewState& state) {
    const bool raw_source =
        state.source.kind == SourceKind::SingleImage || state.source.kind == SourceKind::ImageFolder;
    if (raw_source && state.model_input == ModelInputMode::Weights && state.lanes > 1) {
        return "Weights + raw-image predict requires lanes <= 1.";
    }
    return {};
}

[[nodiscard]] inline std::string live_predict_blocker(const PredictViewState& state, bool job_running,
                                                      std::string_view job_label) {
    if (state.source.kind != SourceKind::VideoStream) {
        return {};
    }
    if (job_running) {
        return job_label.empty()
                   ? "Finish the active workflow before starting live prediction."
                   : "Finish the active workflow before starting live prediction: " + std::string(job_label) + ".";
    }
    std::string source_error = validate_predict_source(state.source);
    if (!source_error.empty()) {
        return source_error;
    }
    if (!mmltk::rfdetr::live_capture_supported()) {
        return "RF-DETR live video predict is not available in this build.";
    }
    return {};
}

[[nodiscard]] inline PredictUiState evaluate_predict_ui_state(const PredictViewState& state, bool job_running,
                                                              std::string_view job_label, bool live_predict_running,
                                                              bool annotate_live_active) {
    PredictUiState ui_state;
    ui_state.live_video = state.source.kind == SourceKind::VideoStream;
    ui_state.source_error = validate_predict_source(state.source);
    ui_state.combo_error = ui_state.live_video ? std::string{} : predict_combo_error(state);
    ui_state.live_blocker = ui_state.live_video ? live_predict_blocker(state, job_running, job_label) : std::string{};
    ui_state.block_live_start = !ui_state.live_blocker.empty() || annotate_live_active;
    ui_state.block_batch_prediction =
        live_predict_running || job_running || !ui_state.source_error.empty() || !ui_state.combo_error.empty();
    return ui_state;
}

[[nodiscard]] inline bool predict_status_ready(const PredictUiState& state,
                                               std::string_view live_start_error) noexcept {
    return state.source_error.empty() && state.combo_error.empty() && state.live_blocker.empty() &&
           live_start_error.empty();
}

[[nodiscard]] inline const char* predict_ready_message(const PredictUiState& state) noexcept {
    return state.live_video ? "Ready to start live prediction." : "Ready to run prediction.";
}

void draw_predict_status_sections(const PredictUiState& state, const std::string& live_start_error, bool job_running);
[[nodiscard]] PredictActionFooterResult draw_predict_action_footer(bool live_video, bool predict_session_active,
                                                                   bool starting, bool stopping, bool block_live_start,
                                                                   bool block_batch_prediction);

}  
