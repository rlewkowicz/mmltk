#pragma once

#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/validate.h"
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

[[nodiscard]] constexpr mmltk::rfdetr::TrainOptimizerKind train_optimizer_kind(const TrainOptimizerMode mode) noexcept {
    switch (mode) {
        case TrainOptimizerMode::AdamW:
            return mmltk::rfdetr::TrainOptimizerKind::AdamW;
        case TrainOptimizerMode::Muon:
            return mmltk::rfdetr::TrainOptimizerKind::Muon;
    }
    return mmltk::rfdetr::TrainOptimizerKind::AdamW;
}

[[nodiscard]] constexpr TrainOptimizerMode train_optimizer_mode(const mmltk::rfdetr::TrainOptimizerKind kind) noexcept {
    switch (kind) {
        case mmltk::rfdetr::TrainOptimizerKind::AdamW:
            return TrainOptimizerMode::AdamW;
        case mmltk::rfdetr::TrainOptimizerKind::Muon:
            return TrainOptimizerMode::Muon;
    }
    return TrainOptimizerMode::AdamW;
}

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
    bool dark_mode = false;
    float ui_scale = 1.0f;
    float font_size = 16.0f;
    float secondary_font_size = 14.0f;
    float mono_font_size = 14.0f;
    float property_label_width = 156.0f;
    float crop_edge_hit_half_width = 8.0f;
    float crop_corner_hit_size = 20.0f;
    float crop_handle_radius = 6.0f;
    UiDensity density = UiDensity::Balanced;
    float content_scale = 1.0f;
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
    mmltk::rfdetr::CompilationMode compile_mode = mmltk::rfdetr::CompilationMode::kSelective;
};

struct TrainExecutionPaneState {
    TrainExecutionTarget execution_target = TrainExecutionTarget::Local;
    std::vector<int> local_device_ids{0};
    std::array<bool, 5> remote_family_enabled{{true, true, true, true, true}};
    std::string remote_container_image;
    std::string remote_launch_template;
};

struct TrainPaneState : TrainExecutionPaneState {
    std::string output_dir = "./gui-train-output";
    std::string resume_path;
    TrainInputMode input_mode = TrainInputMode::Weights;
};

struct TrainWorkflowExecutionState {
    ExecutionTuningState tuning;
    TrainPaneState pane;
};

using TrainViewRuntimeConfig = mmltk::rfdetr::TrainRuntimeSharedConfig<std::string, int, TrainOptimizerMode>;

struct TrainViewState : mmltk::rfdetr::TrainOptimizerTuningConfig, TrainViewRuntimeConfig, TrainExecutionPaneState {
    TrainViewState() {
        batch_size = 2;
        epochs = 12;
        workers = 8;
        progress_bar = false;
    }

    std::string output_dir = "./gui-train-output";
    std::string distributed_store_path;
    double ema_decay = 0.993;
    int ema_tau = 100;
    TrainInputMode input_mode = TrainInputMode::Weights;
    mmltk::rfdetr::CompilationMode compile_mode = mmltk::rfdetr::CompilationMode::kSelective;
    int distributed_rank = 0;
    int distributed_world_size = 1;
    bool fused_optimizer = true;
    bool distributed_worker = false;
    mmltk::rfdetr::TrainRecipeFieldOverrides recipe_overrides;
};

using ValidateViewDatasetConfig = mmltk::rfdetr::ValidationDatasetConfig<std::string, int, int>;

struct ValidateViewState : mmltk::rfdetr::ValidationSharedConfig,
                           ValidateViewDatasetConfig,
                           mmltk::rfdetr::ValidationCompileConfig {
    ValidateViewState() {
        log_mode = mmltk::rfdetr::ValidationLogMode::Quiet;
    }

    std::string report_json_path = "./rfdetr-validation-report.json";
    std::string split = "val";
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
    mmltk::rfdetr::CompilationMode compile_mode = mmltk::rfdetr::CompilationMode::kSelective;
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
    mmltk::rfdetr::CompilationMode compile_mode = mmltk::rfdetr::CompilationMode::kSelective;
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
        std::string{}, std::string{}, s.train_compiled_path, s.val_compiled_path, s.test_compiled_path,
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
        s.cpu_affinity, 0, s.workers, s.lanes, true, s.progress_bar, s.compile_mode,
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
    TrainPaneState pane_state;
    static_cast<TrainExecutionPaneState&>(pane_state) = static_cast<const TrainExecutionPaneState&>(s);
    pane_state.output_dir = s.output_dir;
    pane_state.resume_path = s.resume_path;
    pane_state.input_mode = s.input_mode;
    return pane_state;
}

