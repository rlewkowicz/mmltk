#pragma once

#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/train_recipe.h"
#include "mmltk/rfdetr/validate.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

struct BuildEngineRequest {
    std::filesystem::path onnx_path;
    std::filesystem::path output_path;
    int device_id = 0;
    bool allow_fp16 = true;
};

struct ExportOnnxRequest {
    std::filesystem::path weights_path;
    std::filesystem::path output_path;
    int device_id = 0;
    int opset_version = 19;
    bool simplify = false;
};

struct PredictRequest : ModelArtifactRequest {
    PredictSourceKind source_kind = PredictSourceKind::CompiledDataset;
    std::filesystem::path compiled_path;
    std::vector<PredictImageInput> image_inputs;
    std::filesystem::path output_path;
    std::string backend = "auto";
    std::size_t batch_size = 1;
    std::size_t max_dets_per_image = 500;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    float threshold = 0.0f;
    std::string cpu_affinity;
    bool allow_fp16 = true;
    bool progress_bar = true;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

struct ValidateRequest {
    std::filesystem::path compiled_path;
    std::filesystem::path source_dir;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    std::filesystem::path save_engine_path;
    std::filesystem::path report_json_path;
    std::string split;
    std::string eval_order = "onnx,tensorrt";
    std::uint32_t resolution = 432;
    std::size_t limit_images = 0;
    std::size_t alignment_images = 16;
    std::size_t eval_max_dets = 500;
    std::size_t batch_size = 1;
    std::size_t prefetch_factor = 2;
    int device_id = 0;
    int workers = 0;
    int compile_workers = -1;
    int compile_cuda_mask_batch_size = 0;
    int compile_cuda_device_id = 0;
    std::string cpu_affinity;
    bool recompile = false;
    bool profile = false;
    bool allow_fp16 = true;
    bool write_report_json = true;
    ValidationLogMode log_mode = ValidationLogMode::Interactive;
};

struct TrainRequest {
    std::filesystem::path train_compiled_path;
    std::filesystem::path val_compiled_path;
    std::filesystem::path test_compiled_path;
    std::filesystem::path output_dir;
    std::filesystem::path weights_path;
    std::filesystem::path resume_path;
    std::filesystem::path distributed_store_path;
    std::vector<int> device_ids;
    std::size_t batch_size = 1;
    std::size_t val_batch_size = 0;
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
    double ema_decay = 0.993;
    std::size_t eval_max_dets = 500;
    std::string cpu_affinity;
    std::string lr_scheduler = "step";
    int epochs = 1;
    int grad_accum_steps = 1;
    int lr_drop = 100;
    int ema_tau = 100;
    int print_freq = 100;
    int prefetch_factor = 2;
    int seed = 42;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    int distributed_rank = 0;
    int distributed_world_size = 1;
    bool use_ema = false;
    bool amp = true;
    bool progress_bar = true;
    bool fused_optimizer = true;
    bool distributed_worker = false;
    bool freeze_encoder = false;
    TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
    CompilationMode compilation_mode = CompilationMode::kSelective;
    TrainRecipeFieldOverrides recipe_overrides;
};

template <typename Request>
inline std::size_t selected_model_input_count(const Request& request) {
    return static_cast<std::size_t>(!request.weights_path.empty()) +
           static_cast<std::size_t>(!request.onnx_path.empty()) +
           static_cast<std::size_t>(!request.tensorrt_path.empty());
}

inline const char* train_optimizer_cli_value(const TrainOptimizerKind kind) {
    switch (kind) {
    case TrainOptimizerKind::AdamW:
        return "adamw";
    case TrainOptimizerKind::Muon:
        return "muon";
    }
    return "adamw";
}

inline const char* compilation_mode_cli_value(const CompilationMode mode) {
    switch (mode) {
    case CompilationMode::kNone:
        return "none";
    case CompilationMode::kSelective:
        return "selective";
    case CompilationMode::kFullTrace:
        return "full";
    }
    return "selective";
}

void validate_build_engine_request(const BuildEngineRequest& request);

void validate_export_onnx_request(const ExportOnnxRequest& request);

void validate_predict_request(const PredictRequest& request);

void finalize_predict_request(PredictRequest& request);

PredictOptions to_predict_options(const PredictRequest& request,
                                  std::string_view preset_name = {});

void validate_validate_request(const ValidateRequest& request);

void finalize_validate_request(ValidateRequest& request);

ValidationOptions to_validate_options(const ValidateRequest& request);

void validate_train_request(const TrainRequest& request);

TrainOptions to_train_options(const TrainRequest& request);

TrainRequest train_request_from_options(const TrainOptions& options,
                                       const TrainRecipeFieldOverrides& recipe_overrides = {});

std::string infer_train_recipe_preset_name(const TrainRequest& request);

void finalize_train_request(TrainRequest& request,
                            std::string_view fallback_preset_name = {});

} // namespace mmltk::rfdetr
