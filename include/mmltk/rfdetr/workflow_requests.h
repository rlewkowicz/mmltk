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

inline void validate_build_engine_request(const BuildEngineRequest& request) {
    if (request.onnx_path.empty() || request.output_path.empty()) {
        throw std::runtime_error("rfdetr build-engine requires --onnx and --output");
    }
}

inline void validate_export_onnx_request(const ExportOnnxRequest& request) {
    if (request.weights_path.empty() || request.output_path.empty()) {
        throw std::runtime_error("rfdetr export-onnx requires --weights and --output");
    }
}

inline void validate_predict_request(const PredictRequest& request) {
    if (request.source_kind == PredictSourceKind::CompiledDataset) {
        if (request.compiled_path.empty() || request.output_path.empty()) {
            throw std::runtime_error("rfdetr predict requires --compiled and --output");
        }
    } else {
        if (request.image_inputs.empty()) {
            throw std::runtime_error("rfdetr predict requires at least one image input");
        }
        if (request.output_path.empty()) {
            throw std::runtime_error("rfdetr predict requires --output");
        }
    }
    if (selected_model_input_count(request) != 1U) {
        throw std::runtime_error("rfdetr predict requires exactly one of --weights, --onnx, or --tensorrt");
    }
}

inline void finalize_predict_request(PredictRequest& request) {
    validate_predict_request(request);
}

inline PredictOptions to_predict_options(const PredictRequest& request,
                                         std::string_view preset_name = {}) {
    PredictOptions options;
    options.preset_name = std::string(preset_name);
    options.weights_path = request.weights_path;
    options.onnx_path = request.onnx_path;
    options.tensorrt_path = request.tensorrt_path;
    options.source_kind = request.source_kind;
    options.compiled_path = request.compiled_path;
    options.image_inputs = request.image_inputs;
    options.output_path = request.output_path;
    options.backend = request.backend;
    options.batch_size = request.batch_size;
    options.max_dets_per_image = request.max_dets_per_image;
    options.device_id = request.device_id;
    options.workers = request.workers;
    options.lanes = request.lanes;
    options.threshold = request.threshold;
    options.cpu_affinity = request.cpu_affinity;
    options.allow_fp16 = request.allow_fp16;
    options.progress_bar = request.progress_bar;
    options.compilation_mode = request.compilation_mode;
    return options;
}

inline void validate_validate_request(const ValidateRequest& request) {
    if (request.compiled_path.empty()) {
        throw std::runtime_error("rfdetr validate requires --compiled");
    }
    if (request.onnx_path.empty() && request.tensorrt_path.empty()) {
        throw std::runtime_error("rfdetr validate requires at least one of --onnx or --tensorrt");
    }
    if (request.recompile && request.source_dir.empty()) {
        throw std::runtime_error("rfdetr validate --recompile requires --source");
    }
    if (request.compile_cuda_mask_batch_size < 0) {
        throw std::runtime_error("rfdetr validate --compile-cuda-mask-batch-size must be non-negative");
    }
}

inline void finalize_validate_request(ValidateRequest& request) {
    if (request.split.empty()) {
        request.split = request.compiled_path.stem().string();
    }
    if (request.report_json_path.empty()) {
        request.report_json_path = request.compiled_path.parent_path() / "rfdetr-validation-report.json";
    }
    request.log_mode = ValidationLogMode::Interactive;
    request.write_report_json = true;
    validate_validate_request(request);
}

