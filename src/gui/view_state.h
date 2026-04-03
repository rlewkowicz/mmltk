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

} // namespace mmltk::gui
