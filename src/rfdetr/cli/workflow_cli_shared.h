#pragma once

#include "CLI11.hpp"

#include "rfdetr/backends_internal.h"
#include "mmltk/rfdetr/artifacts.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/train_types.h"
#include "mmltk/rfdetr/workflow_requests.h"
#include "string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::rfdetr::cli_shared {

struct PredictCommandSpec {
    PredictRequest request;
    std::string compile_mode = "selective";
};

struct ValidateCommandSpec {
    ValidateRequest request;
    CLI::Option* compile_workers = nullptr;
    CLI::Option* compile_cuda_mask_batch_size = nullptr;
    CLI::Option* compile_cuda_device_id = nullptr;
};

struct TrainCommandSpec {
    TrainRequest request;
    std::string device_ids_text;
    std::string compile_mode = "selective";
    std::string optimizer = "adamw";
    CLI::Option* lr = nullptr;
    CLI::Option* lr_encoder = nullptr;
    CLI::Option* lr_component_decay = nullptr;
    CLI::Option* encoder_layer_decay = nullptr;
    CLI::Option* momentum = nullptr;
    CLI::Option* weight_decay = nullptr;
    CLI::Option* lr_drop = nullptr;
    CLI::Option* lr_scheduler = nullptr;
    CLI::Option* lr_min_factor = nullptr;
    CLI::Option* warmup_epochs = nullptr;
    CLI::Option* warmup_momentum = nullptr;
    CLI::Option* ema_decay = nullptr;
    CLI::Option* ema_tau = nullptr;
    CLI::Option* fused_optimizer = nullptr;
    CLI::Option* dist_worker = nullptr;
    CLI::Option* dist_rank = nullptr;
    CLI::Option* dist_world_size = nullptr;
    CLI::Option* dist_store_file = nullptr;
    CLI::Option* device_id = nullptr;
    CLI::Option* device_ids = nullptr;
};

enum class ValidateUnsupportedOptionMode : std::uint8_t {
    AllowAll = 0,
    RejectGuiUnsupported = 1,
};

enum class TrainUnsupportedOptionMode : std::uint8_t {
    AllowAll = 0,
    RejectGuiUnsupported = 1,
};

inline CLI::Option* add_path_option(CLI::App* command, const std::string& name, std::filesystem::path& value,
                                    const char* description) {
    return command->add_option(name, value, description)->type_name("PATH");
}

inline void add_compile_mode_option(CLI::App* command, std::string& compile_mode) {
    command->add_option("--compile-mode", compile_mode, "Native PyTorch compilation mode: none, selective, or full")
        ->type_name("MODE");
}

inline void add_model_input_options(CLI::App* command, ModelArtifactRequest& request,
                                    const char* tensorrt_description) {
    add_path_option(command, "--weights", request.weights_path, "RF-DETR checkpoint path");
    add_path_option(command, "--onnx", request.onnx_path, "ONNX model path");
    add_path_option(command, "--tensorrt", request.tensorrt_path, tensorrt_description);
    command->add_option("--preset", request.preset_name, "Declared RF-DETR preset architecture");
    command->add_option("--resolution", request.resolution, "Square model input resolution; 0 uses model metadata")
        ->type_name("INT");
}

inline void add_build_engine_request_options(CLI::App* io_options, CLI::App* execution_options,
                                             BuildEngineRequest& request) {
    add_path_option(io_options, "--onnx", request.onnx_path, "ONNX model path");
    add_path_option(io_options, "--output", request.output_path, "Output TensorRT engine path");
    io_options->add_option("--preset", request.preset_name, "Declared RF-DETR preset architecture");
    io_options
        ->add_option("--resolution", request.resolution,
                     "Square model input resolution; 0 uses backend/catalog metadata")
        ->type_name("INT");
    execution_options->add_option("--device-id", request.device_id, "CUDA device id")->type_name("INT");
    execution_options->add_flag("--fp16,!--no-fp16", request.allow_fp16, "Enable FP16 TensorRT kernels");
}

