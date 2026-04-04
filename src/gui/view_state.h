#pragma once

#include "../rfdetr/train_recipe.h"
#include "source_selection.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mmltk::gui {

enum class View : std::uint8_t {
    Train = 0,
    Validate = 1,
    Predict = 2,
    Annotate = 3,
    Export = 4,
    Live = 5,
};

enum class TrainInputMode : std::uint8_t {
    Weights = 0,
    Resume = 1,
};

enum class TrainOptimizerMode : std::uint8_t {
    AdamW = 0,
    Muon = 1,
};

enum class TrainExecutionTarget : std::uint8_t {
    Local = 0,
    Remote = 1,
};

enum class ModelInputMode : std::uint8_t {
    Weights = 0,
    Onnx = 1,
    TensorRt = 2,
    None = 3,
};

enum class UiDensity : std::uint8_t {
    Compact = 0,
    Balanced = 1,
    Comfortable = 2,
};

struct UiSettingsState {
    float ui_scale = 1.0f;
    float font_size = 16.0f;
    float secondary_font_size = 14.0f;
    float mono_font_size = 14.0f;
    float property_label_width = 156.0f;
    float crop_edge_hit_half_width = 8.0f;
    float crop_corner_hit_size = 20.0f;
    float crop_handle_radius = 6.0f;
    UiDensity density = UiDensity::Balanced;
    float content_scale = 1.0f; // auto-detected from glfwGetWindowContentScale, not serialized
};

struct ModelArtifactSelectionState {
    std::string weights_path;
    std::string onnx_path;
    std::string tensorrt_path;
};

struct DatasetPathState {
    std::string compiled_path;
    std::string source_dir;
    std::string train_compiled_path;
    std::string val_compiled_path;
    std::string test_compiled_path;
};

struct ExecutionTuningState {
    std::string cpu_affinity;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    bool allow_fp16 = true;
    bool progress_bar = false;
    int compile_mode = 1;
};

struct TrainPaneState {
    std::string output_dir = "./gui-train-output";
    std::string resume_path;
    TrainInputMode input_mode = TrainInputMode::Weights;
    TrainExecutionTarget execution_target = TrainExecutionTarget::Local;
    std::vector<int> local_device_ids{0};
    std::array<bool, 5> remote_family_enabled{{true, true, true, true, true}};
};

struct TrainViewState {
    std::string train_compiled_path;
    std::string val_compiled_path;
    std::string test_compiled_path;
    std::string output_dir = "./gui-train-output";
    std::string weights_path;
    std::string resume_path;
    std::string cpu_affinity;
    TrainInputMode input_mode = TrainInputMode::Weights;
    int batch_size = 2;
    int val_batch_size = 0;
    int epochs = 12;
    int grad_accum_steps = 1;
    int eval_max_dets = 500;
    int lr_drop = 100;
    int print_freq = 100;
    int prefetch_factor = 2;
    int seed = 42;
    int workers = 8;
    int lanes = 0;
    double lr = 1.0e-4;
    double lr_encoder = 1.5e-4;
    double lr_component_decay = 0.7;
    double encoder_layer_decay = 0.8;
    double momentum = 0.95;
    double weight_decay = 1.0e-4;
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    double lr_min_factor = 0.0;
    double clip_max_norm = 0.1;
    std::string lr_scheduler = "step";
    bool use_ema = false;
    bool amp = true;
    bool progress_bar = false;
    bool freeze_encoder = false;
    TrainOptimizerMode optimizer = TrainOptimizerMode::AdamW;
    int compile_mode = 1;
    TrainExecutionTarget execution_target = TrainExecutionTarget::Local;
    std::vector<int> local_device_ids{0};
    std::array<bool, 5> remote_family_enabled{{true, true, true, true, true}};
    mmltk::rfdetr::TrainRecipeFieldOverrides recipe_overrides;
};

