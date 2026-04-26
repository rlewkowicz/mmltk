#include "gui_settings.h"
#include "view_state.h"
#include "mmltk_logging.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <utility>

namespace mmltk::gui {

namespace {

template <typename T>
void get_optional(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key)) {
        j.at(key).get_to(out);
    }
}

template <typename T>
T get_value_or(const nlohmann::json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it != j.end()) {
        it->get_to(fallback);
    }
    return fallback;
}

void get_optional_compile_mode(const nlohmann::json& j, const char* key, mmltk::rfdetr::CompilationMode& out) {
    int compile_mode = mmltk::rfdetr::compilation_mode_index(out);
    get_optional(j, key, compile_mode);
    out = mmltk::rfdetr::compilation_mode_from_index(compile_mode);
}

using mmltk::gui::DatasetPathState;
using mmltk::gui::ExecutionTuningState;
using mmltk::gui::ModelArtifactSelectionState;
using mmltk::gui::TrainPaneState;

constexpr const char* kDatasetPathsKey = "dataset_paths";
constexpr const char* kModelArtifactsKey = "model_artifacts";
constexpr const char* kExecutionKey = "execution";
constexpr const char* kTrainingKey = "training";
constexpr const char* kValidationKey = "validation";
constexpr const char* kPredictKey = "predict";
constexpr const char* kAnnotateKey = "annotate";
constexpr const char* kExportKey = "export";

struct ModelArtifactsShape {
    bool weights = true;
    bool onnx = true;
    bool tensorrt = true;
};

struct ExecutionShape {
    bool cpu_affinity = true;
    bool device_id = true;
    bool workers = true;
    bool lanes = true;
    bool allow_fp16 = true;
    bool progress_bar = true;
    bool compile_mode = true;
};

ModelArtifactsShape train_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.onnx = false;
    shape.tensorrt = false;
    return shape;
}

ModelArtifactsShape validate_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.weights = false;
    return shape;
}

ModelArtifactsShape export_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.tensorrt = false;
    return shape;
}

ExecutionShape train_execution_shape() {
    ExecutionShape shape;
    shape.device_id = false;
    shape.allow_fp16 = false;
    return shape;
}

ExecutionShape validate_execution_shape() {
    ExecutionShape shape;
    shape.lanes = false;
    shape.progress_bar = false;
    shape.compile_mode = false;
    return shape;
}

ExecutionShape annotate_execution_shape() {
    ExecutionShape shape;
    shape.cpu_affinity = false;
    shape.workers = false;
    shape.lanes = false;
    shape.progress_bar = false;
    return shape;
}

ExecutionShape export_execution_shape() {
    ExecutionShape shape;
    shape.cpu_affinity = false;
    shape.workers = false;
    shape.lanes = false;
    shape.progress_bar = false;
    shape.compile_mode = false;
    return shape;
}

nlohmann::json make_legacy_execution_json(const nlohmann::json& legacy, std::string cpu_affinity_default = {},
                                          int device_id_default = 0, int workers_default = 0, int lanes_default = 0,
                                          bool allow_fp16_default = true, bool progress_bar_default = false,
                                          int compile_mode_default = 1) {
    return nlohmann::json{
        {"cpu_affinity", get_value_or<std::string>(legacy, "cpu_affinity", std::move(cpu_affinity_default))},
        {"device_id", get_value_or<int>(legacy, "device_id", device_id_default)},
        {"workers", get_value_or<int>(legacy, "workers", workers_default)},
        {"lanes", get_value_or<int>(legacy, "lanes", lanes_default)},
        {"allow_fp16", get_value_or<bool>(legacy, "allow_fp16", allow_fp16_default)},
        {"progress_bar", get_value_or<bool>(legacy, "progress_bar", progress_bar_default)},
        {"compile_mode", get_value_or<int>(legacy, "compile_mode", compile_mode_default)},
    };
}

void apply_source_json(const nlohmann::json& source, SourceSelectionState& s) {
    int kind = static_cast<int>(s.kind);
    get_optional(source, "kind", kind);
    s.kind = static_cast<SourceKind>(kind);
    get_optional(source, "compiled_path", s.compiled_path);
    get_optional(source, "single_image_path", s.single_image_path);
    get_optional(source, "image_directory", s.image_directory);
    get_optional(source, "recursive", s.recursive);
    get_optional(source, "device_index", s.device_index);
    get_optional(source, "capture_width", s.capture_width);
    get_optional(source, "capture_height", s.capture_height);
    get_optional(source, "capture_fps", s.capture_fps);
    get_optional(source, "v4l2_buffer_count", s.v4l2_buffer_count);
    get_optional(source, "crop_x", s.crop_x);
    get_optional(source, "crop_y", s.crop_y);
    get_optional(source, "crop_width", s.crop_width);
    get_optional(source, "crop_height", s.crop_height);
}

nlohmann::json snapshot_dataset_paths(const DatasetPathState& state) {
    return nlohmann::json{
        {"compiled_path", state.compiled_path},
        {"source_dir", state.source_dir},
        {"train_compiled_path", state.train_compiled_path},
        {"val_compiled_path", state.val_compiled_path},
        {"test_compiled_path", state.test_compiled_path},
    };
}

void apply_dataset_paths_json(const nlohmann::json& json, DatasetPathState& state) {
    get_optional(json, "compiled_path", state.compiled_path);
    get_optional(json, "source_dir", state.source_dir);
    get_optional(json, "train_compiled_path", state.train_compiled_path);
    get_optional(json, "val_compiled_path", state.val_compiled_path);
    get_optional(json, "test_compiled_path", state.test_compiled_path);
}

nlohmann::json snapshot_train_execution_target(const TrainExecutionPaneState& state) {
    return nlohmann::json{
        {"execution_target", static_cast<int>(state.execution_target)},
        {"local_device_ids", state.local_device_ids},
        {"remote_family_enabled", state.remote_family_enabled},
        {"remote_container_image", state.remote_container_image},
        {"remote_launch_template", state.remote_launch_template},
    };
}

void apply_train_execution_target_json(const nlohmann::json& json, TrainExecutionPaneState& state) {
    int execution_target = static_cast<int>(state.execution_target);
    get_optional(json, "execution_target", execution_target);
    state.execution_target = static_cast<TrainExecutionTarget>(execution_target);
    get_optional(json, "local_device_ids", state.local_device_ids);
    get_optional(json, "remote_family_enabled", state.remote_family_enabled);
    get_optional(json, "remote_container_image", state.remote_container_image);
    get_optional(json, "remote_launch_template", state.remote_launch_template);
}

nlohmann::json recipe_overrides_json(const mmltk::rfdetr::TrainRecipeFieldOverrides& overrides) {
    return nlohmann::json{
        {"lr", overrides.lr},
        {"lr_encoder", overrides.lr_encoder},
        {"lr_component_decay", overrides.lr_component_decay},
        {"encoder_layer_decay", overrides.encoder_layer_decay},
        {"momentum", overrides.momentum},
        {"weight_decay", overrides.weight_decay},
        {"warmup_epochs", overrides.warmup_epochs},
        {"warmup_momentum", overrides.warmup_momentum},
        {"lr_min_factor", overrides.lr_min_factor},
        {"lr_drop", overrides.lr_drop},
        {"lr_scheduler", overrides.lr_scheduler},
    };
}