inline void add_export_onnx_request_options(CLI::App* io_options, CLI::App* execution_options,
                                            ExportOnnxRequest& request) {
    add_path_option(io_options, "--weights", request.weights_path, "RF-DETR checkpoint path");
    add_path_option(io_options, "--output", request.output_path, "Output ONNX model path");
    io_options->add_option("--preset", request.preset_name, "Declared RF-DETR preset architecture");
    io_options
        ->add_option("--resolution", request.resolution,
                     "Square model input resolution; 0 uses checkpoint/catalog metadata")
        ->type_name("INT");
    execution_options->add_option("--device-id", request.device_id, "CUDA device id")->type_name("INT");
    execution_options
        ->add_option("--opset-version", request.opset_version,
                     "ONNX opset version (native exporter currently supports 19 only)")
        ->type_name("INT");
    execution_options->add_flag("--simplify", request.simplify,
                                "Run ONNX checker and shape inference on the exported model");
}

inline void add_predict_command_options(CLI::App* command, PredictCommandSpec& spec) {
    auto* dataset = command->add_option_group("Dataset");
    add_path_option(dataset, "--compiled", spec.request.compiled_path, "Compiled dataset split (.bin)");
    add_path_option(dataset, "--output", spec.request.output_path, "Prediction JSON output path");
    auto* model = command->add_option_group("Model input");
    add_model_input_options(model, spec.request, "TensorRT engine path or ONNX path to build from");
    auto* execution = command->add_option_group("Execution");
    execution->add_option("--batch-size", spec.request.batch_size, "Batch size for inference")->type_name("INT");
    execution->add_option("--device-id", spec.request.device_id, "CUDA device id")->type_name("INT");
    execution->add_option("--threshold", spec.request.threshold, "Minimum score threshold for saved detections")
        ->type_name("FLOAT");
    execution->add_option("--workers", spec.request.workers, "Dataset worker count")->type_name("INT");
    execution->add_option("--lanes", spec.request.lanes, "Parallel backend lane count")->type_name("INT");
    execution->add_option("--cpu-affinity", spec.request.cpu_affinity, "Linux CPU list or range string for workers");
    execution->add_option("--backend", spec.request.backend,
                          "Backend preference for ONNX/TensorRT artifacts: auto, onnx, or tensorrt");
    execution->add_flag("--fp16,!--no-fp16", spec.request.allow_fp16,
                        "Enable BF16 native inference or supported FP16 fallback/backend execution");
    execution->add_flag("--progress,!--no-progress", spec.request.progress_bar, "Render interactive progress output");
    add_compile_mode_option(execution, spec.compile_mode);
}

inline void add_validate_command_options(CLI::App* command, ValidateCommandSpec& spec,
                                         const bool hide_compile_tuning = false) {
    auto* dataset = command->add_option_group("Dataset");
    add_path_option(dataset, "--compiled", spec.request.compiled_path, "Compiled dataset split (.bin)");
    add_path_option(dataset, "--source", spec.request.source_dir, "Source dataset root used for optional recompiles");
    dataset->add_option("--split", spec.request.split, "Source dataset split name used when recompiling");
    dataset->add_option("--resolution", spec.request.resolution, "Square resolution for recompiles")->type_name("INT");
    dataset->add_flag("--recompile", spec.request.recompile, "Recompile the source dataset before validation");
    spec.compile_workers =
        dataset
            ->add_option("--compile-workers", spec.request.compile_workers,
                         "Total CPU worker budget for recompiles; 0 or negative selects all available CPUs")
            ->type_name("INT");
    spec.compile_cuda_mask_batch_size =
        dataset
            ->add_option("--compile-cuda-mask-batch-size", spec.request.compile_cuda_mask_batch_size,
                         "Batched CUDA mask-resize task count for recompiles; 0 disables GPU mask resizing")
            ->type_name("INT");
    spec.compile_cuda_device_id = dataset
                                      ->add_option("--compile-cuda-device-id", spec.request.compile_cuda_device_id,
                                                   "CUDA device id used for batched mask resizing during recompiles")
                                      ->type_name("INT");
    if (hide_compile_tuning) {
        spec.compile_workers->group("");
        spec.compile_cuda_mask_batch_size->group("");
        spec.compile_cuda_device_id->group("");
    }

    auto* model = command->add_option_group("Model input");
    add_path_option(model, "--weights", spec.request.weights_path, "RF-DETR checkpoint path");
    model->add_option("--preset", spec.request.preset_name,
                      "Declared RF-DETR preset architecture for checkpoint/backend inputs");
    add_path_option(model, "--onnx", spec.request.onnx_path, "ONNX model path");
    add_path_option(model, "--tensorrt", spec.request.tensorrt_path, "TensorRT engine path or ONNX path to build from");
    add_path_option(model, "--save-engine", spec.request.save_engine_path,
                    "Optional path to save a built TensorRT engine");
    auto* reports = command->add_option_group("Reports");
    add_path_option(reports, "--report-json", spec.request.report_json_path, "Validation report JSON path");
    reports->add_option("--eval-order", spec.request.eval_order, "Comma-separated backend evaluation order");
    auto* execution = command->add_option_group("Execution");
    execution->add_option("--limit-images", spec.request.limit_images, "Limit the number of validated images")
        ->type_name("INT");
    execution
        ->add_option("--alignment-images", spec.request.alignment_images,
                     "Number of images used for backend alignment probes")
        ->type_name("INT");
    execution
        ->add_option("--eval-max-dets", spec.request.eval_max_dets, "Maximum detections per image during evaluation")
        ->type_name("INT");
    execution->add_option("--device-id", spec.request.device_id, "CUDA device id")->type_name("INT");
    execution->add_option("--workers", spec.request.workers, "Dataset worker count")->type_name("INT");
    execution->add_option("--batch-size", spec.request.batch_size, "Backend batch size")->type_name("INT");
    execution->add_option("--cpu-affinity", spec.request.cpu_affinity, "Linux CPU list or range string for workers");
    execution->add_flag("--profile", spec.request.profile, "Collect detailed timing metrics during validation");
    execution->add_flag("--fp16,!--no-fp16", spec.request.allow_fp16,
                        "Enable BF16 weights validation or supported FP16 fallback/TensorRT execution");
}