inline ValidationOptions to_validate_options(const ValidateRequest& request) {
    ValidationOptions options;
    options.compiled_path = request.compiled_path;
    options.source_dir = request.source_dir;
    options.onnx_path = request.onnx_path;
    options.tensorrt_path = request.tensorrt_path;
    options.save_engine_path = request.save_engine_path;
    options.report_json_path = request.report_json_path;
    options.split = request.split;
    options.eval_order = request.eval_order;
    options.resolution = request.resolution;
    options.limit_images = request.limit_images;
    options.alignment_images = request.alignment_images;
    options.eval_max_dets = request.eval_max_dets;
    options.batch_size = request.batch_size;
    options.prefetch_factor = request.prefetch_factor;
    options.device_id = request.device_id;
    options.workers = request.workers;
    options.compile_workers = request.compile_workers;
    options.compile_cuda_mask_batch_size = request.compile_cuda_mask_batch_size;
    options.compile_cuda_device_id = request.compile_cuda_device_id;
    options.cpu_affinity = request.cpu_affinity;
    options.recompile = request.recompile;
    options.profile = request.profile;
    options.allow_fp16 = request.allow_fp16;
    options.write_report_json = request.write_report_json;
    options.log_mode = request.log_mode;
    return options;
}

inline void validate_train_request(const TrainRequest& request) {
    if (request.train_compiled_path.empty() ||
        request.val_compiled_path.empty() ||
        request.output_dir.empty()) {
        throw std::runtime_error("rfdetr train requires --train-compiled, --val-compiled, and --output-dir");
    }
    const std::size_t selected_input_count =
        static_cast<std::size_t>(!request.weights_path.empty()) +
        static_cast<std::size_t>(!request.resume_path.empty());
    if (selected_input_count != 1U) {
        throw std::runtime_error("rfdetr train requires exactly one of --weights or --resume");
    }
    if (request.distributed_worker) {
        if (request.distributed_rank < 0 ||
            request.distributed_world_size <= 1 ||
            request.distributed_store_path.empty()) {
            throw std::runtime_error("invalid internal RF-DETR distributed worker arguments");
        }
    } else if (request.distributed_world_size != 1 ||
               request.distributed_rank != 0 ||
               !request.distributed_store_path.empty()) {
        throw std::runtime_error("internal RF-DETR distributed worker options require --dist-worker");
    }
}

inline TrainOptions to_train_options(const TrainRequest& request) {
    TrainOptions options;
    options.batch_size = request.batch_size;
    options.val_batch_size = request.val_batch_size;
    options.lr = request.lr;
    options.lr_encoder = request.lr_encoder;
    options.lr_component_decay = request.lr_component_decay;
    options.encoder_layer_decay = request.encoder_layer_decay;
    options.momentum = request.momentum;
    options.weight_decay = request.weight_decay;
    options.warmup_epochs = request.warmup_epochs;
    options.warmup_momentum = request.warmup_momentum;
    options.lr_min_factor = request.lr_min_factor;
    options.clip_max_norm = request.clip_max_norm;
    options.ema_decay = request.ema_decay;
    options.eval_max_dets = request.eval_max_dets;
    options.cpu_affinity = request.cpu_affinity;
    options.lr_scheduler = request.lr_scheduler;
    options.train_compiled_path = request.train_compiled_path;
    options.val_compiled_path = request.val_compiled_path;
    options.test_compiled_path = request.test_compiled_path;
    options.output_dir = request.output_dir;
    options.weights_path = request.weights_path;
    options.resume_path = request.resume_path;
    options.distributed_store_path = request.distributed_store_path;
    options.device_ids = request.device_ids;
    options.epochs = request.epochs;
    options.grad_accum_steps = request.grad_accum_steps;
    options.lr_drop = request.lr_drop;
    options.ema_tau = request.ema_tau;
    options.print_freq = request.print_freq;
    options.prefetch_factor = request.prefetch_factor;
    options.seed = request.seed;
    options.device_id = request.device_id;
    options.workers = request.workers;
    options.lanes = request.lanes;
    options.distributed_rank = request.distributed_rank;
    options.distributed_world_size = request.distributed_world_size;
    options.use_ema = request.use_ema;
    options.amp = request.amp;
    options.progress_bar = request.progress_bar;
    options.fused_optimizer = request.fused_optimizer;
    options.distributed_worker = request.distributed_worker;
    options.freeze_encoder = request.freeze_encoder;
    options.optimizer = request.optimizer;
    options.compilation_mode = request.compilation_mode;
    return options;
}