nlohmann::json snapshot_train_workflow_state(const TrainViewState& state, const TrainPaneState& train_pane) {
    return nlohmann::json{
        {"input_mode", static_cast<int>(train_pane.input_mode)},
        {"output_dir", train_pane.output_dir},
        {"resume_path", train_pane.resume_path},
        {"batch_size", state.batch_size},
        {"val_batch_size", state.val_batch_size},
        {"epochs", state.epochs},
        {"grad_accum_steps", state.grad_accum_steps},
        {"eval_max_dets", state.eval_max_dets},
        {"lr_drop", state.lr_drop},
        {"ema_tau", state.ema_tau},
        {"print_freq", state.print_freq},
        {"prefetch_factor", state.prefetch_factor},
        {"seed", state.seed},
        {"lr", state.lr},
        {"lr_encoder", state.lr_encoder},
        {"lr_component_decay", state.lr_component_decay},
        {"encoder_layer_decay", state.encoder_layer_decay},
        {"momentum", state.momentum},
        {"weight_decay", state.weight_decay},
        {"warmup_epochs", state.warmup_epochs},
        {"warmup_momentum", state.warmup_momentum},
        {"lr_min_factor", state.lr_min_factor},
        {"clip_max_norm", state.clip_max_norm},
        {"ema_decay", state.ema_decay},
        {"lr_scheduler", state.lr_scheduler},
        {"use_ema", state.use_ema},
        {"amp", state.amp},
        {"freeze_encoder", state.freeze_encoder},
        {"fused_optimizer", state.fused_optimizer},
        {"optimizer", static_cast<int>(state.optimizer)},
        {"distributed_store_path", state.distributed_store_path},
        {"distributed_rank", state.distributed_rank},
        {"distributed_world_size", state.distributed_world_size},
        {"distributed_worker", state.distributed_worker},
        {"recipe_overrides", recipe_overrides_json(state.recipe_overrides)},
    };
}

nlohmann::json snapshot_model_artifacts(const ModelArtifactSelectionState& state, const ModelArtifactsShape& shape) {
    nlohmann::json json = nlohmann::json::object();
    if (shape.weights) {
        json["weights_path"] = state.weights_path;
    }
    if (shape.onnx) {
        json["onnx_path"] = state.onnx_path;
    }
    if (shape.tensorrt) {
        json["tensorrt_path"] = state.tensorrt_path;
    }
    return json;
}

void apply_model_artifacts_json(const nlohmann::json& json, ModelArtifactSelectionState& state,
                                const ModelArtifactsShape& shape) {
    if (shape.weights) {
        get_optional(json, "weights_path", state.weights_path);
    }
    if (shape.onnx) {
        get_optional(json, "onnx_path", state.onnx_path);
    }
    if (shape.tensorrt) {
        get_optional(json, "tensorrt_path", state.tensorrt_path);
    }
}

nlohmann::json snapshot_execution(const ExecutionTuningState& state, const ExecutionShape& shape) {
    nlohmann::json json = nlohmann::json::object();
    if (shape.cpu_affinity) {
        json["cpu_affinity"] = state.cpu_affinity;
    }
    if (shape.device_id) {
        json["device_id"] = state.device_id;
    }
    if (shape.workers) {
        json["workers"] = state.workers;
    }
    if (shape.lanes) {
        json["lanes"] = state.lanes;
    }
    if (shape.allow_fp16) {
        json["allow_fp16"] = state.allow_fp16;
    }
    if (shape.progress_bar) {
        json["progress_bar"] = state.progress_bar;
    }
    if (shape.compile_mode) {
        json["compile_mode"] = mmltk::rfdetr::compilation_mode_index(state.compile_mode);
    }
    return json;
}

void apply_execution_json(const nlohmann::json& json, ExecutionTuningState& state, const ExecutionShape& shape) {
    if (shape.cpu_affinity) {
        get_optional(json, "cpu_affinity", state.cpu_affinity);
    }
    if (shape.device_id) {
        get_optional(json, "device_id", state.device_id);
    }
    if (shape.workers) {
        get_optional(json, "workers", state.workers);
    }
    if (shape.lanes) {
        get_optional(json, "lanes", state.lanes);
    }
    if (shape.allow_fp16) {
        get_optional(json, "allow_fp16", state.allow_fp16);
    }
    if (shape.progress_bar) {
        get_optional(json, "progress_bar", state.progress_bar);
    }
    if (shape.compile_mode) {
        get_optional_compile_mode(json, "compile_mode", state.compile_mode);
    }
}

nlohmann::json normalize_train_workflow_v2_to_v3(const nlohmann::json& legacy) {
    nlohmann::json normalized = nlohmann::json::object();
    normalized[kDatasetPathsKey] = snapshot_dataset_paths(DatasetPathState{
        get_value_or<std::string>(legacy, "train_compiled_path", {}),
        std::string{},
        get_value_or<std::string>(legacy, "train_compiled_path", {}),
        get_value_or<std::string>(legacy, "val_compiled_path", {}),
        get_value_or<std::string>(legacy, "test_compiled_path", {}),
    });
    normalized[kModelArtifactsKey] = nlohmann::json{
        {"weights_path", get_value_or<std::string>(legacy, "weights_path", {})},
        {"onnx_path", get_value_or<std::string>(legacy, "onnx_path", {})},
        {"tensorrt_path", get_value_or<std::string>(legacy, "tensorrt_path", {})},
    };
    normalized[kExecutionKey] = make_legacy_execution_json(legacy);
    constexpr std::array<bool, 5> default_remote_family_enabled{true, true, true, true, true};
    const TrainExecutionPaneState execution_target_state{
        static_cast<TrainExecutionTarget>(get_value_or<int>(legacy, "execution_target", 0)),
        get_value_or<std::vector<int>>(legacy, "local_device_ids", std::vector<int>{0}),
        get_value_or<std::array<bool, 5>>(legacy, "remote_family_enabled", default_remote_family_enabled),
        get_value_or<std::string>(legacy, "remote_container_image", {}),
        get_value_or<std::string>(legacy, "remote_launch_template", {}),
    };
    nlohmann::json training_json = nlohmann::json{
        {"input_mode", get_value_or<int>(legacy, "input_mode", 0)},
        {"output_dir", get_value_or<std::string>(legacy, "output_dir", "./gui-train-output")},
        {"distributed_store_path", get_value_or<std::string>(legacy, "distributed_store_path", {})},
        {"resume_path", get_value_or<std::string>(legacy, "resume_path", {})},
        {"batch_size", get_value_or<int>(legacy, "batch_size", 2)},
        {"val_batch_size", get_value_or<int>(legacy, "val_batch_size", 0)},
        {"epochs", get_value_or<int>(legacy, "epochs", 12)},
        {"grad_accum_steps", get_value_or<int>(legacy, "grad_accum_steps", 1)},
        {"eval_max_dets", get_value_or<int>(legacy, "eval_max_dets", 500)},
        {"lr_drop", get_value_or<int>(legacy, "lr_drop", 100)},
        {"ema_tau", get_value_or<int>(legacy, "ema_tau", 100)},
        {"print_freq", get_value_or<int>(legacy, "print_freq", 100)},
        {"prefetch_factor", get_value_or<int>(legacy, "prefetch_factor", 2)},
        {"seed", get_value_or<int>(legacy, "seed", 42)},
        {"amp", get_value_or<bool>(legacy, "amp", true)},
        {"freeze_encoder", get_value_or<bool>(legacy, "freeze_encoder", false)},
        {"fused_optimizer", get_value_or<bool>(legacy, "fused_optimizer", true)},
        {"optimizer", get_value_or<int>(legacy, "optimizer", 0)},
        {"distributed_rank", get_value_or<int>(legacy, "distributed_rank", 0)},
        {"distributed_world_size", get_value_or<int>(legacy, "distributed_world_size", 1)},
        {"distributed_worker", get_value_or<bool>(legacy, "distributed_worker", false)},
        {"recipe_overrides", nlohmann::json::object()},
        {"lr", get_value_or<double>(legacy, "lr", 1.0e-4)},
        {"lr_encoder", get_value_or<double>(legacy, "lr_encoder", 1.5e-4)},
        {"lr_component_decay", get_value_or<double>(legacy, "lr_component_decay", 0.7)},
        {"encoder_layer_decay", get_value_or<double>(legacy, "encoder_layer_decay", 0.8)},
        {"momentum", get_value_or<double>(legacy, "momentum", 0.95)},
        {"weight_decay", get_value_or<double>(legacy, "weight_decay", 1.0e-4)},
        {"warmup_epochs", get_value_or<double>(legacy, "warmup_epochs", 0.0)},
        {"warmup_momentum", get_value_or<double>(legacy, "warmup_momentum", 0.0)},
        {"lr_min_factor", get_value_or<double>(legacy, "lr_min_factor", 0.0)},
        {"clip_max_norm", get_value_or<double>(legacy, "clip_max_norm", 0.1)},
        {"ema_decay", get_value_or<double>(legacy, "ema_decay", 0.993)},
        {"lr_scheduler", get_value_or<std::string>(legacy, "lr_scheduler", "step")},
    };
    training_json.update(snapshot_train_execution_target(execution_target_state));
    normalized[kTrainingKey] = std::move(training_json);
    normalized[kTrainingKey]["recipe_overrides"] =
        legacy.contains("recipe_overrides") ? legacy.at("recipe_overrides") : nlohmann::json::object();
    return normalized;
}