inline void add_train_command_options(CLI::App* command, TrainCommandSpec& spec,
                                      const bool hide_gui_unsupported_options = false) {
    auto* dataset = command->add_option_group("Dataset");
    add_path_option(dataset, "--train-compiled", spec.request.train_compiled_path, "Compiled training split (.bin)");
    add_path_option(dataset, "--val-compiled", spec.request.val_compiled_path, "Compiled validation split (.bin)");
    add_path_option(dataset, "--test-compiled", spec.request.test_compiled_path, "Optional compiled test split (.bin)");
    dataset
        ->add_option("--resolution", spec.request.resolution,
                     "Square model input resolution; 0 uses checkpoint/catalog metadata")
        ->type_name("INT");
    auto* checkpoint = command->add_option_group("Checkpoint");
    add_path_option(checkpoint, "--output-dir", spec.request.output_dir, "Output directory for checkpoints and logs");
    add_path_option(checkpoint, "--weights", spec.request.weights_path, "Source checkpoint used for initialization");
    add_path_option(checkpoint, "--resume", spec.request.resume_path, "Existing native checkpoint to resume");
    checkpoint->add_option("--preset", spec.request.preset_name, "Declared RF-DETR preset architecture");
    auto* optimization = command->add_option_group("Optimization");
    optimization->add_option("--batch-size", spec.request.batch_size, "Per-rank training batch size")->type_name("INT");
    optimization
        ->add_option("--val-batch-size", spec.request.val_batch_size, "Validation batch size, 0 reuses --batch-size")
        ->type_name("INT");
    optimization->add_option("--epochs", spec.request.epochs, "Number of training epochs")->type_name("INT");
    optimization->add_option("--grad-accum-steps", spec.request.grad_accum_steps, "Gradient accumulation steps")
        ->type_name("INT");
    optimization->add_option("--optimizer", spec.optimizer, "Training optimizer: adamw or muon");
    spec.lr = optimization->add_option("--lr", spec.request.lr, "Decoder/base learning rate");
    spec.lr->type_name("FLOAT");
    spec.lr_encoder = optimization->add_option("--lr-encoder", spec.request.lr_encoder, "Encoder learning rate");
    spec.lr_encoder->type_name("FLOAT");
    spec.momentum = optimization->add_option("--momentum", spec.request.momentum, "Muon momentum");
    spec.momentum->type_name("FLOAT");
    optimization->add_flag("--freeze-encoder,!--no-freeze-encoder", spec.request.freeze_encoder,
                           "Freeze encoder/backbone parameters");
    spec.lr_component_decay = optimization->add_option("--lr-component-decay", spec.request.lr_component_decay,
                                                       "Component-wise learning rate decay");
    spec.lr_component_decay->type_name("FLOAT");
    spec.encoder_layer_decay = optimization->add_option("--encoder-layer-decay", spec.request.encoder_layer_decay,
                                                        "Per-layer encoder learning rate decay");
    spec.encoder_layer_decay->type_name("FLOAT");
    spec.weight_decay = optimization->add_option("--weight-decay", spec.request.weight_decay, "Optimizer weight decay");
    spec.weight_decay->type_name("FLOAT");
    spec.lr_drop = optimization->add_option("--lr-drop", spec.request.lr_drop, "Step scheduler drop epoch");
    spec.lr_drop->type_name("INT");
    spec.lr_scheduler = optimization->add_option("--lr-scheduler", spec.request.lr_scheduler,
                                                 "Learning rate scheduler: step or cosine");
    spec.lr_min_factor = optimization->add_option("--lr-min-factor", spec.request.lr_min_factor,
                                                  "Minimum LR multiplier for cosine decay");
    spec.lr_min_factor->type_name("FLOAT");
    spec.warmup_epochs =
        optimization->add_option("--warmup-epochs", spec.request.warmup_epochs, "Warmup duration in epochs");
    spec.warmup_epochs->type_name("FLOAT");
    spec.warmup_momentum =
        optimization->add_option("--warmup-momentum", spec.request.warmup_momentum, "Muon warmup momentum start value");
    spec.warmup_momentum->type_name("FLOAT");
    optimization->add_option("--clip-max-norm", spec.request.clip_max_norm, "Gradient clipping max norm")
        ->type_name("FLOAT");
    spec.fused_optimizer =
        optimization->add_flag("--fused-optimizer,!--no-fused-optimizer", spec.request.fused_optimizer,
                               "Use the native fused AdamW backend when available (AdamW only)");
    optimization->add_flag("--use-ema,!--no-ema", spec.request.use_ema, "Maintain an EMA shadow model");
    optimization->add_flag("--validation-loss,!--no-validation-loss", spec.request.validation_loss,
                           "Calculate exact criterion loss during validation");
    optimization->add_flag("--validation-profile,!--no-validation-profile", spec.request.validation_profile,
                           "Write detailed training-validation JSONL phase records");
    spec.ema_decay = optimization->add_option("--ema-decay", spec.request.ema_decay, "EMA decay factor");
    spec.ema_decay->type_name("FLOAT");
    spec.ema_tau = optimization->add_option("--ema-tau", spec.request.ema_tau, "EMA warmup tau in steps");
    spec.ema_tau->type_name("INT");
    optimization
        ->add_option("--eval-max-dets", spec.request.eval_max_dets, "Maximum detections per image during validation")
        ->type_name("INT");
    auto* augmentation = command->add_option_group("GPU augmentation");
    augmentation->add_flag("--gpu-augment,!--no-gpu-augment", spec.request.gpu_augmentation.enabled,
                           "Apply fused CUDA-only training augmentations");
    const auto add_augmentation_group = [augmentation](const std::string& name, AugmentationGroupConfig& group,
                                                       const char* label) {
        augmentation
            ->add_option("--aug-" + name + "-prob", group.probability, std::string(label) + " selection probability")
            ->type_name("FLOAT");
        augmentation
            ->add_option("--aug-" + name + "-min-strength", group.min_strength,
                         std::string(label) + " minimum sampled strength")
            ->type_name("FLOAT");
        augmentation
            ->add_option("--aug-" + name + "-max-strength", group.max_strength,
                         std::string(label) + " maximum sampled strength")
            ->type_name("FLOAT");
    };
    add_augmentation_group("geometry", spec.request.gpu_augmentation.geometry, "Geometry");
    add_augmentation_group("resize", spec.request.gpu_augmentation.resize, "Resize");
    add_augmentation_group("color", spec.request.gpu_augmentation.color, "Color");
    add_augmentation_group("noise", spec.request.gpu_augmentation.noise, "Noise");
    add_augmentation_group("blur", spec.request.gpu_augmentation.blur, "Blur");
    add_augmentation_group("occlusion", spec.request.gpu_augmentation.occlusion, "Occlusion");
    augmentation
        ->add_option("--aug-copy-paste-prob", spec.request.gpu_augmentation.copy_paste_probability,
                     "Single-instance copy-paste selection probability")
        ->type_name("FLOAT");
    auto* execution = command->add_option_group("Execution");
    spec.device_id = execution->add_option("--device-id", spec.request.device_id, "Single CUDA device id");
    spec.device_id->type_name("INT");
    spec.device_ids = execution->add_option("--device-ids", spec.device_ids_text,
                                            "Comma-separated CUDA device ids for distributed training");
    spec.device_ids->type_name("LIST");
    execution->add_option("--workers", spec.request.workers, "Dataset worker count")->type_name("INT");
    execution->add_option("--lanes", spec.request.lanes, "Parallel backend lane count")->type_name("INT");
    execution->add_option("--cpu-affinity", spec.request.cpu_affinity, "Linux CPU list or range string for workers");
    execution->add_option("--prefetch-factor", spec.request.prefetch_factor, "Per-rank prefetch factor")
        ->type_name("INT");
    execution->add_option("--print-freq", spec.request.print_freq, "Logging frequency in training steps")
        ->type_name("INT");
    execution->add_option("--seed", spec.request.seed, "Random seed")->type_name("INT");
    execution->add_flag("--amp,!--no-amp", spec.request.amp, "Enable automatic mixed precision");
    execution->add_flag("--progress,!--no-progress", spec.request.progress_bar, "Render interactive progress output");
    add_compile_mode_option(execution, spec.compile_mode);
    auto* distributed = command->add_option_group("Distributed (internal)");
    spec.dist_worker = distributed->add_flag("--dist-worker", spec.request.distributed_worker,
                                             "Internal flag for forked distributed workers");
    spec.dist_rank = distributed->add_option("--dist-rank", spec.request.distributed_rank, "Internal worker rank");
    spec.dist_rank->type_name("INT");
    spec.dist_world_size =
        distributed->add_option("--dist-world-size", spec.request.distributed_world_size, "Internal worker world size");
    spec.dist_world_size->type_name("INT");
    spec.dist_store_file = add_path_option(distributed, "--dist-store-file", spec.request.distributed_store_path,
                                           "Internal distributed rendezvous file");
    if (hide_gui_unsupported_options) {
        spec.fused_optimizer->group("");
        spec.ema_decay->group("");
        spec.ema_tau->group("");
        spec.dist_worker->group("");
        spec.dist_rank->group("");
        spec.dist_world_size->group("");
        spec.dist_store_file->group("");
    }
}