inline TrainRequest train_request_from_options(const TrainOptions& options,
                                               const TrainRecipeFieldOverrides& recipe_overrides = {}) {
    TrainRequest request;
    request.train_compiled_path = options.train_compiled_path;
    request.val_compiled_path = options.val_compiled_path;
    request.test_compiled_path = options.test_compiled_path;
    request.output_dir = options.output_dir;
    request.weights_path = options.weights_path;
    request.resume_path = options.resume_path;
    request.distributed_store_path = options.distributed_store_path;
    request.device_ids = options.device_ids;
    request.batch_size = options.batch_size;
    request.val_batch_size = options.val_batch_size;
    request.lr = options.lr;
    request.lr_encoder = options.lr_encoder;
    request.lr_component_decay = options.lr_component_decay;
    request.encoder_layer_decay = options.encoder_layer_decay;
    request.momentum = options.momentum;
    request.weight_decay = options.weight_decay;
    request.warmup_epochs = options.warmup_epochs;
    request.warmup_momentum = options.warmup_momentum;
    request.lr_min_factor = options.lr_min_factor;
    request.clip_max_norm = options.clip_max_norm;
    request.ema_decay = options.ema_decay;
    request.eval_max_dets = options.eval_max_dets;
    request.cpu_affinity = options.cpu_affinity;
    request.lr_scheduler = options.lr_scheduler;
    request.epochs = options.epochs;
    request.grad_accum_steps = options.grad_accum_steps;
    request.lr_drop = options.lr_drop;
    request.ema_tau = options.ema_tau;
    request.print_freq = options.print_freq;
    request.prefetch_factor = options.prefetch_factor;
    request.seed = options.seed;
    request.device_id = options.device_id;
    request.workers = options.workers;
    request.lanes = options.lanes;
    request.distributed_rank = options.distributed_rank;
    request.distributed_world_size = options.distributed_world_size;
    request.use_ema = options.use_ema;
    request.amp = options.amp;
    request.progress_bar = options.progress_bar;
    request.fused_optimizer = options.fused_optimizer;
    request.distributed_worker = options.distributed_worker;
    request.freeze_encoder = options.freeze_encoder;
    request.optimizer = options.optimizer;
    request.compilation_mode = options.compilation_mode;
    request.recipe_overrides = recipe_overrides;
    return request;
}

inline std::string infer_train_recipe_preset_name(const TrainRequest& request) {
    const auto source_checkpoint = !request.resume_path.empty() ? request.resume_path : request.weights_path;
    return infer_train_recipe_preset_name_from_path(source_checkpoint);
}

inline void finalize_train_request(TrainRequest& request,
                                   std::string_view fallback_preset_name = {}) {
    if (request.device_ids.size() == 1U) {
        request.device_id = request.device_ids.front();
        request.device_ids.clear();
    }

    std::string recipe_preset_name = infer_train_recipe_preset_name(request);
    if (recipe_preset_name.empty()) {
        recipe_preset_name = std::string(fallback_preset_name);
    }
    if (recipe_preset_name.empty()) {
        throw std::runtime_error(
            "unable to infer an RF-DETR train recipe preset from the checkpoint path");
    }

    TrainOptions options = to_train_options(request);
    apply_train_recipe(
        options,
        resolve_train_recipe(recipe_preset_name, options.optimizer),
        request.recipe_overrides);
    if (options.lr_scheduler != "step" && options.lr_scheduler != "cosine") {
        throw std::runtime_error("rfdetr train --lr-scheduler must be 'step' or 'cosine'");
    }
    request = train_request_from_options(options, request.recipe_overrides);
    validate_train_request(request);
}

} // namespace mmltk::rfdetr