nlohmann::json normalize_validate_workflow_v2_to_v3(const nlohmann::json& legacy) {
    return nlohmann::json{
        {kDatasetPathsKey,
         {
             {"compiled_path", get_value_or<std::string>(legacy, "compiled_path", {})},
             {"source_dir", get_value_or<std::string>(legacy, "source_dir", {})},
             {"train_compiled_path", std::string{}},
             {"val_compiled_path", std::string{}},
             {"test_compiled_path", std::string{}},
         }},
        {kModelArtifactsKey,
         {
             {"weights_path", std::string{}},
             {"onnx_path", get_value_or<std::string>(legacy, "onnx_path", {})},
             {"tensorrt_path", get_value_or<std::string>(legacy, "tensorrt_path", {})},
         }},
        {kExecutionKey, make_legacy_execution_json(legacy)},
        {kValidationKey,
         {
             {"save_engine_path", get_value_or<std::string>(legacy, "save_engine_path", {})},
             {"report_json_path", get_value_or<std::string>(legacy, "report_json_path", {})},
             {"split", get_value_or<std::string>(legacy, "split", "val")},
             {"eval_order", get_value_or<std::string>(legacy, "eval_order", "onnx,tensorrt")},
             {"resolution", get_value_or<int>(legacy, "resolution", 432)},
             {"limit_images", get_value_or<int>(legacy, "limit_images", 0)},
             {"alignment_images", get_value_or<int>(legacy, "alignment_images", 16)},
             {"eval_max_dets", get_value_or<int>(legacy, "eval_max_dets", 500)},
             {"batch_size", get_value_or<int>(legacy, "batch_size", 1)},
             {"prefetch_factor", get_value_or<int>(legacy, "prefetch_factor", 2)},
             {"recompile", get_value_or<bool>(legacy, "recompile", false)},
             {"profile", get_value_or<bool>(legacy, "profile", false)},
             {"write_report_json", get_value_or<bool>(legacy, "write_report_json", true)},
             {"compile_workers", get_value_or<int>(legacy, "compile_workers", -1)},
             {"compile_cuda_mask_batch_size", get_value_or<int>(legacy, "compile_cuda_mask_batch_size", 0)},
             {"compile_cuda_device_id", get_value_or<int>(legacy, "compile_cuda_device_id", 0)},
             {"log_mode", get_value_or<int>(legacy, "log_mode", 0)},
         }},
    };
}

nlohmann::json make_legacy_model_artifacts_json(const nlohmann::json& legacy) {
    return {
        {"weights_path", get_value_or<std::string>(legacy, "weights_path", {})},
        {"onnx_path", get_value_or<std::string>(legacy, "onnx_path", {})},
        {"tensorrt_path", get_value_or<std::string>(legacy, "tensorrt_path", {})},
    };
}

nlohmann::json make_legacy_detection_workflow_base_json(const nlohmann::json& legacy) {
    return {
        {"source", legacy.contains("source") ? legacy.at("source") : nlohmann::json::object()},
        {kModelArtifactsKey, make_legacy_model_artifacts_json(legacy)},
        {kExecutionKey, make_legacy_execution_json(legacy)},
    };
}

nlohmann::json normalize_predict_workflow_v2_to_v3(const nlohmann::json& legacy) {
    nlohmann::json normalized = make_legacy_detection_workflow_base_json(legacy);
    normalized[kPredictKey] = {
        {"output_path", get_value_or<std::string>(legacy, "output_path", "./predictions.json")},
        {"backend", get_value_or<std::string>(legacy, "backend", "auto")},
        {"model_input", get_value_or<int>(legacy, "model_input", static_cast<int>(ModelInputMode::Weights))},
        {"batch_size", get_value_or<int>(legacy, "batch_size", 4)},
        {"max_dets_per_image", get_value_or<int>(legacy, "max_dets_per_image", 500)},
        {"live_split_count", get_value_or<int>(legacy, "live_split_count", 1)},
        {"threshold", get_value_or<float>(legacy, "threshold", 0.25f)},
    };
    return normalized;
}

nlohmann::json normalize_annotate_workflow_v2_to_v3(const nlohmann::json& legacy) {
    nlohmann::json normalized = make_legacy_detection_workflow_base_json(legacy);
    normalized[kAnnotateKey] = {
        {"output_dir", get_value_or<std::string>(legacy, "output_dir", "./annotated-scenes")},
        {"split", get_value_or<std::string>(legacy, "split", "train")},
        {"backend", get_value_or<std::string>(legacy, "backend", "auto")},
        {"model_input", get_value_or<int>(legacy, "model_input", static_cast<int>(ModelInputMode::None))},
        {"device_id", get_value_or<int>(legacy, "device_id", 0)},
        {"max_dets_per_image", get_value_or<int>(legacy, "max_dets_per_image", 300)},
        {"threshold", get_value_or<float>(legacy, "threshold", 0.25f)},
        {"allow_fp16", get_value_or<bool>(legacy, "allow_fp16", true)},
        {"full_frame", get_value_or<bool>(legacy, "full_frame", false)},
        {"compile_mode", get_value_or<int>(legacy, "compile_mode", 1)},
    };
    return normalized;
}

nlohmann::json normalize_export_workflow_v2_to_v3(const nlohmann::json& legacy) {
    return nlohmann::json{
        {kModelArtifactsKey,
         {
             {"weights_path", get_value_or<std::string>(legacy, "weights_path", {})},
             {"onnx_path", get_value_or<std::string>(legacy, "onnx_path", {})},
             {"tensorrt_path", std::string{}},
         }},
        {kExecutionKey, make_legacy_execution_json(legacy)},
        {kExportKey,
         {
             {"output_path", get_value_or<std::string>(legacy, "output_path", "./rfdetr-engine.trt")},
             {"opset_version", get_value_or<int>(legacy, "opset_version", 19)},
             {"build_tensorrt", get_value_or<bool>(legacy, "build_tensorrt", true)},
             {"simplify", get_value_or<bool>(legacy, "simplify", false)},
         }},
    };
}