inline CompilationMode parse_compilation_mode(const std::string& value) {
    if (value == "none")
        return CompilationMode::kNone;
    if (value == "selective")
        return CompilationMode::kSelective;
    if (value == "full")
        return CompilationMode::kFullTrace;
    throw std::runtime_error("--compile-mode must be 'none', 'selective', or 'full', got: " + value);
}

inline TrainOptimizerKind parse_train_optimizer_kind(std::string value) {
    value = strings::to_lower(std::move(value));
    if (value == "adamw") {
        return TrainOptimizerKind::AdamW;
    }
    if (value == "muon") {
        return TrainOptimizerKind::Muon;
    }
    throw std::runtime_error("--optimizer must be 'adamw' or 'muon', got: " + value);
}

template <typename Options>
inline void require_exactly_one_model_input(const Options& options, const char* command) {
    const size_t selected_input_count = static_cast<size_t>(!options.weights_path.empty()) +
                                        static_cast<size_t>(!options.onnx_path.empty()) +
                                        static_cast<size_t>(!options.tensorrt_path.empty());
    if (selected_input_count != 1U) {
        throw std::runtime_error(std::string("rfdetr ") + command +
                                 " requires exactly one of --weights, --onnx, or --tensorrt");
    }
}

inline void require_exactly_one_model_input(const std::filesystem::path& onnx_path,
                                            const std::filesystem::path& tensorrt_path, const char* command) {
    const size_t selected_input_count =
        static_cast<size_t>(!onnx_path.empty()) + static_cast<size_t>(!tensorrt_path.empty());
    if (selected_input_count != 1U) {
        throw std::runtime_error(std::string("rfdetr ") + command + " requires exactly one of --onnx or --tensorrt");
    }
}

