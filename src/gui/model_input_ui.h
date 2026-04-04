#pragma once

#include "gui/view_state.h"

#include <cstdint>
#include <string>

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
    const char* allow_fp16_label = "FP16";
    const char* progress_bar_label = "Progress";
    const char* compile_mode_label = "Compile Mode";
};

[[nodiscard]] const char* compilation_mode_label(int index) noexcept;
[[nodiscard]] const char* model_input_label(ModelInputMode mode) noexcept;
[[nodiscard]] bool draw_compile_mode_combo(const char* label, int& index);
void draw_dataset_paths_section(DatasetPathState& state,
                                const DatasetPathSectionConfig& config = {});
void draw_validate_workflow_sections(DatasetPathState& dataset_state,
                                     ModelArtifactSelectionState& artifact_state,
                                     ExecutionTuningState& execution_state,
                                     std::string& report_json_path,
                                     std::string& save_engine_path,
                                     std::string& eval_order,
                                     std::string& split,
                                     int& resolution,
                                     int& limit_images,
                                     int& alignment_images,
                                     int& eval_max_dets,
                                     int& batch_size,
                                     int& prefetch_factor,
                                     bool& recompile,
                                     bool& profile,
                                     bool& write_report_json);
[[nodiscard]] ModelInputBrowseRequest draw_model_input_section(
    ModelInputMode& mode,
    ModelArtifactSelectionState& state,
    bool weights_browse_busy,
    bool onnx_browse_busy,
    bool tensorrt_browse_busy,
    const ModelInputSectionConfig& config = {});
[[nodiscard]] ModelInputBrowseRequest draw_model_input_selector(
    ModelInputMode& mode,
    std::string& weights_path,
    std::string& onnx_path,
    std::string& tensorrt_path,
    bool weights_browse_busy,
    bool onnx_browse_busy,
    bool tensorrt_browse_busy,
    bool allow_none = false);
void draw_execution_tuning_section(ExecutionTuningState& state,
                                   const ExecutionTuningSectionConfig& config = {});
[[nodiscard]] ModelInputBrowseRequest draw_predict_workflow_sections(
    ModelInputMode& model_input,
    ModelArtifactSelectionState& artifact_state,
    ExecutionTuningState& execution_state,
    std::string& backend,
    std::string& output_path,
    int& batch_size,
    int& live_split_count,
    int& max_dets_per_image,
    float& threshold,
    bool live_video,
    bool controls_disabled,
    bool single_image,
    bool weights_browse_busy,
    bool onnx_browse_busy,
    bool tensorrt_browse_busy);
void draw_predict_status_sections(bool live_video,
                                  const std::string& source_error,
                                  const std::string& live_blocker,
                                  const std::string& combo_error,
                                  const std::string& live_start_error,
                                  bool job_running);
[[nodiscard]] PredictActionFooterResult draw_predict_action_footer(
    bool live_video,
    bool predict_session_active,
    bool starting,
    bool stopping,
    bool block_live_start,
    bool block_batch_prediction);

} // namespace mmltk::gui