nlohmann::json normalize_gui_settings_v2_to_v3(const nlohmann::json& legacy) {
    nlohmann::json normalized = legacy;
    normalized["schema_version"] = kGuiSettingsSchemaVersion;

    if (const auto workflows_it = legacy.find("workflows"); workflows_it != legacy.end() && workflows_it->is_object()) {
        nlohmann::json workflows = nlohmann::json::object();
        if (const auto train_it = workflows_it->find("train");
            train_it != workflows_it->end() && train_it->is_object()) {
            workflows["train"] = normalize_train_workflow_v2_to_v3(*train_it);
        }
        if (const auto validate_it = workflows_it->find("validate");
            validate_it != workflows_it->end() && validate_it->is_object()) {
            workflows["validate"] = normalize_validate_workflow_v2_to_v3(*validate_it);
        }
        if (const auto predict_it = workflows_it->find("predict");
            predict_it != workflows_it->end() && predict_it->is_object()) {
            workflows["predict"] = normalize_predict_workflow_v2_to_v3(*predict_it);
        }
        if (const auto annotate_it = workflows_it->find("annotate");
            annotate_it != workflows_it->end() && annotate_it->is_object()) {
            workflows["annotate"] = normalize_annotate_workflow_v2_to_v3(*annotate_it);
        }
        if (const auto export_it = workflows_it->find("export");
            export_it != workflows_it->end() && export_it->is_object()) {
            workflows["export"] = normalize_export_workflow_v2_to_v3(*export_it);
        }
        normalized["workflows"] = std::move(workflows);
    }

    return normalized;
}

nlohmann::json normalize_gui_settings_document_impl(const nlohmann::json& j) {
    if (!j.is_object()) {
        throw std::runtime_error("GUI settings must be a JSON object");
    }

    const auto schema_it = j.find("schema_version");
    if (schema_it == j.end()) {
        throw std::runtime_error("GUI settings schema_version is missing");
    }
    if (!schema_it->is_number_integer() && !schema_it->is_number_unsigned()) {
        throw std::runtime_error("GUI settings schema_version must be an integer");
    }

    const std::uint32_t schema_version = schema_it->get<std::uint32_t>();
    if (schema_version == kGuiSettingsSchemaVersion) {
        return j;
    }
    if (schema_version == 2U) {
        return normalize_gui_settings_v2_to_v3(j);
    }
    throw std::runtime_error("unsupported GUI settings schema_version");
}

}  // namespace

void to_json(nlohmann::json& j, const SourceSelectionState& s) {
    j = nlohmann::json{
        {"kind", static_cast<int>(s.kind)},
        {"compiled_path", s.compiled_path},
        {"single_image_path", s.single_image_path},
        {"image_directory", s.image_directory},
        {"recursive", s.recursive},
        {"device_index", s.device_index},
        {"capture_width", s.capture_width},
        {"capture_height", s.capture_height},
        {"capture_fps", s.capture_fps},
        {"v4l2_buffer_count", s.v4l2_buffer_count},
        {"crop_x", s.crop_x},
        {"crop_y", s.crop_y},
        {"crop_width", s.crop_width},
        {"crop_height", s.crop_height},
    };
}

void from_json(const nlohmann::json& j, SourceSelectionState& s) {
    apply_source_json(j, s);
}

void to_json(nlohmann::json& j, const ModelArtifactSelectionState& s) {
    j = nlohmann::json{
        {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
    };
}

void from_json(const nlohmann::json& j, ModelArtifactSelectionState& s) {
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
}

void to_json(nlohmann::json& j, const DatasetPathState& s) {
    j = nlohmann::json{
        {"compiled_path", s.compiled_path},
        {"source_dir", s.source_dir},
        {"train_compiled_path", s.train_compiled_path},
        {"val_compiled_path", s.val_compiled_path},
        {"test_compiled_path", s.test_compiled_path},
    };
}

void from_json(const nlohmann::json& j, DatasetPathState& s) {
    get_optional(j, "compiled_path", s.compiled_path);
    get_optional(j, "source_dir", s.source_dir);
    get_optional(j, "train_compiled_path", s.train_compiled_path);
    get_optional(j, "val_compiled_path", s.val_compiled_path);
    get_optional(j, "test_compiled_path", s.test_compiled_path);
}

void to_json(nlohmann::json& j, const ExecutionTuningState& s) {
    j = nlohmann::json{
        {"cpu_affinity", s.cpu_affinity},
        {"device_id", s.device_id},
        {"workers", s.workers},
        {"lanes", s.lanes},
        {"allow_fp16", s.allow_fp16},
        {"progress_bar", s.progress_bar},
        {"compile_mode", mmltk::rfdetr::compilation_mode_index(s.compile_mode)},
    };
}

void from_json(const nlohmann::json& j, ExecutionTuningState& s) {
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "workers", s.workers);
    get_optional(j, "lanes", s.lanes);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "progress_bar", s.progress_bar);
    get_optional_compile_mode(j, "compile_mode", s.compile_mode);
}

void load_recipe_overrides(const nlohmann::json& j, mmltk::rfdetr::TrainRecipeFieldOverrides& overrides) {
    get_optional(j, "lr", overrides.lr);
    get_optional(j, "lr_encoder", overrides.lr_encoder);
    get_optional(j, "lr_component_decay", overrides.lr_component_decay);
    get_optional(j, "encoder_layer_decay", overrides.encoder_layer_decay);
    get_optional(j, "momentum", overrides.momentum);
    get_optional(j, "weight_decay", overrides.weight_decay);
    get_optional(j, "warmup_epochs", overrides.warmup_epochs);
    get_optional(j, "warmup_momentum", overrides.warmup_momentum);
    get_optional(j, "lr_min_factor", overrides.lr_min_factor);
    get_optional(j, "lr_drop", overrides.lr_drop);
    get_optional(j, "lr_scheduler", overrides.lr_scheduler);
}

void to_json(nlohmann::json& j, const TrainViewState& s) {
    const DatasetPathState dataset_state = dataset_paths(s);
    const ModelArtifactSelectionState artifact_state = model_artifacts(s);
    const ExecutionTuningState execution_state = execution_tuning(s);
    const TrainPaneState train_pane = train_pane_state(s);
    j = snapshot_train_workflow_state(s, train_pane);
    j.update(nlohmann::json{
        {"train_compiled_path", dataset_state.train_compiled_path},
        {"val_compiled_path", dataset_state.val_compiled_path},
        {"test_compiled_path", dataset_state.test_compiled_path},
        {"weights_path", artifact_state.weights_path},
        {"cpu_affinity", execution_state.cpu_affinity},
        {"workers", execution_state.workers},
        {"lanes", execution_state.lanes},
        {"progress_bar", execution_state.progress_bar},
        {"compile_mode", mmltk::rfdetr::compilation_mode_index(execution_state.compile_mode)},
    });
    j.update(snapshot_train_execution_target(train_pane));
}