inline std::vector<int> parse_device_ids(const std::string& value) {
    std::vector<int> ids;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item.empty()) {
            throw std::runtime_error("rfdetr train --device-ids must not contain empty elements");
        }
        ids.push_back(std::stoi(item));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (ids.empty()) {
        throw std::runtime_error("rfdetr train --device-ids requires at least one CUDA device id");
    }
    return ids;
}

inline std::string join_strings(const std::vector<std::string>& values, const std::string_view separator) {
    std::string joined;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            joined += separator;
        }
        joined += values[index];
    }
    return joined;
}

inline void append_unsupported_gui_flag(std::vector<std::string>& unsupported_flags, const CLI::Option* option,
                                        const char* flag_name) {
    if (option != nullptr && option->count() > 0) {
        unsupported_flags.emplace_back(flag_name);
    }
}

inline void reject_unsupported_gui_flags(const char* command, const char* reason,
                                         const std::vector<std::string>& unsupported_flags) {
    if (unsupported_flags.empty()) {
        return;
    }
    throw std::runtime_error(std::string("GUI seeding for `mmltk rfdetr ") + command + "` does not support " + reason +
                             ": " + join_strings(unsupported_flags, ", "));
}

inline std::string infer_train_recipe_preset_name_with_native_fallback(const TrainRequest& request) {
    if (!request.preset_name.empty()) {
        return request.preset_name;
    }
    const auto source_checkpoint = !request.resume_path.empty() ? request.resume_path : request.weights_path;
    std::string preset_name = infer_train_recipe_preset_name_from_path(source_checkpoint);
    if (!preset_name.empty()) {
        return preset_name;
    }
    if (!is_native_checkpoint_file(source_checkpoint)) {
        return {};
    }
    const auto checkpoint = load_checkpoint(source_checkpoint);
    return checkpoint.metadata.preset_name;
}