struct ValidateViewState {
    std::string compiled_path;
    std::string source_dir;
    std::string onnx_path;
    std::string tensorrt_path;
    std::string save_engine_path;
    std::string report_json_path = "./rfdetr-validation-report.json";
    std::string split = "val";
    std::string eval_order = "onnx,tensorrt";
    std::string cpu_affinity;
    int resolution = 432;
    int limit_images = 0;
    int alignment_images = 16;
    int eval_max_dets = 500;
    int batch_size = 1;
    int prefetch_factor = 2;
    int device_id = 0;
    int workers = 0;
    bool recompile = false;
    bool profile = false;
    bool allow_fp16 = true;
    bool write_report_json = true;
};

struct PredictViewState {
    SourceSelectionState source;
    std::string weights_path;
    std::string onnx_path;
    std::string tensorrt_path;
    std::string output_path = "./predictions.json";
    std::string backend = "auto";
    std::string cpu_affinity;
    ModelInputMode model_input = ModelInputMode::Weights;
    int batch_size = 4;
    int max_dets_per_image = 500;
    int live_split_count = 1;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    float threshold = 0.25f;
    bool allow_fp16 = true;
    bool progress_bar = false;
    int compile_mode = 1;
};

struct AnnotateViewState {
    SourceSelectionState source;
    std::string weights_path;
    std::string onnx_path;
    std::string tensorrt_path;
    std::string output_dir = "./annotated-scenes";
    std::string split = "train";
    std::string backend = "auto";
    ModelInputMode model_input = ModelInputMode::None;
    int device_id = 0;
    int max_dets_per_image = 300;
    float threshold = 0.25f;
    bool allow_fp16 = true;
    bool full_frame = false;
    int compile_mode = 1;
};

struct ExportViewState {
    std::string weights_path;
    std::string onnx_path;
    std::string output_path = "./rfdetr-engine.trt";
    int device_id = 0;
    int opset_version = 19;
    bool allow_fp16 = true;
    bool build_tensorrt = true;
    bool simplify = false;
};

inline DatasetPathState dataset_paths(const TrainViewState& s) {
    return DatasetPathState{
        std::string{},
        std::string{},
        s.train_compiled_path,
        s.val_compiled_path,
        s.test_compiled_path,
    };
}

inline void apply_dataset_paths(TrainViewState& s, const DatasetPathState& dataset_state) {
    s.train_compiled_path = dataset_state.train_compiled_path;
    s.val_compiled_path = dataset_state.val_compiled_path;
    s.test_compiled_path = dataset_state.test_compiled_path;
}

inline ModelArtifactSelectionState model_artifacts(const TrainViewState& s) {
    return ModelArtifactSelectionState{s.weights_path, std::string{}, std::string{}};
}

inline void apply_model_artifacts(TrainViewState& s, const ModelArtifactSelectionState& artifact_state) {
    s.weights_path = artifact_state.weights_path;
}

inline ExecutionTuningState execution_tuning(const TrainViewState& s) {
    return ExecutionTuningState{
        s.cpu_affinity,
        0,
        s.workers,
        s.lanes,
        true,
        s.progress_bar,
        s.compile_mode,
    };
}

inline void apply_execution_tuning(TrainViewState& s, const ExecutionTuningState& execution_state) {
    s.cpu_affinity = execution_state.cpu_affinity;
    s.workers = execution_state.workers;
    s.lanes = execution_state.lanes;
    s.progress_bar = execution_state.progress_bar;
    s.compile_mode = execution_state.compile_mode;
}

inline TrainPaneState train_pane_state(const TrainViewState& s) {
    return TrainPaneState{
        s.output_dir,
        s.resume_path,
        s.input_mode,
        s.execution_target,
        s.local_device_ids,
        s.remote_family_enabled,
    };
}

inline void apply_train_pane_state(TrainViewState& s, const TrainPaneState& pane_state) {
    s.output_dir = pane_state.output_dir;
    s.resume_path = pane_state.resume_path;
    s.input_mode = pane_state.input_mode;
    s.execution_target = pane_state.execution_target;
    s.local_device_ids = pane_state.local_device_ids;
    s.remote_family_enabled = pane_state.remote_family_enabled;
}

inline DatasetPathState dataset_paths(const ValidateViewState& s) {
    return DatasetPathState{
        s.compiled_path,
        s.source_dir,
        std::string{},
        std::string{},
        std::string{},
    };
}

inline void apply_dataset_paths(ValidateViewState& s, const DatasetPathState& dataset_state) {
    s.compiled_path = dataset_state.compiled_path;
    s.source_dir = dataset_state.source_dir;
}