void from_json(const nlohmann::json& j, TrainViewState& s) {
    DatasetPathState dataset_state = dataset_paths(s);
    ModelArtifactSelectionState artifact_state = model_artifacts(s);
    ExecutionTuningState execution_state = execution_tuning(s);
    TrainPaneState train_pane = train_pane_state(s);
    get_optional(j, "train_compiled_path", dataset_state.train_compiled_path);
    get_optional(j, "val_compiled_path", dataset_state.val_compiled_path);
    get_optional(j, "test_compiled_path", dataset_state.test_compiled_path);
    get_optional(j, "output_dir", train_pane.output_dir);
    get_optional(j, "distributed_store_path", s.distributed_store_path);
    get_optional(j, "weights_path", artifact_state.weights_path);
    get_optional(j, "resume_path", train_pane.resume_path);
    get_optional(j, "cpu_affinity", execution_state.cpu_affinity);
    int input_mode = static_cast<int>(train_pane.input_mode);
    get_optional(j, "input_mode", input_mode);
    train_pane.input_mode = static_cast<TrainInputMode>(input_mode);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "val_batch_size", s.val_batch_size);
    get_optional(j, "epochs", s.epochs);
    get_optional(j, "grad_accum_steps", s.grad_accum_steps);
    get_optional(j, "eval_max_dets", s.eval_max_dets);
    get_optional(j, "lr_drop", s.lr_drop);
    get_optional(j, "ema_tau", s.ema_tau);
    get_optional(j, "print_freq", s.print_freq);
    get_optional(j, "prefetch_factor", s.prefetch_factor);
    get_optional(j, "seed", s.seed);
    get_optional(j, "workers", execution_state.workers);
    get_optional(j, "lanes", execution_state.lanes);
    get_optional(j, "lr", s.lr);
    get_optional(j, "lr_encoder", s.lr_encoder);
    get_optional(j, "lr_component_decay", s.lr_component_decay);
    get_optional(j, "encoder_layer_decay", s.encoder_layer_decay);
    get_optional(j, "momentum", s.momentum);
    get_optional(j, "weight_decay", s.weight_decay);
    get_optional(j, "warmup_epochs", s.warmup_epochs);
    get_optional(j, "warmup_momentum", s.warmup_momentum);
    get_optional(j, "lr_min_factor", s.lr_min_factor);
    get_optional(j, "clip_max_norm", s.clip_max_norm);
    get_optional(j, "ema_decay", s.ema_decay);
    get_optional(j, "lr_scheduler", s.lr_scheduler);
    get_optional(j, "use_ema", s.use_ema);
    get_optional(j, "amp", s.amp);
    get_optional(j, "progress_bar", execution_state.progress_bar);
    get_optional(j, "freeze_encoder", s.freeze_encoder);
    get_optional(j, "fused_optimizer", s.fused_optimizer);
    int optimizer = static_cast<int>(s.optimizer);
    get_optional(j, "optimizer", optimizer);
    s.optimizer = static_cast<TrainOptimizerMode>(optimizer);
    get_optional_compile_mode(j, "compile_mode", execution_state.compile_mode);
    get_optional(j, "distributed_rank", s.distributed_rank);
    get_optional(j, "distributed_world_size", s.distributed_world_size);
    get_optional(j, "distributed_worker", s.distributed_worker);
    apply_train_execution_target_json(j, train_pane);
    if (const auto overrides = j.find("recipe_overrides"); overrides != j.end() && overrides->is_object()) {
        load_recipe_overrides(*overrides, s.recipe_overrides);
    }
    apply_dataset_paths(s, dataset_state);
    apply_model_artifacts(s, artifact_state);
    apply_execution_tuning(s, execution_state);
    apply_train_pane_state(s, train_pane);
}

void to_json(nlohmann::json& j, const ValidateViewState& s) {
    j = nlohmann::json{
        {"compiled_path", s.compiled_path},
        {"source_dir", s.source_dir},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
        {"save_engine_path", s.save_engine_path},
        {"report_json_path", s.report_json_path},
        {"split", s.split},
        {"eval_order", s.eval_order},
        {"cpu_affinity", s.cpu_affinity},
        {"resolution", s.resolution},
        {"limit_images", s.limit_images},
        {"alignment_images", s.alignment_images},
        {"eval_max_dets", s.eval_max_dets},
        {"batch_size", s.batch_size},
        {"prefetch_factor", s.prefetch_factor},
        {"device_id", s.device_id},
        {"workers", s.workers},
        {"recompile", s.recompile},
        {"profile", s.profile},
        {"allow_fp16", s.allow_fp16},
        {"write_report_json", s.write_report_json},
        {"compile_workers", s.compile_workers},
        {"compile_cuda_mask_batch_size", s.compile_cuda_mask_batch_size},
        {"compile_cuda_device_id", s.compile_cuda_device_id},
        {"log_mode", static_cast<int>(s.log_mode)},
    };
}

void from_json(const nlohmann::json& j, ValidateViewState& s) {
    get_optional(j, "compiled_path", s.compiled_path);
    get_optional(j, "source_dir", s.source_dir);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "save_engine_path", s.save_engine_path);
    get_optional(j, "report_json_path", s.report_json_path);
    get_optional(j, "split", s.split);
    get_optional(j, "eval_order", s.eval_order);
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    get_optional(j, "resolution", s.resolution);
    get_optional(j, "limit_images", s.limit_images);
    get_optional(j, "alignment_images", s.alignment_images);
    get_optional(j, "eval_max_dets", s.eval_max_dets);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "prefetch_factor", s.prefetch_factor);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "workers", s.workers);
    get_optional(j, "recompile", s.recompile);
    get_optional(j, "profile", s.profile);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "write_report_json", s.write_report_json);
    get_optional(j, "compile_workers", s.compile_workers);
    get_optional(j, "compile_cuda_mask_batch_size", s.compile_cuda_mask_batch_size);
    get_optional(j, "compile_cuda_device_id", s.compile_cuda_device_id);
    int log_mode = static_cast<int>(s.log_mode);
    get_optional(j, "log_mode", log_mode);
    s.log_mode = static_cast<mmltk::rfdetr::ValidationLogMode>(log_mode);
}

void to_json(nlohmann::json& j, const PredictViewState& s) {
    j = nlohmann::json{
        {"source", s.source},
        {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},
        {"tensorrt_path", s.tensorrt_path},
        {"output_path", s.output_path},
        {"backend", s.backend},
        {"cpu_affinity", s.cpu_affinity},
        {"model_input", static_cast<int>(s.model_input)},
        {"batch_size", s.batch_size},
        {"max_dets_per_image", s.max_dets_per_image},
        {"live_split_count", s.live_split_count},
        {"device_id", s.device_id},
        {"workers", s.workers},
        {"lanes", s.lanes},
        {"threshold", s.threshold},
        {"allow_fp16", s.allow_fp16},
        {"progress_bar", s.progress_bar},
        {"compile_mode", mmltk::rfdetr::compilation_mode_index(s.compile_mode)},
    };
}

void from_json(const nlohmann::json& j, PredictViewState& s) {
    get_optional(j, "source", s.source);
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "output_path", s.output_path);
    get_optional(j, "backend", s.backend);
    get_optional(j, "cpu_affinity", s.cpu_affinity);
    int model_input = static_cast<int>(s.model_input);
    get_optional(j, "model_input", model_input);
    s.model_input = static_cast<ModelInputMode>(model_input);
    get_optional(j, "batch_size", s.batch_size);
    get_optional(j, "max_dets_per_image", s.max_dets_per_image);
    get_optional(j, "live_split_count", s.live_split_count);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "workers", s.workers);
    get_optional(j, "lanes", s.lanes);
    get_optional(j, "threshold", s.threshold);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "progress_bar", s.progress_bar);
    get_optional_compile_mode(j, "compile_mode", s.compile_mode);
}

void to_json(nlohmann::json& j, const UiSettingsState& s) {
    j = nlohmann::json{
        {"dark_mode", s.dark_mode},
        {"ui_scale", s.ui_scale},
        {"font_size", s.font_size},
        {"secondary_font_size", s.secondary_font_size},
        {"mono_font_size", s.mono_font_size},
        {"property_label_width", s.property_label_width},
        {"crop_edge_hit_half_width", s.crop_edge_hit_half_width},
        {"crop_corner_hit_size", s.crop_corner_hit_size},
        {"crop_handle_radius", s.crop_handle_radius},
        {"density", static_cast<int>(s.density)},
    };
}

void from_json(const nlohmann::json& j, UiSettingsState& s) {
    get_optional(j, "dark_mode", s.dark_mode);
    get_optional(j, "ui_scale", s.ui_scale);
    get_optional(j, "font_size", s.font_size);
    get_optional(j, "secondary_font_size", s.secondary_font_size);
    get_optional(j, "mono_font_size", s.mono_font_size);
    get_optional(j, "property_label_width", s.property_label_width);
    get_optional(j, "crop_edge_hit_half_width", s.crop_edge_hit_half_width);
    get_optional(j, "crop_corner_hit_size", s.crop_corner_hit_size);
    get_optional(j, "crop_handle_radius", s.crop_handle_radius);
    int density = static_cast<int>(s.density);
    get_optional(j, "density", density);
    s.density = static_cast<UiDensity>(density);
}