inline void finalize_predict_command(PredictCommandSpec& spec) {
    spec.request.source_kind = PredictSourceKind::CompiledDataset;
    spec.request.compilation_mode = parse_compilation_mode(spec.compile_mode);
    mmltk::rfdetr::finalize_predict_request(spec.request);
}

inline void finalize_validate_command(ValidateCommandSpec& spec, const ValidateUnsupportedOptionMode unsupported_mode =
                                                                     ValidateUnsupportedOptionMode::AllowAll) {
    if (unsupported_mode == ValidateUnsupportedOptionMode::RejectGuiUnsupported) {
        std::vector<std::string> unsupported_flags;
        append_unsupported_gui_flag(unsupported_flags, spec.compile_workers, "--compile-workers");
        append_unsupported_gui_flag(unsupported_flags, spec.compile_cuda_mask_batch_size,
                                    "--compile-cuda-mask-batch-size");
        append_unsupported_gui_flag(unsupported_flags, spec.compile_cuda_device_id, "--compile-cuda-device-id");
        reject_unsupported_gui_flags("validate", "compile-worker tuning flags because the GUI has no matching fields",
                                     unsupported_flags);
    }
    mmltk::rfdetr::finalize_validate_request(spec.request);
}

inline void finalize_train_command(
    TrainCommandSpec& spec, const std::string_view fallback_preset_name = {},
    const TrainUnsupportedOptionMode unsupported_mode = TrainUnsupportedOptionMode::AllowAll) {
    spec.request.compilation_mode = parse_compilation_mode(spec.compile_mode);
    spec.request.optimizer = parse_train_optimizer_kind(spec.optimizer);
    if (spec.device_id && spec.device_ids && spec.device_id->count() > 0 && spec.device_ids->count() > 0) {
        throw std::runtime_error("rfdetr train accepts only one of --device-id or --device-ids");
    }
    if (spec.device_ids && spec.device_ids->count() > 0) {
        spec.request.device_ids = parse_device_ids(spec.device_ids_text);
    }

    auto apply_override_flag = [](bool& target, CLI::Option* option) {
        if (option != nullptr) {
            target = option->count() > 0;
        }
    };
    apply_override_flag(spec.request.recipe_overrides.lr, spec.lr);
    apply_override_flag(spec.request.recipe_overrides.lr_encoder, spec.lr_encoder);
    apply_override_flag(spec.request.recipe_overrides.lr_component_decay, spec.lr_component_decay);
    apply_override_flag(spec.request.recipe_overrides.encoder_layer_decay, spec.encoder_layer_decay);
    apply_override_flag(spec.request.recipe_overrides.momentum, spec.momentum);
    apply_override_flag(spec.request.recipe_overrides.weight_decay, spec.weight_decay);
    apply_override_flag(spec.request.recipe_overrides.lr_drop, spec.lr_drop);
    apply_override_flag(spec.request.recipe_overrides.lr_scheduler, spec.lr_scheduler);
    apply_override_flag(spec.request.recipe_overrides.lr_min_factor, spec.lr_min_factor);
    apply_override_flag(spec.request.recipe_overrides.warmup_epochs, spec.warmup_epochs);
    apply_override_flag(spec.request.recipe_overrides.warmup_momentum, spec.warmup_momentum);

    if (unsupported_mode == TrainUnsupportedOptionMode::RejectGuiUnsupported) {
        std::vector<std::string> unsupported_flags;
        append_unsupported_gui_flag(unsupported_flags, spec.ema_decay, "--ema-decay");
        append_unsupported_gui_flag(unsupported_flags, spec.ema_tau, "--ema-tau");
        append_unsupported_gui_flag(unsupported_flags, spec.fused_optimizer, "--fused-optimizer");
        append_unsupported_gui_flag(unsupported_flags, spec.dist_worker, "--dist-worker");
        append_unsupported_gui_flag(unsupported_flags, spec.dist_rank, "--dist-rank");
        append_unsupported_gui_flag(unsupported_flags, spec.dist_world_size, "--dist-world-size");
        append_unsupported_gui_flag(unsupported_flags, spec.dist_store_file, "--dist-store-file");
        reject_unsupported_gui_flags("train", "these flags because the GUI has no matching fields", unsupported_flags);
    }

    std::string recipe_preset_name = infer_train_recipe_preset_name_with_native_fallback(spec.request);
    if (recipe_preset_name.empty()) {
        recipe_preset_name = std::string(fallback_preset_name);
    }
    mmltk::rfdetr::finalize_train_request(spec.request, recipe_preset_name);
}