inline ModelArtifactSelectionState model_artifacts(const ValidateViewState& s) {
    return ModelArtifactSelectionState{std::string{}, s.onnx_path, s.tensorrt_path};
}

inline void apply_model_artifacts(ValidateViewState& s, const ModelArtifactSelectionState& artifact_state) {
    s.onnx_path = artifact_state.onnx_path;
    s.tensorrt_path = artifact_state.tensorrt_path;
}

inline ExecutionTuningState execution_tuning(const ValidateViewState& s) {
    return ExecutionTuningState{
        s.cpu_affinity,
        s.device_id,
        s.workers,
        0,
        s.allow_fp16,
        false,
        1,
    };
}

inline void apply_execution_tuning(ValidateViewState& s, const ExecutionTuningState& execution_state) {
    s.cpu_affinity = execution_state.cpu_affinity;
    s.device_id = execution_state.device_id;
    s.workers = execution_state.workers;
    s.allow_fp16 = execution_state.allow_fp16;
}

inline ModelArtifactSelectionState model_artifacts(const PredictViewState& s) {
    return ModelArtifactSelectionState{s.weights_path, s.onnx_path, s.tensorrt_path};
}

inline void apply_model_artifacts(PredictViewState& s, const ModelArtifactSelectionState& artifact_state) {
    s.weights_path = artifact_state.weights_path;
    s.onnx_path = artifact_state.onnx_path;
    s.tensorrt_path = artifact_state.tensorrt_path;
}

inline ExecutionTuningState execution_tuning(const PredictViewState& s) {
    return ExecutionTuningState{
        s.cpu_affinity,
        s.device_id,
        s.workers,
        s.lanes,
        s.allow_fp16,
        s.progress_bar,
        s.compile_mode,
    };
}

inline void apply_execution_tuning(PredictViewState& s, const ExecutionTuningState& execution_state) {
    s.cpu_affinity = execution_state.cpu_affinity;
    s.device_id = execution_state.device_id;
    s.workers = execution_state.workers;
    s.lanes = execution_state.lanes;
    s.allow_fp16 = execution_state.allow_fp16;
    s.progress_bar = execution_state.progress_bar;
    s.compile_mode = execution_state.compile_mode;
}

inline ModelArtifactSelectionState model_artifacts(const AnnotateViewState& s) {
    return ModelArtifactSelectionState{s.weights_path, s.onnx_path, s.tensorrt_path};
}

inline void apply_model_artifacts(AnnotateViewState& s, const ModelArtifactSelectionState& artifact_state) {
    s.weights_path = artifact_state.weights_path;
    s.onnx_path = artifact_state.onnx_path;
    s.tensorrt_path = artifact_state.tensorrt_path;
}

inline ExecutionTuningState execution_tuning(const AnnotateViewState& s) {
    return ExecutionTuningState{
        std::string{},
        s.device_id,
        0,
        0,
        s.allow_fp16,
        false,
        s.compile_mode,
    };
}

inline void apply_execution_tuning(AnnotateViewState& s, const ExecutionTuningState& execution_state) {
    s.device_id = execution_state.device_id;
    s.allow_fp16 = execution_state.allow_fp16;
    s.compile_mode = execution_state.compile_mode;
}

inline ModelArtifactSelectionState model_artifacts(const ExportViewState& s) {
    return ModelArtifactSelectionState{s.weights_path, s.onnx_path, std::string{}};
}

inline void apply_model_artifacts(ExportViewState& s, const ModelArtifactSelectionState& artifact_state) {
    s.weights_path = artifact_state.weights_path;
    s.onnx_path = artifact_state.onnx_path;
}

inline ExecutionTuningState execution_tuning(const ExportViewState& s) {
    return ExecutionTuningState{
        std::string{},
        s.device_id,
        0,
        0,
        s.allow_fp16,
        false,
        1,
    };
}

inline void apply_execution_tuning(ExportViewState& s, const ExecutionTuningState& execution_state) {
    s.device_id = execution_state.device_id;
    s.allow_fp16 = execution_state.allow_fp16;
}

} // namespace mmltk::gui
