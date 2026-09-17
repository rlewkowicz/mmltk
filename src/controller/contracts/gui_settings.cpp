#include "src/controller/contracts/gui_settings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/view_state.h"
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/serialization/serialization.h"
import mmltk.common.logging.mmltk_logging;
namespace mmltk::controller::contracts {
namespace {
template <typename T>
void get_optional(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key)) { mmltk::frameworks::serialization::decode_json_value_exact(j.at(key), out); }
}
template <typename T>
T get_value_or(const nlohmann::json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it != j.end()) { it->get_to(fallback); }
    return fallback;
}
[[nodiscard]] float quantize_hundredths(const float value) noexcept { return std::round(value * 100.0F) / 100.0F; }
[[nodiscard]] double json_hundredths(const float value) noexcept { return std::round(static_cast<double>(value) * 100.0) / 100.0; }
void get_optional_compile_mode(const nlohmann::json& j, const char* key, mmltk::backend::models::rfdetr::CompilationMode& out) {
    int compile_mode = static_cast<int>(out);
    get_optional(j, key, compile_mode);
    out = compile_mode >= static_cast<int>(mmltk::backend::models::rfdetr::CompilationMode::kNone) &&
                  compile_mode <= static_cast<int>(mmltk::backend::models::rfdetr::CompilationMode::kFullTrace)
              ? static_cast<mmltk::backend::models::rfdetr::CompilationMode>(compile_mode)
              : mmltk::backend::models::rfdetr::CompilationMode::kSelective;
}
[[nodiscard]] ModelArtifactInputKind model_input_from_index(const int value, const ModelArtifactInputKind fallback) noexcept {
    switch (static_cast<ModelArtifactInputKind>(value)) {
        case ModelArtifactInputKind::Weights: return ModelArtifactInputKind::Weights;
        case ModelArtifactInputKind::Onnx: return ModelArtifactInputKind::Onnx;
        case ModelArtifactInputKind::TensorRt: return ModelArtifactInputKind::TensorRt;
        case ModelArtifactInputKind::None: return ModelArtifactInputKind::None;
    }
    return fallback;
}
[[nodiscard]] ModelSelectionSource model_selection_source_from_index(const int value,
                                                                     const ModelSelectionSource fallback = ModelSelectionSource::Canonical) noexcept {
    switch (static_cast<ModelSelectionSource>(value)) {
        case ModelSelectionSource::Canonical: return ModelSelectionSource::Canonical;
        case ModelSelectionSource::Custom: return ModelSelectionSource::Custom;
    }
    return fallback;
}
using mmltk::controller::contracts::ModelArtifactSelectionState;
constexpr const char* kDatasetPathsKey = "dataset_paths";
constexpr const char* kModelArtifactsKey = "model_artifacts";
constexpr const char* kExecutionKey = "execution";
constexpr const char* kTrainingKey = "training";
constexpr const char* kAugmentationKey = "augmentation";
constexpr const char* kValidationKey = "validation";
constexpr const char* kPredictKey = "predict";
constexpr const char* kAnnotateKey = "annotate";
constexpr const char* kExportKey = "export";
constexpr const char* kExploreKey = "explore";
struct ModelArtifactsShape {
    bool weights = true;
    bool onnx = true;
    bool tensorrt = true;
    bool preserve_comparison_paths = false;
    bool preserve_unselected_paths = false;
    const char* onnx_key = "onnx_path";
};
void normalize_model_artifacts(ModelArtifactSelectionState& state, const ModelArtifactsShape& shape) {
    const mmltk::backend::models::rfdetr::PresetCatalogEntry* preset = mmltk::backend::models::rfdetr::find_preset_catalog_entry(state.preset_name);
    const bool unsupported_input = (state.input == ModelArtifactInputKind::Weights && !shape.weights) ||
                                   (state.input == ModelArtifactInputKind::Onnx && !shape.onnx) ||
                                   (state.input == ModelArtifactInputKind::TensorRt && !shape.tensorrt);
    if (preset == nullptr || unsupported_input) {
        state = ModelArtifactSelectionState{};
        return;
    }
    if (state.resolution <= 0) { state.resolution = static_cast<int>(preset->resolution); }
    if (state.input == ModelArtifactInputKind::None) {
        if (shape.preserve_unselected_paths) { return; }
        state.weights_path.clear();
        state.onnx_path.clear();
        state.tensorrt_path.clear();
        return;
    }
    if (state.source == ModelSelectionSource::Canonical) {
        state.input = ModelArtifactInputKind::Weights;
        if (!shape.preserve_comparison_paths) {
            state.onnx_path.clear();
            state.tensorrt_path.clear();
        }
    }
}
ModelArtifactsShape train_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.onnx = false;
    shape.tensorrt = false;
    return shape;
}
ModelArtifactsShape validate_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.preserve_comparison_paths = true;
    return shape;
}
ModelArtifactsShape export_model_artifacts_shape() {
    ModelArtifactsShape shape;
    shape.tensorrt = false;
    shape.preserve_comparison_paths = true;
    shape.preserve_unselected_paths = true;
    shape.onnx_key = "onnx_input_path";
    return shape;
}
[[nodiscard]] const nlohmann::json* find_object(const nlohmann::json& parent, const char* key) {
    const auto it = parent.find(key);
    return it != parent.end() && it->is_object() ? &*it : nullptr;
}
// Marks a float field that is persisted rounded to hundredths in both directions.
template <typename T>
struct Hundredths {
    T& value;
};
template <typename T>
Hundredths(T&) -> Hundredths<T>;
// One field list per struct drives both serialization directions: the same visit_* lambda is invoked
// with a JsonFieldWriter to emit JSON and with a JsonFieldReader to apply it back onto the state.
struct JsonFieldWriter {
    nlohmann::json& json;
    template <typename T>
    void operator()(const char* key, const T& value) const {
        if constexpr (std::is_enum_v<T>) {
            json[key] = static_cast<int>(value);
        } else {
            json[key] = value;
        }
    }
    void operator()(const char* key, const mmltk::backend::models::rfdetr::CompilationMode& value) const { json[key] = static_cast<int>(value); }
    void operator()(const char* key, const mmltk::backend::models::rfdetr::TrainLrSchedulerKind& value) const {
        json[key] = mmltk::backend::models::rfdetr::cli_enum_spelling(value);
    }
    void operator()(const char* key, const mmltk::backend::models::rfdetr::TrainAssignmentKind& value) const {
        json[key] = mmltk::backend::models::rfdetr::cli_enum_spelling(value);
    }
    template <typename T>
    void operator()(const char* key, const Hundredths<T>& field) const {
        json[key] = json_hundredths(field.value);
    }
    template <typename T, typename FieldVisitor>
    void nested(const char* key, const T& value, const FieldVisitor& fields) const {
        nlohmann::json child = nlohmann::json::object();
        fields(value, JsonFieldWriter{child});
        json[key] = std::move(child);
    }
};
struct JsonFieldReader {
    const nlohmann::json& json;
    template <typename T>
    void operator()(const char* key, T& value) const {
        if constexpr (std::is_enum_v<T>) {
            int index = static_cast<int>(value);
            get_optional(json, key, index);
            value = static_cast<T>(index);
        } else {
            get_optional(json, key, value);
        }
    }
    void operator()(const char* key, mmltk::backend::models::rfdetr::CompilationMode& value) const { get_optional_compile_mode(json, key, value); }
    void operator()(const char* key, mmltk::backend::models::rfdetr::TrainLrSchedulerKind& value) const {
        const auto found = json.find(key);
        if (found == json.end()) return;
        if (found->is_number_integer()) {
            const int index = found->get<int>();
            const auto candidate = static_cast<mmltk::backend::models::rfdetr::TrainLrSchedulerKind>(index);
            if (mmltk::frameworks::reflection::enum_contains(candidate)) value = candidate;
            return;
        }
        if (!found->is_string()) return;
        if (const auto parsed = mmltk::backend::models::rfdetr::train_lr_scheduler_from_spelling(found->get<std::string>())) value = *parsed;
    }
    // CLEANUP-IGNORE: Assignment rejects invalid strings; scheduler loading tolerates legacy integers and ignores unsupported values.
    void operator()(const char* key, mmltk::backend::models::rfdetr::TrainAssignmentKind& value) const {
        const auto found = json.find(key);
        if (found == json.end()) return;
        if (!found->is_string()) throw std::runtime_error("training assignment must be a canonical string");
        const auto parsed = mmltk::backend::models::rfdetr::train_assignment_from_spelling(found->get<std::string>());
        if (!parsed) throw std::runtime_error("training assignment is invalid");
        value = *parsed;
    }
    void operator()(const char* key, ModelArtifactInputKind& value) const {
        int index = static_cast<int>(value);
        get_optional(json, key, index);
        value = model_input_from_index(index, value);
    }
    // CLEANUP-IGNORE: This enum retains its own persisted integer admission and fallback policy; adjacent enum policies differ.
    void operator()(const char* key, ModelSelectionSource& value) const {
        int index = static_cast<int>(value);
        get_optional(json, key, index);
        value = model_selection_source_from_index(index, value);
    }
    // CLEANUP-IGNORE: This enum retains its own persisted integer admission and fallback policy; adjacent enum policies differ.
    void operator()(const char* key, ExploreDatasetSource& value) const {
        int index = static_cast<int>(value);
        get_optional(json, key, index);
        value = index >= static_cast<int>(ExploreDatasetSource::Train) && index <= static_cast<int>(ExploreDatasetSource::Custom)
                    ? static_cast<ExploreDatasetSource>(index)
                    : ExploreDatasetSource::Train;
    }
    // CLEANUP-IGNORE: This enum retains its own persisted integer admission and fallback policy; adjacent enum policies differ.
    void operator()(const char* key, ExploreOrder& value) const {
        int index = static_cast<int>(value);
        get_optional(json, key, index);
        value = index == static_cast<int>(ExploreOrder::Shuffled) ? ExploreOrder::Shuffled : ExploreOrder::Sequential;
    }
    // CLEANUP-IGNORE: This enum retains its own persisted integer admission and fallback policy; adjacent enum policies differ.
    void operator()(const char* key, ExploreDetailScaleMode& value) const {
        int index = static_cast<int>(value);
        get_optional(json, key, index);
        switch (index) {
            case static_cast<int>(ExploreDetailScaleMode::Fast): value = ExploreDetailScaleMode::Fast; break;
            case static_cast<int>(ExploreDetailScaleMode::Neural): value = ExploreDetailScaleMode::Neural; break;
            case static_cast<int>(ExploreDetailScaleMode::Basic):
            default: value = ExploreDetailScaleMode::Basic; break;
        }
    }
    template <typename T>
    void operator()(const char* key, const Hundredths<T>& field) const {
        get_optional(json, key, field.value);
        field.value = quantize_hundredths(field.value);
    }
    template <typename T, typename FieldVisitor>
    void nested(const char* key, T& value, const FieldVisitor& fields) const {
        const auto found = json.find(key);
        if constexpr (std::is_same_v<T, mmltk::backend::models::rfdetr::TrainingSupervisionConfig>) {
            T candidate{};
            if (found == json.end()) {
                value = candidate;
                return;
            }
            if (!found->is_object()) throw std::runtime_error("training_supervision must be an object");
            fields(candidate, JsonFieldReader{*found});
            if (!mmltk::backend::models::rfdetr::training_supervision_config_valid(candidate))
                throw std::runtime_error("training_supervision violates canonical constraints");
            value = candidate;
        } else if constexpr (std::is_same_v<T, mmltk::backend::models::rfdetr::MatchFreeSupervisionConfig> ||
                             std::is_same_v<T, mmltk::backend::models::rfdetr::DenoisingSupervisionConfig>) {
            if (found == json.end()) return;
            if (!found->is_object()) throw std::runtime_error(std::string(key) + " must be an object");
            fields(value, JsonFieldReader{*found});
        } else if (const nlohmann::json* child = find_object(json, key)) {
            fields(value, JsonFieldReader{*child});
        }
    }
};
template <typename State, typename FieldVisitor>
[[nodiscard]] nlohmann::json snapshot_fields(const State& state, const FieldVisitor& fields) {
    nlohmann::json json = nlohmann::json::object();
    fields(state, JsonFieldWriter{json});
    return json;
}
template <class Record, class State, class Visitor>
void visit_record_fields(State& state, const Visitor& visit) {
    mmltk::frameworks::reflection::visit_materialized_bases<Record>([&]<class Base>() { visit_record_fields<Base>(state, visit); });
    mmltk::frameworks::reflection::visit_materialized_members<Record>(
        [&]<class Declaration>(const auto& field) { visit(field.member_name.data(), state.*Declaration::pointer); });
}
constexpr auto source_fields = [](auto& state, const auto& visit) { visit_record_fields<SourceSelectionState>(state, visit); };
constexpr auto train_dataset_fields = [](auto& state, const auto& visit) {
    visit("source_dir", state.dataset_source_dir);
    visit("compiled_directory", state.compiled_dataset_dir);
    visit("use_compiled_directory_defaults", state.use_compiled_directory_defaults);
    visit("train_compiled_path", state.request.train_compiled_path);
    visit("val_compiled_path", state.request.val_compiled_path);
    visit("test_compiled_path", state.request.test_compiled_path);
    visit("overwrite", state.overwrite_compiled_dataset);
    visit("compile_dimensions", state.compile_dimensions);
    visit("perceptual_downscale", state.compile_perceptual_downscale);
    visit("compile_benchmark_dataset_override", state.compile_benchmark_dataset_override);
};
constexpr auto validate_dataset_fields = [](auto& request, const auto& visit) {
    visit("compiled_path", request.compiled_path);
    visit("source_dir", request.source_dir);
};
constexpr auto train_execution_target_fields = [](auto& state, const auto& visit) { visit_record_fields<TrainExecutionPaneState>(state, visit); };
// Recipe scalars persisted under the same JSON keys by both the train pane and the per-preset recipe
// overrides. Both visitors below delegate here so the key/member pairs cannot drift apart.
constexpr auto recipe_scalar_fields = [](auto& state, const auto& visit) {
    visit("lr", state.lr);
    visit("lr_encoder", state.lr_encoder);
    visit("lr_component_decay", state.lr_component_decay);
    visit("encoder_layer_decay", state.encoder_layer_decay);
    visit("momentum", state.momentum);
    visit("weight_decay", state.weight_decay);
    visit("warmup_epochs", state.warmup_epochs);
    visit("warmup_momentum", state.warmup_momentum);
    visit("lr_min_factor", state.lr_min_factor);
};
constexpr auto recipe_override_fields = [](auto& overrides, const auto& visit) {
    using Relation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
    Relation::VisitMembers([&]<class Entry>() {
        bool explicit_override = Relation::template overridden<Entry::destination>(overrides);
        constexpr auto terminal = std::remove_cvref_t<decltype(Entry::destination)>::terminal_member;
        visit(mmltk::frameworks::reflection::materialized_member_name<terminal>().data(), explicit_override);
        if constexpr (!std::is_const_v<std::remove_reference_t<decltype(overrides)>>) {
            if (explicit_override)
                Relation::template set_override<Entry::destination>(overrides);
            else
                Relation::template clear_override<Entry::destination>(overrides);
        }
    });
};
constexpr auto train_training_fields = [](auto& state, const auto& visit) {
    auto& request = state.request;
    visit("output_dir", request.output_dir);
    visit("resume_path", request.resume_path);
    visit("batch_size", request.batch_size);
    visit("val_batch_size", request.val_batch_size);
    visit("epochs", request.epochs);
    visit("grad_accum_steps", request.grad_accum_steps);
    visit("num_queries", request.num_queries);
    visit("eval_max_dets", request.eval_max_dets);
    visit("lr_drop", request.lr_drop);
    visit("ema_tau", request.ema_tau);
    visit("print_freq", request.print_freq);
    visit("prefetch_factor", request.prefetch_factor);
    visit("seed", request.seed);
    recipe_scalar_fields(request, visit);
    visit("clip_max_norm", request.clip_max_norm);
    visit("ema_decay", request.ema_decay);
    visit("lr_scheduler", request.lr_scheduler);
    visit("use_ema", request.use_ema);
    visit("validation_loss", request.validation_loss);
    visit("validation_profile", request.validation_profile);
    visit("amp", request.amp);
    visit("freeze_encoder", request.freeze_encoder);
    visit("fused_optimizer", request.fused_optimizer);
    visit("optimizer", request.optimizer);
    visit("distributed_store_path", request.distributed_store_path);
    visit("distributed_rank", request.distributed_rank);
    visit("distributed_world_size", request.distributed_world_size);
    visit("distributed_worker", request.distributed_worker);
    visit("local_device_ids", request.device_ids);
    visit.nested("training_supervision", request.training_supervision, [](auto& supervision, const auto& nested) {
        nested("assignment", supervision.assignment);
        nested.nested("match_free", supervision.match_free, [](auto& match_free, const auto& field) {
            field("rho", match_free.rho);
            field("correspondence_weight", match_free.correspondence_weight);
            field("query_weight", match_free.query_weight);
        });
        nested.nested("denoising", supervision.denoising, [](auto& denoising, const auto& field) {
            field("enabled", denoising.enabled);
            field("groups", denoising.groups);
            field("label_noise_ratio", denoising.label_noise_ratio);
            field("center_noise_scale", denoising.center_noise_scale);
            field("size_noise_scale", denoising.size_noise_scale);
        });
    });
    visit.nested("recipe_overrides", request.recipe_overrides, recipe_override_fields);
};
constexpr auto augmentation_group_fields = [](auto& group, const auto& visit) {
    visit("probability", Hundredths{group.probability});
    visit("min_strength", Hundredths{group.min_strength});
    visit("max_strength", Hundredths{group.max_strength});
};
constexpr auto gpu_augmentation_fields = [](auto& config, const auto& visit) {
    visit("enabled", config.enabled);
    visit("perceptual_downscale", config.perceptual_downscale);
    visit.nested("geometry", config.geometry, augmentation_group_fields);
    visit.nested("resize", config.resize, augmentation_group_fields);
    visit.nested("color", config.color, augmentation_group_fields);
    visit.nested("noise", config.noise, augmentation_group_fields);
    visit.nested("blur", config.blur, augmentation_group_fields);
    visit.nested("occlusion", config.occlusion, augmentation_group_fields);
    visit("copy_paste_probability", Hundredths{config.copy_paste_probability});
};
constexpr auto model_artifact_fields = [](auto& state, const ModelArtifactsShape& shape, const auto& visit) {
    visit("preset_name", state.preset_name);
    visit("resolution", state.resolution);
    visit("source", state.source);
    visit("input", state.input);
    visit("class_layout_path", state.class_layout_path);
    if (shape.weights) { visit("weights_path", state.weights_path); }
    if (shape.onnx) { visit(shape.onnx_key, state.onnx_path); }
    if (shape.tensorrt) { visit("tensorrt_path", state.tensorrt_path); }
};
constexpr auto train_execution_fields = [](auto& request, const auto& visit) {
    visit("numa_nodes", request.numa_nodes);
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(request, visit);
    visit("cpu_affinity", request.cpu_affinity);
    visit("workers", request.workers);
    visit("lanes", request.lanes);
    visit("progress_bar", request.progress_bar);
    visit("compile_mode", request.compilation_mode);
};
constexpr auto validate_execution_fields = [](auto& request, const auto& visit) {
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(request, visit);
    visit("cpu_affinity", request.cpu_affinity);
    visit("device_id", request.device_id);
    visit("workers", request.workers);
    visit("allow_fp16", request.allow_fp16);
};
constexpr auto predict_execution_fields = [](auto& request, const auto& visit) {
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(request, visit);
    visit("cpu_affinity", request.cpu_affinity);
    visit("device_id", request.device_id);
    visit("workers", request.workers);
    visit("lanes", request.lanes);
    visit("allow_fp16", request.allow_fp16);
    visit("progress_bar", request.progress_bar);
    visit("compile_mode", request.compilation_mode);
};
constexpr auto annotate_execution_fields = [](auto& state, const auto& visit) {
    visit("device_id", state.device_id);
    visit("allow_fp16", state.allow_fp16);
    visit("compile_mode", state.compile_mode);
};
constexpr auto export_execution_fields = [](auto& state, const auto& visit) {
    visit("device_id", state.device_id);
    visit("allow_fp16", state.allow_fp16);
};
nlohmann::json snapshot_train_workflow_state(const TrainViewState& state) { return snapshot_fields(state, train_training_fields); }
nlohmann::json snapshot_gpu_augmentation(const mmltk::backend::models::rfdetr::GpuAugmentationConfig& config) {
    return snapshot_fields(config, gpu_augmentation_fields);
}
void apply_gpu_augmentation_json(const nlohmann::json& json, mmltk::backend::models::rfdetr::GpuAugmentationConfig& config) {
    gpu_augmentation_fields(config, JsonFieldReader{json});
}
nlohmann::json snapshot_model_artifacts(const ModelArtifactSelectionState& state, const ModelArtifactsShape& shape) {
    nlohmann::json json = nlohmann::json::object();
    model_artifact_fields(state, shape, JsonFieldWriter{json});
    return json;
}
template <ModelArtifactSelectionView State>
void add_model_selection_json(nlohmann::json& json, const State& state) {
    ModelArtifactSelectionState selection = model_artifacts(state);
    JsonFieldWriter write{json};
    write("preset_name", selection.preset_name);
    write("model_resolution", selection.resolution);
    write("model_source", selection.source);
    write("model_input", selection.input);
}
template <ModelArtifactSelectionView State>
void apply_model_selection_json(const nlohmann::json& json, State& state) {
    ModelArtifactSelectionState selection = model_artifacts(state);
    JsonFieldReader read{json};
    read("preset_name", selection.preset_name);
    read("model_resolution", selection.resolution);
    read("model_source", selection.source);
    read("model_input", selection.input);
    apply_model_artifacts(state, selection);
}
void apply_model_artifacts_json(const nlohmann::json& json, ModelArtifactSelectionState& state, const ModelArtifactsShape& shape) {
    model_artifact_fields(state, shape, JsonFieldReader{json});
    normalize_model_artifacts(state, shape);
}
// Fields persisted only in the flat legacy layout of to_json/from_json(TrainViewState).
constexpr auto train_flat_fields = [](auto& state, auto& artifact_state, const auto& visit) {
    visit("train_compiled_path", state.request.train_compiled_path);
    visit("val_compiled_path", state.request.val_compiled_path);
    visit("test_compiled_path", state.request.test_compiled_path);
    visit("compile_benchmark_dataset_override", state.compile_benchmark_dataset_override);
    visit("weights_path", artifact_state.weights_path);
    visit("class_layout_path", artifact_state.class_layout_path);
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(state.request, visit);
    visit("cpu_affinity", state.request.cpu_affinity);
    visit("workers", state.request.workers);
    visit("lanes", state.request.lanes);
    visit("progress_bar", state.request.progress_bar);
    visit("compile_mode", state.request.compilation_mode);
};
// The "validation" workflow section; also reused inside the flat ValidateViewState layout.
constexpr auto validation_fields = [](auto& state, const auto& visit) {
    visit("save_engine_path", state.save_engine_path);
    visit("report_json_path", state.report_json_path);
    visit("split", state.split);
    visit("eval_order", state.eval_order);
    visit("resolution", state.resolution);
    visit("limit_images", state.limit_images);
    visit("alignment_images", state.alignment_images);
    visit("num_queries", state.num_queries);
    visit("eval_max_dets", state.eval_max_dets);
    visit("batch_size", state.batch_size);
    visit("prefetch_factor", state.prefetch_factor);
    visit("recompile", state.recompile);
    visit("profile", state.profile);
    visit("write_report_json", state.write_report_json);
    visit("compile_workers", state.compile_workers);
    visit("compile_cuda_mask_batch_size", state.compile_cuda_mask_batch_size);
    visit("compile_cuda_device_id", state.compile_cuda_device_id);
    visit("log_mode", state.log_mode);
};
constexpr auto validate_flat_fields = [](auto& state, const auto& visit) {
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(state, visit);
    visit("compiled_path", state.compiled_path);
    visit("source_dir", state.source_dir);
    visit("weights_path", state.weights_path);
    if constexpr (requires { state.class_layout_path; }) visit("class_layout_path", state.class_layout_path);
    visit("onnx_path", state.onnx_path);
    visit("tensorrt_path", state.tensorrt_path);
    visit("cpu_affinity", state.cpu_affinity);
    visit("device_id", state.device_id);
    visit("workers", state.workers);
    visit("allow_fp16", state.allow_fp16);
    validation_fields(state, visit);
};
constexpr auto predict_fields = [](auto& state, const auto& visit) {
    visit("output_path", state.output_path);
    visit("backend", state.backend);
    visit("batch_size", state.batch_size);
    visit("max_dets_per_image", state.max_dets_per_image);
    visit("threshold", state.threshold);
};
constexpr auto predict_flat_fields = [](auto& state, const auto& visit) {
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(state, visit);
    visit("weights_path", state.weights_path);
    if constexpr (requires { state.class_layout_path; }) visit("class_layout_path", state.class_layout_path);
    visit("onnx_path", state.onnx_path);
    visit("tensorrt_path", state.tensorrt_path);
    visit("cpu_affinity", state.cpu_affinity);
    visit("device_id", state.device_id);
    visit("workers", state.workers);
    visit("lanes", state.lanes);
    visit("allow_fp16", state.allow_fp16);
    visit("progress_bar", state.progress_bar);
    visit("compile_mode", state.compilation_mode);
    predict_fields(state, visit);
};
constexpr auto annotate_fields = [](auto& state, const auto& visit) {
    visit("output_dir", state.output_dir);
    visit("split", state.split);
    visit("backend", state.backend);
    visit("max_dets_per_image", state.max_dets_per_image);
    visit("threshold", state.threshold);
    visit("full_frame", state.full_frame);
};
constexpr auto annotate_flat_fields = [](auto& state, const auto& visit) {
    visit("source", state.source);
    visit("weights_path", state.weights_path);
    if constexpr (requires { state.class_layout_path; }) visit("class_layout_path", state.class_layout_path);
    visit("onnx_path", state.onnx_path);
    visit("tensorrt_path", state.tensorrt_path);
    visit("device_id", state.device_id);
    visit("allow_fp16", state.allow_fp16);
    visit("compile_mode", state.compile_mode);
    annotate_fields(state, visit);
};
constexpr auto export_fields = [](auto& state, const auto& visit) {
    visit("onnx_output_path", state.onnx_output_path);
    visit("output_path", state.output_path);
    visit("opset_version", state.opset_version);
    visit("build_tensorrt", state.build_tensorrt);
    visit("simplify", state.simplify);
};
constexpr auto export_flat_fields = [](auto& state, const auto& visit) {
    visit("weights_path", state.weights_path);
    if constexpr (requires { state.class_layout_path; }) visit("class_layout_path", state.class_layout_path);
    visit("onnx_input_path", state.onnx_input_path);
    visit("device_id", state.device_id);
    visit("allow_fp16", state.allow_fp16);
    export_fields(state, visit);
};
constexpr auto ui_settings_fields = [](auto& state, const auto& visit) { visit_record_fields<UiSettingsState>(state, visit); };
constexpr auto explore_fields = [](auto& state, const auto& visit) {
    visit_record_fields<mmltk::backend::data::DataLoadingOptions>(state, visit);
    visit("dataset_source", state.dataset_source);
    visit("custom_compiled_path", state.custom_compiled_path);
    visit("device_id", state.device_id);
    visit("grid_width", state.grid_width);
    visit("order", state.order);
    visit("shuffle_seed", state.shuffle_seed);
    visit("require_boxes", state.require_boxes);
    visit("require_masks", state.require_masks);
    visit("min_instances", state.min_instances);
    visit("max_instances", state.max_instances);
    visit("min_compiled_index", state.min_compiled_index);
    visit("max_compiled_index", state.max_compiled_index);
    visit("class_catalog_identity", state.class_catalog_identity);
    visit("sample_classes", state.sample_classes);
    visit("overlay_classes", state.overlay_classes);
    visit("show_boxes", state.show_boxes);
    visit("show_masks", state.show_masks);
    visit("show_original_dimensions", state.show_original_dimensions);
    visit("detail_scale_mode", state.detail_scale_mode);
};
template <typename State, typename Execution, typename ExecutionFields>
[[nodiscard]] nlohmann::json snapshot_workflow_artifacts_and_execution(const State& s, const ModelArtifactsShape& artifacts_shape, const Execution& execution,
                                                                       const ExecutionFields& execution_fields) {
    return nlohmann::json{
        {kModelArtifactsKey, snapshot_model_artifacts(model_artifacts(s), artifacts_shape)},
        {kExecutionKey, snapshot_fields(execution, execution_fields)},
    };
}
template <typename State, typename Execution, typename ExecutionFields>
void apply_workflow_artifacts_and_execution(const nlohmann::json& workflow, State& s, const ModelArtifactsShape& artifacts_shape, Execution& execution,
                                            const ExecutionFields& execution_fields) {
    ModelArtifactSelectionState artifact_state = model_artifacts(s);
    if (const nlohmann::json* artifacts = find_object(workflow, kModelArtifactsKey)) {
        apply_model_artifacts_json(*artifacts, artifact_state, artifacts_shape);
    }
    if (const nlohmann::json* execution_json = find_object(workflow, kExecutionKey)) { execution_fields(execution, JsonFieldReader{*execution_json}); }
    apply_model_artifacts(s, artifact_state);
}
// Reads one flat field section out of a workflow object when the section is present.
template <typename State, typename FieldList>
void apply_workflow_section(const nlohmann::json& workflow, State& s, const char* section_key, const FieldList& fields) {
    if (const nlohmann::json* section = find_object(workflow, section_key)) { fields(s, JsonFieldReader{*section}); }
}
// Workflows that persist only the shared artifacts/execution blocks plus one flat field section.
template <typename State, typename Execution, typename ExecutionFields, typename FieldList>
void apply_section_workflow(const nlohmann::json& workflow, State& s, const ModelArtifactsShape& artifacts_shape, Execution& execution,
                            const ExecutionFields& execution_fields, const char* section_key, const FieldList& fields) {
    apply_workflow_artifacts_and_execution(workflow, s, artifacts_shape, execution, execution_fields);
    apply_workflow_section(workflow, s, section_key, fields);
}
// Predict and annotate persist the same layout: a source block, the shared artifacts/execution
// blocks, and one flat field section. Only the shapes, the section key and the field list differ.
template <typename State, typename Execution, typename ExecutionFields, typename FieldList>
void apply_source_workflow(const nlohmann::json& workflow, State& s, const ModelArtifactsShape& artifacts_shape, Execution& execution,
                           const ExecutionFields& execution_fields, const char* section_key, const FieldList& fields) {
    if (const nlohmann::json* source = find_object(workflow, "source")) { source_fields(s.source, JsonFieldReader{*source}); }
    apply_section_workflow(workflow, s, artifacts_shape, execution, execution_fields, section_key, fields);
}
// Every workflow branch below is the same lookup: skip the workflow when the caller passed no state,
// then skip it again when the document has no object for it. Only the body differs, so the two
// guards are written once here.
template <typename State, typename ApplyFn>
void apply_workflow(const nlohmann::json& workflows_json, State* state, const char* workflow_key, ApplyFn&& apply) {
    if (state == nullptr) { return; }
    if (const nlohmann::json* workflow = find_object(workflows_json, workflow_key)) { apply(*workflow, *state); }
}
// Train and validate persist only projections of their canonical request fields.
template <typename State, typename Dataset, typename DatasetFields, typename Execution, typename ExecutionFields>
void apply_dataset_workflow(const nlohmann::json& workflow, State& s, const ModelArtifactsShape& artifacts_shape, Dataset& dataset,
                            const DatasetFields& dataset_fields, Execution& execution, const ExecutionFields& execution_fields) {
    if (const nlohmann::json* datasets = find_object(workflow, kDatasetPathsKey)) { dataset_fields(dataset, JsonFieldReader{*datasets}); }
    apply_workflow_artifacts_and_execution(workflow, s, artifacts_shape, execution, execution_fields);
}
nlohmann::json normalize_gui_settings_document_impl(const nlohmann::json& j) {
    if (!j.is_object()) { throw std::runtime_error("GUI settings must be a JSON object"); }
    const auto schema_it = j.find("schema_version");
    if (schema_it == j.end()) { throw std::runtime_error("GUI settings schema_version is missing"); }
    if (!schema_it->is_number_integer() && !schema_it->is_number_unsigned()) { throw std::runtime_error("GUI settings schema_version must be an integer"); }
    if (mmltk::frameworks::serialization::decode_json_integer_exact<std::uint32_t>(*schema_it) != kGuiSettingsSchemaVersion) {
        throw std::runtime_error("unsupported GUI settings schema_version");
    }
    return j;
}
}  // namespace
namespace settings_json_detail {
void convert(JsonWrite<SourceSelectionState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s, source_fields);
}
void convert(JsonRead<SourceSelectionState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    source_fields(s, JsonFieldReader{j});
}
void convert(JsonWrite<TrainViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    const ModelArtifactSelectionState artifact_state = model_artifacts(s);
    j = snapshot_train_workflow_state(s);
    const JsonFieldWriter write{j};
    train_flat_fields(s, artifact_state, write);
    write.nested("gpu_augmentation", s.request.gpu_augmentation, gpu_augmentation_fields);
    write("visualize_augmentation_in_explore", s.visualize_augmentation_in_explore);
    train_execution_target_fields(s, write);
    add_model_selection_json(j, s);
}
void convert(JsonRead<TrainViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    ModelArtifactSelectionState artifact_state = model_artifacts(s);
    const JsonFieldReader read{j};
    train_training_fields(s, read);
    train_flat_fields(s, artifact_state, read);
    train_execution_target_fields(s, read);
    read.nested("gpu_augmentation", s.request.gpu_augmentation, gpu_augmentation_fields);
    read("visualize_augmentation_in_explore", s.visualize_augmentation_in_explore);
    apply_model_artifacts(s, artifact_state);
    apply_model_selection_json(j, s);
}
void convert(JsonWrite<AnnotateViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s, annotate_flat_fields);
    add_model_selection_json(j, s);
}
void convert(JsonRead<AnnotateViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    annotate_flat_fields(s, JsonFieldReader{j});
    apply_model_selection_json(j, s);
}
void convert(JsonWrite<ExportViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s, export_flat_fields);
    add_model_selection_json(j, s);
}
void convert(JsonRead<ExportViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    export_flat_fields(s, JsonFieldReader{j});
    apply_model_selection_json(j, s);
}
void convert(JsonWrite<ValidateViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s.request, validate_flat_fields);
    add_model_selection_json(j, s);
}
void convert(JsonRead<ValidateViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    validate_flat_fields(s.request, JsonFieldReader{j});
    apply_model_selection_json(j, s);
}
void convert(JsonWrite<PredictViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s.request, predict_flat_fields);
    j["source"] = s.source;
    j["live_split_count"] = s.live_split_count;
    add_model_selection_json(j, s);
}
void convert(JsonRead<PredictViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    predict_flat_fields(s.request, JsonFieldReader{j});
    s.request.batch_size = 1U;
    get_optional(j, "source", s.source);
    get_optional(j, "live_split_count", s.live_split_count);
    apply_model_selection_json(j, s);
}
void convert(JsonWrite<UiSettingsState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s, ui_settings_fields);
}
void convert(JsonRead<UiSettingsState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    ui_settings_fields(s, JsonFieldReader{j});
    mmltk::frameworks::reflection::normalize_reflected_intrinsics(s, UiSettingsState{});
}
void convert(JsonWrite<ExploreViewState> value) {
    auto& j = value.json;
    const auto& s = value.state;
    j = snapshot_fields(s, explore_fields);
}
void convert(JsonRead<ExploreViewState> value) {
    const auto& j = value.json;
    auto& s = value.state;
    explore_fields(s, JsonFieldReader{j});
    mmltk::frameworks::reflection::normalize_reflected_intrinsics(s, ExploreViewState{});
    s.max_instances = std::max(s.max_instances, s.min_instances);
    if (s.max_compiled_index < s.min_compiled_index) { s.max_compiled_index = s.min_compiled_index; }
}
}  // namespace settings_json_detail
nlohmann::json snapshot_workflows(const GuiSettingsState& settings) {
    nlohmann::json j = nlohmann::json::object();
    {
        const TrainViewState& s = settings.workflows.train;
        nlohmann::json train_json = snapshot_workflow_artifacts_and_execution(s, train_model_artifacts_shape(), s.request, train_execution_fields);
        train_json[kDatasetPathsKey] = snapshot_fields(s, train_dataset_fields);
        nlohmann::json training_json = snapshot_train_workflow_state(s);
        training_json.update(snapshot_fields(s, train_execution_target_fields));
        train_json[kTrainingKey] = std::move(training_json);
        train_json[kAugmentationKey] = snapshot_gpu_augmentation(s.request.gpu_augmentation);
        train_json[kAugmentationKey]["visualize_in_explore"] = s.visualize_augmentation_in_explore;
        j["train"] = std::move(train_json);
    }
    {
        const ValidateViewState& s = settings.workflows.validate;
        nlohmann::json validate_json = snapshot_workflow_artifacts_and_execution(s, validate_model_artifacts_shape(), s.request, validate_execution_fields);
        validate_json[kDatasetPathsKey] = snapshot_fields(s.request, validate_dataset_fields);
        validate_json[kValidationKey] = snapshot_fields(s.request, validation_fields);
        j["validate"] = std::move(validate_json);
    }
    {
        const PredictViewState& s = settings.workflows.predict;
        nlohmann::json predict_json = snapshot_workflow_artifacts_and_execution(s, ModelArtifactsShape{}, s.request, predict_execution_fields);
        predict_json["source"] = s.source;
        predict_json[kPredictKey] = snapshot_fields(s.request, predict_fields);
        predict_json[kPredictKey]["live_split_count"] = s.live_split_count;
        j["predict"] = std::move(predict_json);
    }
    {
        const AnnotateViewState& s = settings.workflows.annotate;
        nlohmann::json annotate_json = snapshot_workflow_artifacts_and_execution(s, ModelArtifactsShape{}, s, annotate_execution_fields);
        annotate_json["source"] = s.source;
        annotate_json[kAnnotateKey] = snapshot_fields(s, annotate_fields);
        j["annotate"] = std::move(annotate_json);
    }
    {
        const ExportViewState& s = settings.workflows.export_state;
        nlohmann::json export_json = snapshot_workflow_artifacts_and_execution(s, export_model_artifacts_shape(), s, export_execution_fields);
        export_json[kExportKey] = snapshot_fields(s, export_fields);
        j["export"] = std::move(export_json);
    }
    j[kExploreKey] = settings.workflows.explore;
    return j;
}
void apply_workflows(const nlohmann::json& j, GuiSettingsState& settings) {
    const nlohmann::json* workflows_json = find_object(j, "workflows");
    if (workflows_json == nullptr) { return; }
    apply_workflow(*workflows_json, &settings.workflows.train, "train", [](const nlohmann::json& train, TrainViewState& s) {
        apply_dataset_workflow(train, s, train_model_artifacts_shape(), s, train_dataset_fields, s.request, train_execution_fields);
        if (const nlohmann::json* training = find_object(train, kTrainingKey)) {
            const JsonFieldReader read{*training};
            train_training_fields(s, read);
            train_execution_target_fields(s, read);
        }
        if (const nlohmann::json* augmentation = find_object(train, kAugmentationKey)) {
            apply_gpu_augmentation_json(*augmentation, s.request.gpu_augmentation);
            get_optional(*augmentation, "visualize_in_explore", s.visualize_augmentation_in_explore);
        }
    });
    apply_workflow(*workflows_json, &settings.workflows.validate, "validate", [](const nlohmann::json& validate, ValidateViewState& s) {
        apply_dataset_workflow(validate, s, validate_model_artifacts_shape(), s.request, validate_dataset_fields, s.request, validate_execution_fields);
        apply_workflow_section(validate, s.request, kValidationKey, validation_fields);
    });
    apply_workflow(*workflows_json, &settings.workflows.predict, "predict", [](const nlohmann::json& predict, PredictViewState& s) {
        if (const nlohmann::json* source = find_object(predict, "source")) { source_fields(s.source, JsonFieldReader{*source}); }
        apply_workflow_artifacts_and_execution(predict, s, ModelArtifactsShape{}, s.request, predict_execution_fields);
        apply_workflow_section(predict, s.request, kPredictKey, predict_fields);
        if (const nlohmann::json* values = find_object(predict, kPredictKey)) { get_optional(*values, "live_split_count", s.live_split_count); }
    });
    apply_workflow(*workflows_json, &settings.workflows.annotate, "annotate", [](const nlohmann::json& annotate, AnnotateViewState& s) {
        apply_source_workflow(annotate, s, ModelArtifactsShape{}, s, annotate_execution_fields, kAnnotateKey, annotate_fields);
    });
    apply_workflow(*workflows_json, &settings.workflows.export_state, "export", [](const nlohmann::json& export_json, ExportViewState& s) {
        apply_section_workflow(export_json, s, export_model_artifacts_shape(), s, export_execution_fields, kExportKey, export_fields);
    });
    apply_workflow(*workflows_json, &settings.workflows.explore, kExploreKey, [](const nlohmann::json& explore, ExploreViewState& s) { explore.get_to(s); });
}
nlohmann::json normalize_gui_settings_document(const nlohmann::json& j) { return normalize_gui_settings_document_impl(j); }
nlohmann::json default_gui_settings_document() { return normalize_gui_settings_document_impl(snapshot_gui_settings(default_gui_settings_state())); }
GuiSettingsState load_initial_gui_settings_state(const std::string& path) {
    GuiSettingsState states = default_gui_settings_state();
    static_cast<void>(load_gui_settings_file(path, states));
    return states;
}
nlohmann::json snapshot_gui_settings(const GuiSettingsState& state) {
    nlohmann::json j;
    j["schema_version"] = kGuiSettingsSchemaVersion;
    j["current_view"] = static_cast<int>(state.current_view);
    j["ui"] = state.ui;
    const nlohmann::json workflows = snapshot_workflows(state);
    if (!workflows.empty()) j["workflows"] = workflows;
    return j;
}
void apply_gui_settings(const nlohmann::json& j, GuiSettingsState& state) {
    const nlohmann::json normalized = normalize_gui_settings_document(j);
    GuiSettingsState candidate = state;
    if (normalized.contains("current_view")) {
        const int value = mmltk::frameworks::serialization::decode_json_integer_exact<int>(normalized.at("current_view"));
        candidate.current_view = value >= static_cast<int>(mmltk::controller::contracts::FeatureId::Train) &&
                                         value <= static_cast<int>(mmltk::controller::contracts::FeatureId::Explore)
                                     ? static_cast<mmltk::controller::contracts::FeatureId>(value)
                                     : mmltk::controller::contracts::FeatureId::Train;
    }
    if (normalized.contains("ui")) normalized.at("ui").get_to(candidate.ui);
    apply_workflows(normalized, candidate);
    candidate.workflows.predict.request.batch_size = 1U;
    const auto normalize_start_selection = [](auto& workflow) {
        if (workflow.model_input == ModelArtifactInputKind::None && workflow.model_source == ModelSelectionSource::Canonical)
            workflow.model_input = ModelArtifactInputKind::Weights;
    };
    normalize_start_selection(candidate.workflows.train);
    normalize_start_selection(candidate.workflows.validate);
    normalize_start_selection(candidate.workflows.predict);
    if (!gui_settings_valid(candidate) ||
        !mmltk::backend::models::rfdetr::training_supervision_config_valid(candidate.workflows.train.request.training_supervision)) {
        throw std::runtime_error("GUI settings violate typed field or cross-field constraints");
    }
    state = std::move(candidate);
}
namespace settings_json_detail {
void convert(JsonWrite<GuiSettingsState> value) {
    auto& json = value.json;
    const auto& state = value.state;
    json = snapshot_gui_settings(state);
}
void convert(JsonRead<GuiSettingsState> value) {
    const auto& json = value.json;
    auto& state = value.state;
    apply_gui_settings(json, state);
}
}  // namespace settings_json_detail
bool load_gui_settings_file(const std::string& path, GuiSettingsState& state, nlohmann::json* const normalized_document, bool* const repair_required) {
    std::ifstream file(path);
    if (!file.is_open()) { return false; }
    try {
        const nlohmann::json raw = nlohmann::json::parse(file);
        const nlohmann::json normalized = normalize_gui_settings_document(raw);
        GuiSettingsState candidate = state;
        apply_gui_settings(normalized, candidate);
        bool repaired = normalized != raw;
        const auto repair_selection = [&repaired](auto& selection) {
            ModelArtifactSelectionState artifacts = model_artifacts(selection);
            if (mmltk::backend::models::rfdetr::find_preset_catalog_entry(artifacts.preset_name) != nullptr) { return; }
            const auto* fallback = mmltk::backend::models::rfdetr::find_preset_catalog_entry(kDefaultModelPresetName);
            if (fallback == nullptr) { throw std::runtime_error("default RF-DETR preset is unavailable"); }
            artifacts.preset_name = fallback->preset_name;
            artifacts.resolution = static_cast<int>(fallback->resolution);
            artifacts.source = ModelSelectionSource::Canonical;
            artifacts.input = ModelArtifactInputKind::None;
            artifacts.weights_path.clear();
            artifacts.onnx_path.clear();
            artifacts.tensorrt_path.clear();
            apply_model_artifacts(selection, artifacts);
            repaired = true;
        };
        repair_selection(candidate.workflows.train);
        repair_selection(candidate.workflows.validate);
        repair_selection(candidate.workflows.predict);
        repair_selection(candidate.workflows.annotate);
        repair_selection(candidate.workflows.export_state);
        if (candidate.workflows.train.use_compiled_directory_defaults) {
            const std::filesystem::path directory{candidate.workflows.train.compiled_dataset_dir};
            const bool defaults_match = candidate.workflows.train.request.train_compiled_path == directory / "train.bin" &&
                                        candidate.workflows.train.request.val_compiled_path == directory / "val.bin";
            if (!defaults_match) {
                candidate.workflows.train.use_compiled_directory_defaults = false;
                repaired = true;
            }
        }
        if (!gui_settings_valid(candidate)) { throw std::runtime_error("GUI settings violate typed field or cross-field constraints"); }
        repaired = repaired || snapshot_gui_settings(candidate) != normalized;
        state = std::move(candidate);
        if (normalized_document != nullptr) { *normalized_document = normalized; }
        if (repair_required != nullptr) { *repair_required = repaired; }
        return true;
    } catch (const std::exception& e) {
        mmltk::common::logging::warn([&](auto& logger) { logger.warn("[gui] ignoring malformed settings from {}: {}", path, e.what()); });
        return false;
    }
}
}  // namespace mmltk::controller::contracts