inline void append_bool_flag(std::vector<std::string>& args, const bool value, const std::string_view enabled,
                             const std::string_view disabled) {
    args.emplace_back(value ? enabled : disabled);
}

inline void append_option(std::vector<std::string>& args, const std::string_view name,
                          const std::filesystem::path& value) {
    if (value.empty()) {
        return;
    }
    args.emplace_back(name);
    args.push_back(value.string());
}

inline void append_option(std::vector<std::string>& args, const std::string_view name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    args.emplace_back(name);
    args.push_back(value);
}

inline void append_option(std::vector<std::string>& args, const std::string_view name, const int value) {
    args.emplace_back(name);
    args.push_back(std::to_string(value));
}

inline void append_option(std::vector<std::string>& args, const std::string_view name, const double value) {
    args.emplace_back(name);
    args.push_back(std::to_string(value));
}

inline std::string join_device_ids(const std::vector<int>& device_ids) {
    std::string joined;
    for (size_t index = 0; index < device_ids.size(); ++index) {
        if (index > 0) {
            joined.push_back(',');
        }
        joined += std::to_string(device_ids[index]);
    }
    return joined;
}

inline std::vector<int> resolve_train_device_ids(const TrainRequest& request) {
    std::vector<int> resolved_device_ids = request.device_ids;
    if (resolved_device_ids.empty() && request.device_id >= 0) {
        resolved_device_ids.push_back(request.device_id);
    }
    if (resolved_device_ids.empty()) {
        throw std::runtime_error("rfdetr train requires at least one selected CUDA device");
    }
    return resolved_device_ids;
}