void to_json(nlohmann::json& j, const AnnotateViewState& s) {
    j = nlohmann::json{
        {"source", s.source},         {"weights_path", s.weights_path},
        {"onnx_path", s.onnx_path},   {"tensorrt_path", s.tensorrt_path},
        {"output_dir", s.output_dir}, {"split", s.split},
        {"backend", s.backend},       {"model_input", static_cast<int>(s.model_input)},
        {"device_id", s.device_id},   {"max_dets_per_image", s.max_dets_per_image},
        {"threshold", s.threshold},   {"allow_fp16", s.allow_fp16},
        {"full_frame", s.full_frame}, {"compile_mode", mmltk::rfdetr::compilation_mode_index(s.compile_mode)},
    };
}

void from_json(const nlohmann::json& j, AnnotateViewState& s) {
    get_optional(j, "source", s.source);
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "tensorrt_path", s.tensorrt_path);
    get_optional(j, "output_dir", s.output_dir);
    get_optional(j, "split", s.split);
    get_optional(j, "backend", s.backend);
    int model_input = static_cast<int>(s.model_input);
    get_optional(j, "model_input", model_input);
    s.model_input = static_cast<ModelInputMode>(model_input);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "max_dets_per_image", s.max_dets_per_image);
    get_optional(j, "threshold", s.threshold);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "full_frame", s.full_frame);
    get_optional_compile_mode(j, "compile_mode", s.compile_mode);
}

void to_json(nlohmann::json& j, const ExportViewState& s) {
    j = nlohmann::json{
        {"weights_path", s.weights_path},     {"onnx_path", s.onnx_path},         {"output_path", s.output_path},
        {"device_id", s.device_id},           {"opset_version", s.opset_version}, {"allow_fp16", s.allow_fp16},
        {"build_tensorrt", s.build_tensorrt}, {"simplify", s.simplify},
    };
}

void from_json(const nlohmann::json& j, ExportViewState& s) {
    get_optional(j, "weights_path", s.weights_path);
    get_optional(j, "onnx_path", s.onnx_path);
    get_optional(j, "output_path", s.output_path);
    get_optional(j, "device_id", s.device_id);
    get_optional(j, "opset_version", s.opset_version);
    get_optional(j, "allow_fp16", s.allow_fp16);
    get_optional(j, "build_tensorrt", s.build_tensorrt);
    get_optional(j, "simplify", s.simplify);
}

nlohmann::json snapshot_workflows(const WorkflowSettingsSnapshot& workflows) {
    nlohmann::json j = nlohmann::json::object();

    if (workflows.train != nullptr) {
        const TrainViewState& s = *workflows.train;
        const DatasetPathState dataset_state = dataset_paths(s);
        const ModelArtifactSelectionState artifact_state = model_artifacts(s);
        const ExecutionTuningState execution_state = execution_tuning(s);
        const TrainPaneState train_pane = train_pane_state(s);
        nlohmann::json train_json = nlohmann::json{
            {kDatasetPathsKey, snapshot_dataset_paths(dataset_state)},
            {kModelArtifactsKey, snapshot_model_artifacts(artifact_state, train_model_artifacts_shape())},
            {kExecutionKey, snapshot_execution(execution_state, train_execution_shape())},
        };
        nlohmann::json training_json = snapshot_train_workflow_state(s, train_pane);
        training_json.update(snapshot_train_execution_target(train_pane));
        train_json[kTrainingKey] = std::move(training_json);
        j["train"] = std::move(train_json);
    }

    if (workflows.validate != nullptr) {
        const ValidateViewState& s = *workflows.validate;
        const DatasetPathState dataset_state = dataset_paths(s);
        const ModelArtifactSelectionState artifact_state = model_artifacts(s);
        const ExecutionTuningState execution_state = execution_tuning(s);
        j["validate"] = nlohmann::json{
            {kDatasetPathsKey, snapshot_dataset_paths(dataset_state)},
            {kModelArtifactsKey, snapshot_model_artifacts(artifact_state, validate_model_artifacts_shape())},
            {kExecutionKey, snapshot_execution(execution_state, validate_execution_shape())},
            {kValidationKey,
             {
                 {"save_engine_path", s.save_engine_path},
                 {"report_json_path", s.report_json_path},
                 {"split", s.split},
                 {"eval_order", s.eval_order},
                 {"resolution", s.resolution},
                 {"limit_images", s.limit_images},
                 {"alignment_images", s.alignment_images},
                 {"eval_max_dets", s.eval_max_dets},
                 {"batch_size", s.batch_size},
                 {"prefetch_factor", s.prefetch_factor},
                 {"recompile", s.recompile},
                 {"profile", s.profile},
                 {"write_report_json", s.write_report_json},
                 {"compile_workers", s.compile_workers},
                 {"compile_cuda_mask_batch_size", s.compile_cuda_mask_batch_size},
                 {"compile_cuda_device_id", s.compile_cuda_device_id},
                 {"log_mode", static_cast<int>(s.log_mode)},
             }},
        };
    }

    if (workflows.predict != nullptr) {
        const PredictViewState& s = *workflows.predict;
        const ModelArtifactSelectionState artifact_state = model_artifacts(s);
        const ExecutionTuningState execution_state = execution_tuning(s);
        j["predict"] = nlohmann::json{
            {"source", s.source},
            {kModelArtifactsKey, snapshot_model_artifacts(artifact_state, ModelArtifactsShape{})},
            {kExecutionKey, snapshot_execution(execution_state, ExecutionShape{})},
            {kPredictKey,
             {
                 {"output_path", s.output_path},
                 {"backend", s.backend},
                 {"model_input", static_cast<int>(s.model_input)},
                 {"batch_size", s.batch_size},
                 {"max_dets_per_image", s.max_dets_per_image},
                 {"live_split_count", s.live_split_count},
                 {"threshold", s.threshold},
             }},
        };
    }

    if (workflows.annotate != nullptr) {
        const AnnotateViewState& s = *workflows.annotate;
        const ModelArtifactSelectionState artifact_state = model_artifacts(s);
        const ExecutionTuningState execution_state = execution_tuning(s);
        j["annotate"] = nlohmann::json{
            {"source", s.source},
            {kModelArtifactsKey, snapshot_model_artifacts(artifact_state, ModelArtifactsShape{})},
            {kExecutionKey, snapshot_execution(execution_state, annotate_execution_shape())},
            {kAnnotateKey,
             {
                 {"output_dir", s.output_dir},
                 {"split", s.split},
                 {"backend", s.backend},
                 {"model_input", static_cast<int>(s.model_input)},
                 {"max_dets_per_image", s.max_dets_per_image},
                 {"threshold", s.threshold},
                 {"full_frame", s.full_frame},
             }},
        };
    }

    if (workflows.export_state != nullptr) {
        const ExportViewState& s = *workflows.export_state;
        const ModelArtifactSelectionState artifact_state = model_artifacts(s);
        const ExecutionTuningState execution_state = execution_tuning(s);
        j["export"] = nlohmann::json{
            {kModelArtifactsKey, snapshot_model_artifacts(artifact_state, export_model_artifacts_shape())},
            {kExecutionKey, snapshot_execution(execution_state, export_execution_shape())},
            {kExportKey,
             {
                 {"output_path", s.output_path},
                 {"opset_version", s.opset_version},
                 {"build_tensorrt", s.build_tensorrt},
                 {"simplify", s.simplify},
             }},
        };
    }

    return j;
}