inline void apply_train_pane_state(TrainViewState& s, const TrainPaneState& pane_state) {
    s.output_dir = pane_state.output_dir;
    s.resume_path = pane_state.resume_path;
    s.input_mode = pane_state.input_mode;
    static_cast<TrainExecutionPaneState&>(s) = static_cast<const TrainExecutionPaneState&>(pane_state);
}

inline TrainWorkflowExecutionState train_workflow_execution_state(const TrainViewState& s) {
    return TrainWorkflowExecutionState{
        execution_tuning(s),
        train_pane_state(s),
    };
}

inline void apply_train_workflow_execution_state(TrainViewState& s,
                                                 const TrainWorkflowExecutionState& execution_state) {
    apply_execution_tuning(s, execution_state.tuning);
    apply_train_pane_state(s, execution_state.pane);
}

inline mmltk::rfdetr::TrainOptions train_options(const TrainViewState& s, const std::vector<int>& device_ids = {}) {
    mmltk::rfdetr::TrainOptions options;
    const DatasetPathState dataset_state = dataset_paths(s);
    const ModelArtifactSelectionState artifact_state = model_artifacts(s);
    const TrainWorkflowExecutionState execution_state = train_workflow_execution_state(s);

    options.train_compiled_path = dataset_state.train_compiled_path;
    options.val_compiled_path = dataset_state.val_compiled_path;
    options.test_compiled_path = dataset_state.test_compiled_path;
    options.output_dir = execution_state.pane.output_dir;
    options.weights_path =
        execution_state.pane.input_mode == TrainInputMode::Weights ? artifact_state.weights_path : std::string{};
    options.resume_path =
        execution_state.pane.input_mode == TrainInputMode::Resume ? execution_state.pane.resume_path : std::string{};
    options.batch_size = static_cast<std::size_t>(s.batch_size);
    options.val_batch_size = static_cast<std::size_t>(s.val_batch_size);
    options.eval_max_dets = static_cast<std::size_t>(s.eval_max_dets);
    options.cpu_affinity = execution_state.tuning.cpu_affinity;
    options.lr_scheduler = s.lr_scheduler;
    options.epochs = s.epochs;
    options.grad_accum_steps = s.grad_accum_steps;
    options.lr_drop = s.lr_drop;
    options.ema_tau = s.ema_tau;
    options.print_freq = s.print_freq;
    options.prefetch_factor = s.prefetch_factor;
    options.seed = s.seed;
    options.device_id = device_ids.size() == 1U ? device_ids.front() : -1;
    options.workers = execution_state.tuning.workers;
    options.lanes = execution_state.tuning.lanes;
    options.distributed_rank = s.distributed_rank;
    options.distributed_world_size = s.distributed_world_size;
    options.use_ema = s.use_ema;
    options.amp = s.amp;
    options.progress_bar = execution_state.tuning.progress_bar;
    options.fused_optimizer = s.fused_optimizer;
    options.distributed_worker = s.distributed_worker;
    options.freeze_encoder = s.freeze_encoder;
    options.optimizer = train_optimizer_kind(s.optimizer);
    options.compilation_mode = execution_state.tuning.compile_mode;
    options.lr = s.lr;
    options.lr_encoder = s.lr_encoder;
    options.lr_component_decay = s.lr_component_decay;
    options.encoder_layer_decay = s.encoder_layer_decay;
    options.momentum = s.momentum;
    options.weight_decay = s.weight_decay;
    options.warmup_epochs = s.warmup_epochs;
    options.warmup_momentum = s.warmup_momentum;
    options.lr_min_factor = s.lr_min_factor;
    options.clip_max_norm = s.clip_max_norm;
    options.ema_decay = s.ema_decay;
    options.distributed_store_path = s.distributed_store_path;
    options.device_ids = device_ids;
    return options;
}