inline std::vector<std::string> serialize_train_command_arguments(const TrainRequest& request,
                                                                  const bool include_command_prefix = true) {
    validate_train_request(request);
    const std::vector<int> resolved_device_ids = resolve_train_device_ids(request);

    std::vector<std::string> args;
    args.reserve(112);
    if (include_command_prefix) {
        args.emplace_back("rfdetr");
        args.emplace_back("train");
    }
    append_option(args, "--train-compiled", request.train_compiled_path);
    append_option(args, "--val-compiled", request.val_compiled_path);
    append_option(args, "--test-compiled", request.test_compiled_path);
    append_option(args, "--resolution", request.resolution);
    append_option(args, "--output-dir", request.output_dir);
    append_option(args, "--weights", request.weights_path);
    append_option(args, "--resume", request.resume_path);
    append_option(args, "--preset", request.preset_name);
    append_option(args, "--batch-size", static_cast<int>(request.batch_size));
    append_option(args, "--val-batch-size", static_cast<int>(request.val_batch_size));
    append_option(args, "--epochs", request.epochs);
    append_option(args, "--grad-accum-steps", request.grad_accum_steps);
    append_option(args, "--optimizer", std::string(train_optimizer_cli_value(request.optimizer)));
    append_option(args, "--lr", request.lr);
    append_option(args, "--lr-encoder", request.lr_encoder);
    append_option(args, "--lr-component-decay", request.lr_component_decay);
    append_option(args, "--encoder-layer-decay", request.encoder_layer_decay);
    append_option(args, "--momentum", request.momentum);
    append_option(args, "--weight-decay", request.weight_decay);
    append_option(args, "--lr-drop", request.lr_drop);
    append_option(args, "--lr-scheduler", request.lr_scheduler);
    append_option(args, "--lr-min-factor", request.lr_min_factor);
    append_option(args, "--warmup-epochs", request.warmup_epochs);
    append_option(args, "--warmup-momentum", request.warmup_momentum);
    append_option(args, "--clip-max-norm", request.clip_max_norm);
    append_option(args, "--eval-max-dets", static_cast<int>(request.eval_max_dets));
    append_option(args, "--workers", request.workers);
    append_option(args, "--lanes", request.lanes);
    append_option(args, "--prefetch-factor", request.prefetch_factor);
    append_option(args, "--seed", request.seed);
    append_option(args, "--cpu-affinity", request.cpu_affinity);
    append_bool_flag(args, request.amp, "--amp", "--no-amp");
    append_bool_flag(args, request.fused_optimizer, "--fused-optimizer", "--no-fused-optimizer");
    append_bool_flag(args, request.use_ema, "--use-ema", "--no-ema");
    append_bool_flag(args, request.validation_loss, "--validation-loss", "--no-validation-loss");
    append_bool_flag(args, request.validation_profile, "--validation-profile", "--no-validation-profile");
    append_option(args, "--ema-decay", request.ema_decay);
    append_option(args, "--ema-tau", request.ema_tau);
    append_option(args, "--print-freq", request.print_freq);
    append_bool_flag(args, request.progress_bar, "--progress", "--no-progress");
    append_bool_flag(args, request.freeze_encoder, "--freeze-encoder", "--no-freeze-encoder");
    append_bool_flag(args, request.gpu_augmentation.enabled, "--gpu-augment", "--no-gpu-augment");
    const auto append_augmentation_group = [&args](const std::string_view name, const AugmentationGroupConfig& group) {
        append_option(args, std::string("--aug-") + std::string(name) + "-prob", group.probability);
        append_option(args, std::string("--aug-") + std::string(name) + "-min-strength", group.min_strength);
        append_option(args, std::string("--aug-") + std::string(name) + "-max-strength", group.max_strength);
    };
    append_augmentation_group("geometry", request.gpu_augmentation.geometry);
    append_augmentation_group("resize", request.gpu_augmentation.resize);
    append_augmentation_group("color", request.gpu_augmentation.color);
    append_augmentation_group("noise", request.gpu_augmentation.noise);
    append_augmentation_group("blur", request.gpu_augmentation.blur);
    append_augmentation_group("occlusion", request.gpu_augmentation.occlusion);
    append_option(args, "--aug-copy-paste-prob", request.gpu_augmentation.copy_paste_probability);
    append_option(args, "--compile-mode", std::string(compilation_mode_cli_value(request.compilation_mode)));
    if (resolved_device_ids.size() == 1U) {
        append_option(args, "--device-id", resolved_device_ids.front());
    } else {
        append_option(args, "--device-ids", join_device_ids(resolved_device_ids));
    }
    if (request.distributed_worker) {
        args.emplace_back("--dist-worker");
        append_option(args, "--dist-rank", request.distributed_rank);
        append_option(args, "--dist-world-size", request.distributed_world_size);
        append_option(args, "--dist-store-file", request.distributed_store_path);
    }
    return args;
}

}  