void apply_workflows(const nlohmann::json& j, WorkflowSettingsSnapshot& workflows) {
    const auto workflows_it = j.find("workflows");
    if (workflows_it == j.end() || !workflows_it->is_object()) {
        return;
    }

    if (workflows.train != nullptr) {
        const auto train_it = workflows_it->find("train");
        if (train_it != workflows_it->end() && train_it->is_object()) {
            TrainViewState& s = *workflows.train;
            DatasetPathState dataset_state = dataset_paths(s);
            ModelArtifactSelectionState artifact_state = model_artifacts(s);
            ExecutionTuningState execution_state = execution_tuning(s);
            TrainPaneState train_pane = train_pane_state(s);
            if (const auto datasets = train_it->find(kDatasetPathsKey);
                datasets != train_it->end() && datasets->is_object()) {
                apply_dataset_paths_json(*datasets, dataset_state);
            }
            if (const auto artifacts = train_it->find(kModelArtifactsKey);
                artifacts != train_it->end() && artifacts->is_object()) {
                apply_model_artifacts_json(*artifacts, artifact_state, train_model_artifacts_shape());
            }
            if (const auto execution = train_it->find(kExecutionKey);
                execution != train_it->end() && execution->is_object()) {
                apply_execution_json(*execution, execution_state, train_execution_shape());
            }
            apply_dataset_paths(s, dataset_state);
            apply_model_artifacts(s, artifact_state);
            apply_execution_tuning(s, execution_state);
            if (const auto training = train_it->find(kTrainingKey);
                training != train_it->end() && training->is_object()) {
                get_optional(*training, "output_dir", train_pane.output_dir);
                get_optional(*training, "distributed_store_path", s.distributed_store_path);
                get_optional(*training, "resume_path", train_pane.resume_path);
                int input_mode = static_cast<int>(train_pane.input_mode);
                get_optional(*training, "input_mode", input_mode);
                train_pane.input_mode = static_cast<TrainInputMode>(input_mode);
                get_optional(*training, "batch_size", s.batch_size);
                get_optional(*training, "val_batch_size", s.val_batch_size);
                get_optional(*training, "epochs", s.epochs);
                get_optional(*training, "grad_accum_steps", s.grad_accum_steps);
                get_optional(*training, "eval_max_dets", s.eval_max_dets);
                get_optional(*training, "lr_drop", s.lr_drop);
                get_optional(*training, "ema_tau", s.ema_tau);
                get_optional(*training, "print_freq", s.print_freq);
                get_optional(*training, "prefetch_factor", s.prefetch_factor);
                get_optional(*training, "seed", s.seed);
                get_optional(*training, "lr", s.lr);
                get_optional(*training, "lr_encoder", s.lr_encoder);
                get_optional(*training, "lr_component_decay", s.lr_component_decay);
                get_optional(*training, "encoder_layer_decay", s.encoder_layer_decay);
                get_optional(*training, "momentum", s.momentum);
                get_optional(*training, "weight_decay", s.weight_decay);
                get_optional(*training, "warmup_epochs", s.warmup_epochs);
                get_optional(*training, "warmup_momentum", s.warmup_momentum);
                get_optional(*training, "lr_min_factor", s.lr_min_factor);
                get_optional(*training, "clip_max_norm", s.clip_max_norm);
                get_optional(*training, "ema_decay", s.ema_decay);
                get_optional(*training, "lr_scheduler", s.lr_scheduler);
                get_optional(*training, "use_ema", s.use_ema);
                get_optional(*training, "amp", s.amp);
                get_optional(*training, "freeze_encoder", s.freeze_encoder);
                get_optional(*training, "fused_optimizer", s.fused_optimizer);
                int optimizer = static_cast<int>(s.optimizer);
                get_optional(*training, "optimizer", optimizer);
                s.optimizer = static_cast<TrainOptimizerMode>(optimizer);
                get_optional(*training, "distributed_rank", s.distributed_rank);
                get_optional(*training, "distributed_world_size", s.distributed_world_size);
                get_optional(*training, "distributed_worker", s.distributed_worker);
                apply_train_execution_target_json(*training, train_pane);
                if (const auto overrides = training->find("recipe_overrides");
                    overrides != training->end() && overrides->is_object()) {
                    load_recipe_overrides(*overrides, s.recipe_overrides);
                }
            }
            apply_train_pane_state(s, train_pane);
        }
    }

    if (workflows.validate != nullptr) {
        const auto validate_it = workflows_it->find("validate");
        if (validate_it != workflows_it->end() && validate_it->is_object()) {
            ValidateViewState& s = *workflows.validate;
            DatasetPathState dataset_state = dataset_paths(s);
            ModelArtifactSelectionState artifact_state = model_artifacts(s);
            ExecutionTuningState execution_state = execution_tuning(s);
            if (const auto datasets = validate_it->find(kDatasetPathsKey);
                datasets != validate_it->end() && datasets->is_object()) {
                apply_dataset_paths_json(*datasets, dataset_state);
            }
            if (const auto artifacts = validate_it->find(kModelArtifactsKey);
                artifacts != validate_it->end() && artifacts->is_object()) {
                apply_model_artifacts_json(*artifacts, artifact_state, validate_model_artifacts_shape());
            }
            if (const auto execution = validate_it->find(kExecutionKey);
                execution != validate_it->end() && execution->is_object()) {
                apply_execution_json(*execution, execution_state, validate_execution_shape());
            }
            apply_dataset_paths(s, dataset_state);
            apply_model_artifacts(s, artifact_state);
            apply_execution_tuning(s, execution_state);
            if (const auto validation = validate_it->find(kValidationKey);
                validation != validate_it->end() && validation->is_object()) {
                get_optional(*validation, "save_engine_path", s.save_engine_path);
                get_optional(*validation, "report_json_path", s.report_json_path);
                get_optional(*validation, "split", s.split);
                get_optional(*validation, "eval_order", s.eval_order);
                get_optional(*validation, "resolution", s.resolution);
                get_optional(*validation, "limit_images", s.limit_images);
                get_optional(*validation, "alignment_images", s.alignment_images);
                get_optional(*validation, "eval_max_dets", s.eval_max_dets);
                get_optional(*validation, "batch_size", s.batch_size);
                get_optional(*validation, "prefetch_factor", s.prefetch_factor);
                get_optional(*validation, "recompile", s.recompile);
                get_optional(*validation, "profile", s.profile);
                get_optional(*validation, "write_report_json", s.write_report_json);
                get_optional(*validation, "compile_workers", s.compile_workers);
                get_optional(*validation, "compile_cuda_mask_batch_size", s.compile_cuda_mask_batch_size);
                get_optional(*validation, "compile_cuda_device_id", s.compile_cuda_device_id);
                int log_mode = static_cast<int>(s.log_mode);
                get_optional(*validation, "log_mode", log_mode);
                s.log_mode = static_cast<mmltk::rfdetr::ValidationLogMode>(log_mode);
            }
        }
    }

    if (workflows.predict != nullptr) {
        const auto predict_it = workflows_it->find("predict");
        if (predict_it != workflows_it->end() && predict_it->is_object()) {
            PredictViewState& s = *workflows.predict;
            ModelArtifactSelectionState artifact_state = model_artifacts(s);
            ExecutionTuningState execution_state = execution_tuning(s);
            if (const auto source = predict_it->find("source"); source != predict_it->end() && source->is_object()) {
                apply_source_json(*source, s.source);
            }
            if (const auto artifacts = predict_it->find(kModelArtifactsKey);
                artifacts != predict_it->end() && artifacts->is_object()) {
                apply_model_artifacts_json(*artifacts, artifact_state, ModelArtifactsShape{});
            }
            if (const auto execution = predict_it->find(kExecutionKey);
                execution != predict_it->end() && execution->is_object()) {
                apply_execution_json(*execution, execution_state, ExecutionShape{});
            }
            apply_model_artifacts(s, artifact_state);
            apply_execution_tuning(s, execution_state);
            if (const auto predict = predict_it->find(kPredictKey);
                predict != predict_it->end() && predict->is_object()) {
                get_optional(*predict, "output_path", s.output_path);
                get_optional(*predict, "backend", s.backend);
                int model_input = static_cast<int>(s.model_input);
                get_optional(*predict, "model_input", model_input);
                s.model_input = static_cast<ModelInputMode>(model_input);
                get_optional(*predict, "batch_size", s.batch_size);
                get_optional(*predict, "max_dets_per_image", s.max_dets_per_image);
                get_optional(*predict, "live_split_count", s.live_split_count);
                get_optional(*predict, "threshold", s.threshold);
            }
        }
    }

    if (workflows.annotate != nullptr) {
        const auto annotate_it = workflows_it->find("annotate");
        if (annotate_it != workflows_it->end() && annotate_it->is_object()) {
            AnnotateViewState& s = *workflows.annotate;
            ModelArtifactSelectionState artifact_state = model_artifacts(s);
            ExecutionTuningState execution_state = execution_tuning(s);
            if (const auto source = annotate_it->find("source"); source != annotate_it->end() && source->is_object()) {
                apply_source_json(*source, s.source);
            }
            if (const auto artifacts = annotate_it->find(kModelArtifactsKey);
                artifacts != annotate_it->end() && artifacts->is_object()) {
                apply_model_artifacts_json(*artifacts, artifact_state, ModelArtifactsShape{});
            }
            if (const auto execution = annotate_it->find(kExecutionKey);
                execution != annotate_it->end() && execution->is_object()) {
                apply_execution_json(*execution, execution_state, annotate_execution_shape());
            }
            apply_model_artifacts(s, artifact_state);
            apply_execution_tuning(s, execution_state);
            if (const auto annotate = annotate_it->find(kAnnotateKey);
                annotate != annotate_it->end() && annotate->is_object()) {
                get_optional(*annotate, "output_dir", s.output_dir);
                get_optional(*annotate, "split", s.split);
                get_optional(*annotate, "backend", s.backend);
                int model_input = static_cast<int>(s.model_input);
                get_optional(*annotate, "model_input", model_input);
                s.model_input = static_cast<ModelInputMode>(model_input);
                get_optional(*annotate, "max_dets_per_image", s.max_dets_per_image);
                get_optional(*annotate, "threshold", s.threshold);
                get_optional(*annotate, "full_frame", s.full_frame);
            }
        }
    }

    if (workflows.export_state != nullptr) {
        const auto export_it = workflows_it->find("export");
        if (export_it != workflows_it->end() && export_it->is_object()) {
            ExportViewState& s = *workflows.export_state;
            ModelArtifactSelectionState artifact_state = model_artifacts(s);
            ExecutionTuningState execution_state = execution_tuning(s);
            if (const auto artifacts = export_it->find(kModelArtifactsKey);
                artifacts != export_it->end() && artifacts->is_object()) {
                apply_model_artifacts_json(*artifacts, artifact_state, export_model_artifacts_shape());
            }
            if (const auto execution = export_it->find(kExecutionKey);
                execution != export_it->end() && execution->is_object()) {
                apply_execution_json(*execution, execution_state, export_execution_shape());
            }
            apply_model_artifacts(s, artifact_state);
            apply_execution_tuning(s, execution_state);
            if (const auto export_settings = export_it->find(kExportKey);
                export_settings != export_it->end() && export_settings->is_object()) {
                get_optional(*export_settings, "output_path", s.output_path);
                get_optional(*export_settings, "opset_version", s.opset_version);
                get_optional(*export_settings, "build_tensorrt", s.build_tensorrt);
                get_optional(*export_settings, "simplify", s.simplify);
            }
        }
    }
}