inline void apply_train_options(TrainViewState& s, const mmltk::rfdetr::TrainOptions& options) {
    DatasetPathState dataset_state = dataset_paths(s);
    ModelArtifactSelectionState artifact_state = model_artifacts(s);
    TrainWorkflowExecutionState execution_state = train_workflow_execution_state(s);

    dataset_state.train_compiled_path = options.train_compiled_path.string();
    dataset_state.val_compiled_path = options.val_compiled_path.string();
    dataset_state.test_compiled_path = options.test_compiled_path.string();
    execution_state.pane.output_dir = options.output_dir.string();
    artifact_state.weights_path = options.weights_path.string();
    execution_state.pane.resume_path = options.resume_path.string();
    execution_state.pane.input_mode = options.resume_path.empty() ? TrainInputMode::Weights : TrainInputMode::Resume;
    execution_state.pane.execution_target = TrainExecutionTarget::Local;
    execution_state.pane.local_device_ids =
        !options.device_ids.empty()
            ? options.device_ids
            : (options.device_id >= 0 ? std::vector<int>{options.device_id} : std::vector<int>{});
    execution_state.tuning.cpu_affinity = options.cpu_affinity;
    execution_state.tuning.workers = options.workers;
    execution_state.tuning.lanes = options.lanes;
    execution_state.tuning.progress_bar = options.progress_bar;
    execution_state.tuning.compile_mode = options.compilation_mode;
    s.batch_size = static_cast<int>(options.batch_size);
    s.val_batch_size = static_cast<int>(options.val_batch_size);
    s.epochs = options.epochs;
    s.grad_accum_steps = options.grad_accum_steps;
    s.eval_max_dets = static_cast<int>(options.eval_max_dets);
    s.lr_drop = options.lr_drop;
    s.print_freq = options.print_freq;
    s.prefetch_factor = options.prefetch_factor;
    s.seed = options.seed;
    s.lr = options.lr;
    s.lr_encoder = options.lr_encoder;
    s.lr_component_decay = options.lr_component_decay;
    s.encoder_layer_decay = options.encoder_layer_decay;
    s.momentum = options.momentum;
    s.weight_decay = options.weight_decay;
    s.warmup_epochs = options.warmup_epochs;
    s.warmup_momentum = options.warmup_momentum;
    s.lr_min_factor = options.lr_min_factor;
    s.clip_max_norm = options.clip_max_norm;
    s.ema_decay = options.ema_decay;
    s.ema_tau = options.ema_tau;
    s.lr_scheduler = options.lr_scheduler;
    s.use_ema = options.use_ema;
    s.amp = options.amp;
    s.freeze_encoder = options.freeze_encoder;
    s.fused_optimizer = options.fused_optimizer;
    s.distributed_store_path = options.distributed_store_path.string();
    s.distributed_rank = options.distributed_rank;
    s.distributed_world_size = options.distributed_world_size;
    s.distributed_worker = options.distributed_worker;
    s.optimizer = train_optimizer_mode(options.optimizer);

    apply_dataset_paths(s, dataset_state);
    apply_model_artifacts(s, artifact_state);
    apply_train_workflow_execution_state(s, execution_state);
}

inline DatasetPathState dataset_paths(const ValidateViewState& s) {
    return DatasetPathState{
        s.compiled_path, s.source_dir, std::string{}, std::string{}, std::string{},
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
        s.cpu_affinity, s.device_id, s.workers, 0, s.allow_fp16, false, mmltk::rfdetr::CompilationMode::kSelective,
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
        s.cpu_affinity, s.device_id, s.workers, s.lanes, s.allow_fp16, s.progress_bar, s.compile_mode,
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
        std::string{}, s.device_id, 0, 0, s.allow_fp16, false, s.compile_mode,
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
        std::string{}, s.device_id, 0, 0, s.allow_fp16, false, mmltk::rfdetr::CompilationMode::kSelective,
    };
}

inline void apply_execution_tuning(ExportViewState& s, const ExecutionTuningState& execution_state) {
    s.device_id = execution_state.device_id;
    s.allow_fp16 = execution_state.allow_fp16;
}

}  // namespace mmltk::gui