nlohmann::json normalize_gui_settings_document(const nlohmann::json& j) {
    return normalize_gui_settings_document_impl(j);
}

nlohmann::json snapshot_gui_settings(const GuiSettingsSnapshot& snap) {
    nlohmann::json j;
    j["schema_version"] = kGuiSettingsSchemaVersion;
    j["current_view"] = static_cast<int>(snap.current_view);
    j["selected_preset"] = snap.selected_preset;
    if (snap.ui_settings)
        j["ui"] = *snap.ui_settings;
    const nlohmann::json workflows = snapshot_workflows(snap.workflows);
    if (!workflows.empty())
        j["workflows"] = workflows;
    return j;
}

void apply_gui_settings(const nlohmann::json& j, GuiSettingsSnapshot& snap) {
    const nlohmann::json normalized = normalize_gui_settings_document(j);
    if (normalized.contains("current_view")) {
        int v = normalized.at("current_view").get<int>();
        snap.current_view = static_cast<View>(v);
    }
    get_optional(normalized, "selected_preset", snap.selected_preset);
    if (normalized.contains("ui") && snap.ui_settings)
        normalized.at("ui").get_to(*snap.ui_settings);
    apply_workflows(normalized, snap.workflows);
}

GuiSettingsPersistence::GuiSettingsPersistence(std::string path)
    : path_(std::move(path)), writer_thread_(&GuiSettingsPersistence::writer_main, this) {}

GuiSettingsPersistence::~GuiSettingsPersistence() {
    flush();
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_should_stop_ = true;
    }
    writer_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

bool GuiSettingsPersistence::load(GuiSettingsSnapshot& snap) {
    std::ifstream file(path_);
    if (!file.is_open()) {
        return false;
    }
    try {
        const nlohmann::json raw = nlohmann::json::parse(file);
        const nlohmann::json normalized = normalize_gui_settings_document(raw);
        apply_gui_settings(normalized, snap);
        if (normalized != raw) {
            save_to_disk(normalized);
        }
        last_saved_ = normalized;
        return true;
    } catch (const std::exception& e) {
        mmltk::logging::logger("gui")->warn("[gui] ignoring malformed settings from {}: {}", path_, e.what());
        return false;
    }
}

void GuiSettingsPersistence::notify_frame(const GuiSettingsSnapshot& snap) {
    nlohmann::json current = snapshot_gui_settings(snap);
    if (current != last_saved_) {
        if (!dirty_) {
            dirty_ = true;
        }
        last_change_time_ = std::chrono::steady_clock::now();
        last_saved_ = current;
    }
    if (dirty_) {
        const auto elapsed = std::chrono::steady_clock::now() - last_change_time_;
        if (elapsed >= kSaveDelay) {
            bool coalesced_pending_save = false;
            {
                std::lock_guard<std::mutex> lock(writer_mutex_);
                coalesced_pending_save = save_in_flight_ && pending_save_.has_value();
            }
            if (!coalesced_pending_save) {
                enqueue_save(last_saved_);
                dirty_ = false;
            }
        }
    }
}

void GuiSettingsPersistence::flush() {
    if (dirty_) {
        enqueue_save(last_saved_);
        dirty_ = false;
    }
    std::unique_lock<std::mutex> lock(writer_mutex_);
    writer_cv_.wait(lock, [this]() { return !pending_save_.has_value() && !save_in_flight_; });
}

void GuiSettingsPersistence::enqueue_save(nlohmann::json j) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        if (pending_save_.has_value() && *pending_save_ == j) {
            return;
        }
        pending_save_ = std::move(j);
        notify = true;
    }
    if (notify) {
        writer_cv_.notify_all();
    }
}

void GuiSettingsPersistence::writer_main() {
    while (true) {
        std::optional<nlohmann::json> save_request;
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait(lock, [this]() { return writer_should_stop_ || pending_save_.has_value(); });
            if (writer_should_stop_ && !pending_save_.has_value()) {
                return;
            }
            save_in_flight_ = true;
            save_request = std::exchange(pending_save_, std::nullopt);
        }

        if (save_request.has_value()) {
            save_to_disk(*save_request);
        }

        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            save_in_flight_ = false;
        }
        writer_cv_.notify_all();
    }
}

void GuiSettingsPersistence::save_to_disk(const nlohmann::json& j) {
    const std::string tmp_path = path_ + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            mmltk::logging::logger("gui")->error("[gui] failed to write settings to {}", tmp_path);
            return;
        }
        file << j.dump(2) << '\n';
    }
    if (std::rename(tmp_path.c_str(), path_.c_str()) != 0) {
        mmltk::logging::logger("gui")->error("[gui] failed to rename {} -> {}", tmp_path, path_);
    }
}

}  // namespace mmltk::gui
