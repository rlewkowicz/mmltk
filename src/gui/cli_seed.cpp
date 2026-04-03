#include "cli_seed.h"

#include "default_state.h"
#include "gui_settings.h"
#include "rfdetr_workflows.h"
#include "view_state.h"

#include "CLI11.hpp"

#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/validate.h"
#include "mmltk/rfdetr/workflow_requests.h"
#include "mmltk_logging.h"
#include "rfdetr/cli/workflow_cli_shared.h"
#include "rfdetr/train_recipe.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::gui {

namespace {

namespace fs = std::filesystem;

using PredictSeedCliState = mmltk::rfdetr::cli_shared::PredictCommandSpec;
using ValidateSeedCliState = mmltk::rfdetr::cli_shared::ValidateCommandSpec;
using TrainSeedCliState = mmltk::rfdetr::cli_shared::TrainCommandSpec;

struct SeedSnapshotState {
    View current_view = View::Train;
    std::string selected_preset;
    UiSettingsState ui;
    TrainViewState train;
    ValidateViewState validate;
    PredictViewState predict;
    AnnotateViewState annotate;
    ExportViewState export_state;
};

WorkflowSettingsSnapshot bind_workflows(SeedSnapshotState& state) {
    return WorkflowSettingsSnapshot{
        &state.train,
        &state.validate,
        &state.predict,
        &state.annotate,
        &state.export_state,
    };
}

GuiSettingsSnapshot bind_snapshot(SeedSnapshotState& state) {
    return GuiSettingsSnapshot{
        state.current_view,
        state.selected_preset,
        &state.ui,
        bind_workflows(state),
    };
}

int compile_mode_index_from_cli(const std::string& value) {
    if (value == "none") {
        return 0;
    }
    if (value == "selective") {
        return 1;
    }
    if (value == "full") {
        return 2;
    }
    throw std::runtime_error("unsupported compilation mode: " + value);
}

TrainOptimizerMode train_optimizer_mode_from_kind(const mmltk::rfdetr::TrainOptimizerKind kind) {
    switch (kind) {
    case mmltk::rfdetr::TrainOptimizerKind::AdamW:
        return TrainOptimizerMode::AdamW;
    case mmltk::rfdetr::TrainOptimizerKind::Muon:
        return TrainOptimizerMode::Muon;
    }
    return TrainOptimizerMode::AdamW;
}


template <typename AppFactory>
void parse_cli_app(CLI::App& app,
                   const std::string& app_name,
                   const std::vector<std::string>& args,
                   AppFactory&&) {
    std::vector<std::string> command_args;
    command_args.reserve(args.size() + 1U);
    command_args.push_back(app_name);
    command_args.insert(command_args.end(), args.begin(), args.end());

    std::vector<const char*> raw_args;
    raw_args.reserve(command_args.size());
    for (const std::string& arg : command_args) {
        raw_args.push_back(arg.c_str());
    }

    try {
        app.parse(static_cast<int>(raw_args.size()), raw_args.data());
    } catch (const CLI::ParseError& error) {
        throw std::runtime_error("invalid GUI seed arguments for `" + app_name + "`: " + error.what());
    }
}

std::string infer_preset_name_from_path(const fs::path& input_path) {
    if (input_path.empty()) {
        return {};
    }
    if (const auto* preset = mmltk::rfdetr::infer_model_preset_from_path(input_path)) {
        return std::string(preset->preset_name);
    }
    return {};
}

std::string choose_seed_preset(const std::string& current_preset,
                               const std::initializer_list<fs::path>& candidates) {
    for (const fs::path& candidate : candidates) {
        const std::string inferred = infer_preset_name_from_path(candidate);
        if (!inferred.empty()) {
            return inferred;
        }
    }
    return current_preset.empty() ? std::string(kDefaultGuiPresetName) : current_preset;
}

bool has_current_schema_version(const nlohmann::json& j) {
    if (!j.is_object()) {
        return false;
    }

    const auto schema_it = j.find("schema_version");
    if (schema_it == j.end()) {
        return false;
    }
    if (!schema_it->is_number_integer() && !schema_it->is_number_unsigned()) {
        return false;
    }

    try {
        return schema_it->get<std::uint32_t>() == kGuiSettingsSchemaVersion;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

ModelInputMode detect_model_input_mode(const mmltk::rfdetr::ModelArtifactRequest& request) {
    if (!request.weights_path.empty()) {
        return ModelInputMode::Weights;
    }
    if (!request.onnx_path.empty()) {
        return ModelInputMode::Onnx;
    }
    if (!request.tensorrt_path.empty()) {
        return ModelInputMode::TensorRt;
    }
    throw std::runtime_error("missing model input artifact");
}

SeedSnapshotState load_seed_snapshot(const fs::path& settings_path) {
    SeedSnapshotState state;
    apply_default_gui_state(state.selected_preset,
                            state.train,
                            state.validate,
                            state.predict,
                            state.annotate,
                            state.export_state);

    std::ifstream file(settings_path);
    if (!file.is_open()) {
        return state;
    }

    try {
        const nlohmann::json j = nlohmann::json::parse(file);
        if (!has_current_schema_version(j)) {
            mmltk::logging::logger("gui")->warn(
                "[gui] ignoring settings from {} because schema_version is missing or unsupported",
                settings_path.string());
            return state;
        }
        GuiSettingsSnapshot snapshot = bind_snapshot(state);
        apply_gui_settings(j, snapshot);
        state.current_view = snapshot.current_view;
        state.selected_preset = snapshot.selected_preset;
        return state;
    } catch (const nlohmann::json::exception& error) {
        mmltk::logging::logger("gui")->warn("[gui] ignoring malformed settings from {}: {}",
                                             settings_path.string(),
                                             error.what());
        return state;
    }
}

void save_seed_snapshot(const fs::path& settings_path, SeedSnapshotState& state) {
    std::error_code error;
    const fs::path parent = settings_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            throw std::runtime_error(
                "failed to create settings directory `" + parent.string() + "`: " + error.message());
        }
    }

    const nlohmann::json j = snapshot_gui_settings(bind_snapshot(state));
    const fs::path tmp_path = settings_path.string() + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            throw std::runtime_error("failed to write GUI settings to `" + tmp_path.string() + "`");
        }
        file << j.dump(2) << '\n';
    }
    fs::rename(tmp_path, settings_path, error);
    if (error) {
        std::error_code cleanup_error;
        fs::remove(tmp_path, cleanup_error);
        throw std::runtime_error(
            "failed to rename `" + tmp_path.string() + "` to `" + settings_path.string() + "`: " + error.message());
    }
}

PredictSeedCliState parse_predict_seed(const std::vector<std::string>& args) {
    CLI::App app{"Seed Predict view from mmltk rfdetr predict"};
    app.option_defaults()->always_capture_default();
    PredictSeedCliState state = mmltk::rfdetr::cli_shared::add_predict_command_options(&app);
    parse_cli_app(app, "mmltk rfdetr predict", args, []() {});
    mmltk::rfdetr::cli_shared::finalize_predict_command(state);
    return state;
}

ValidateSeedCliState parse_validate_seed(const std::vector<std::string>& args) {
    CLI::App app{"Seed Validate view from mmltk rfdetr validate"};
    app.option_defaults()->always_capture_default();
    ValidateSeedCliState state = mmltk::rfdetr::cli_shared::add_validate_command_options(&app, true);
    parse_cli_app(app, "mmltk rfdetr validate", args, []() {});
    mmltk::rfdetr::cli_shared::finalize_validate_command(
        state,
        mmltk::rfdetr::cli_shared::ValidateUnsupportedOptionMode::RejectGuiUnsupported);
    return state;
}

TrainSeedCliState parse_train_seed(const std::vector<std::string>& args, const std::string& fallback_preset_name) {
    CLI::App app{"Seed Train view from mmltk rfdetr train"};
    app.option_defaults()->always_capture_default();
    TrainSeedCliState state = mmltk::rfdetr::cli_shared::add_train_command_options(&app, true);
    parse_cli_app(app, "mmltk rfdetr train", args, []() {});
    mmltk::rfdetr::cli_shared::finalize_train_command(
        state,
        fallback_preset_name,
        mmltk::rfdetr::cli_shared::TrainUnsupportedOptionMode::RejectGuiUnsupported);
    return state;
}

mmltk::rfdetr::BuildEngineRequest parse_build_engine_seed(const std::vector<std::string>& args) {
    mmltk::rfdetr::BuildEngineRequest request;

    CLI::App app{"Seed Export view from mmltk rfdetr build-engine"};
    app.option_defaults()->always_capture_default();
    mmltk::rfdetr::cli_shared::add_build_engine_request_options(&app, &app, request);
    parse_cli_app(app, "mmltk rfdetr build-engine", args, []() {});

    mmltk::rfdetr::validate_build_engine_request(request);
    return request;
}

mmltk::rfdetr::ExportOnnxRequest parse_export_onnx_seed(const std::vector<std::string>& args) {
    mmltk::rfdetr::ExportOnnxRequest request;

    CLI::App app{"Seed Export view from mmltk rfdetr export-onnx"};
    app.option_defaults()->always_capture_default();
    mmltk::rfdetr::cli_shared::add_export_onnx_request_options(&app, &app, request);
    parse_cli_app(app, "mmltk rfdetr export-onnx", args, []() {});

    mmltk::rfdetr::validate_export_onnx_request(request);
    return request;
}

std::vector<std::string> normalize_seed_cli_args(const std::vector<std::string>& cli_args) {
    std::vector<std::string> normalized = cli_args;
    if (!normalized.empty() && (normalized.front() == "mmltk" || normalized.front() == "mmltk-gui")) {
        normalized.erase(normalized.begin());
    }
    return normalized;
}

void apply_predict_seed(SeedSnapshotState& snapshot, const PredictSeedCliState& state) {
    snapshot.current_view = View::Predict;
    snapshot.selected_preset = choose_seed_preset(
        snapshot.selected_preset,
        {state.request.weights_path, state.request.onnx_path, state.request.tensorrt_path});

    PredictViewState& predict = snapshot.predict;
    predict.source = {};
    predict.source.kind = SourceKind::CompiledDataset;
    predict.source.compiled_path = state.request.compiled_path.string();
    predict.weights_path.clear();
    predict.onnx_path.clear();
    predict.tensorrt_path.clear();
    predict.model_input = detect_model_input_mode(state.request);
    switch (predict.model_input) {
    case ModelInputMode::Weights:
        predict.weights_path = state.request.weights_path.string();
        break;
    case ModelInputMode::Onnx:
        predict.onnx_path = state.request.onnx_path.string();
        break;
    case ModelInputMode::TensorRt:
        predict.tensorrt_path = state.request.tensorrt_path.string();
        break;
    case ModelInputMode::None:
        break;
    }
    predict.output_path = state.request.output_path.string();
    predict.backend = state.request.backend;
    predict.cpu_affinity = state.request.cpu_affinity;
    predict.batch_size = static_cast<int>(state.request.batch_size);
    predict.max_dets_per_image = static_cast<int>(state.request.max_dets_per_image);
    predict.device_id = state.request.device_id;
    predict.workers = state.request.workers;
    predict.lanes = state.request.lanes;
    predict.threshold = state.request.threshold;
    predict.allow_fp16 = state.request.allow_fp16;
    predict.progress_bar = state.request.progress_bar;
    predict.compile_mode = compile_mode_index_from_cli(state.compile_mode);
}

void apply_validate_seed(SeedSnapshotState& snapshot, const ValidateSeedCliState& state) {
    snapshot.current_view = View::Validate;
    snapshot.selected_preset = choose_seed_preset(
        snapshot.selected_preset,
        {state.request.onnx_path, state.request.tensorrt_path});

    ValidateViewState& validate = snapshot.validate;
    validate.compiled_path = state.request.compiled_path.string();
    validate.source_dir = state.request.source_dir.string();
    validate.onnx_path = state.request.onnx_path.string();
    validate.tensorrt_path = state.request.tensorrt_path.string();
    validate.save_engine_path = state.request.save_engine_path.string();
    validate.report_json_path = state.request.report_json_path.string();
    validate.split = state.request.split;
    validate.eval_order = state.request.eval_order;
    validate.resolution = static_cast<int>(state.request.resolution);
    validate.limit_images = static_cast<int>(state.request.limit_images);
    validate.alignment_images = static_cast<int>(state.request.alignment_images);
    validate.eval_max_dets = static_cast<int>(state.request.eval_max_dets);
    validate.batch_size = static_cast<int>(state.request.batch_size);
    validate.prefetch_factor = static_cast<int>(state.request.prefetch_factor);
    validate.device_id = state.request.device_id;
    validate.workers = state.request.workers;
    validate.cpu_affinity = state.request.cpu_affinity;
    validate.recompile = state.request.recompile;
    validate.profile = state.request.profile;
    validate.allow_fp16 = state.request.allow_fp16;
    validate.write_report_json = state.request.write_report_json;
}

void apply_train_seed(SeedSnapshotState& snapshot, const TrainSeedCliState& state) {
    snapshot.current_view = View::Train;
    const std::string inferred_preset =
        mmltk::rfdetr::cli_shared::infer_train_recipe_preset_name_with_native_fallback(state.request);
    if (!inferred_preset.empty()) {
        snapshot.selected_preset = inferred_preset;
    } else if (snapshot.selected_preset.empty()) {
        snapshot.selected_preset = kDefaultGuiPresetName;
    }

    TrainViewState& train = snapshot.train;
    train.execution_target = TrainExecutionTarget::Local;
    train.train_compiled_path = state.request.train_compiled_path.string();
    train.val_compiled_path = state.request.val_compiled_path.string();
    train.test_compiled_path = state.request.test_compiled_path.string();
    train.output_dir = state.request.output_dir.string();
    train.weights_path = state.request.weights_path.string();
    train.resume_path = state.request.resume_path.string();
    train.cpu_affinity = state.request.cpu_affinity;
    train.input_mode = state.request.resume_path.empty() ? TrainInputMode::Weights : TrainInputMode::Resume;
    train.batch_size = static_cast<int>(state.request.batch_size);
    train.val_batch_size = static_cast<int>(state.request.val_batch_size);
    train.epochs = state.request.epochs;
    train.grad_accum_steps = state.request.grad_accum_steps;
    train.eval_max_dets = static_cast<int>(state.request.eval_max_dets);
    train.lr_drop = state.request.lr_drop;
    train.print_freq = state.request.print_freq;
    train.prefetch_factor = state.request.prefetch_factor;
    train.seed = state.request.seed;
    train.workers = state.request.workers;
    train.lanes = state.request.lanes;
    train.lr = state.request.lr;
    train.lr_encoder = state.request.lr_encoder;
    train.lr_component_decay = state.request.lr_component_decay;
    train.encoder_layer_decay = state.request.encoder_layer_decay;
    train.momentum = state.request.momentum;
    train.weight_decay = state.request.weight_decay;
    train.warmup_epochs = state.request.warmup_epochs;
    train.warmup_momentum = state.request.warmup_momentum;
    train.lr_min_factor = state.request.lr_min_factor;
    train.clip_max_norm = state.request.clip_max_norm;
    train.lr_scheduler = state.request.lr_scheduler;
    train.use_ema = state.request.use_ema;
    train.amp = state.request.amp;
    train.progress_bar = state.request.progress_bar;
    train.freeze_encoder = state.request.freeze_encoder;
    train.optimizer = train_optimizer_mode_from_kind(state.request.optimizer);
    train.compile_mode = compile_mode_index_from_cli(state.compile_mode);
    train.local_device_ids = !state.request.device_ids.empty()
                                 ? state.request.device_ids
                                 : std::vector<int>{state.request.device_id};
    train.recipe_overrides = state.request.recipe_overrides;
}

void apply_build_engine_seed(SeedSnapshotState& snapshot, const mmltk::rfdetr::BuildEngineRequest& request) {
    snapshot.current_view = View::Export;
    snapshot.selected_preset = choose_seed_preset(snapshot.selected_preset, {request.onnx_path});
    rfdetr_workflows::apply_build_engine_request(snapshot.export_state, request);
}

void apply_export_onnx_seed(SeedSnapshotState& snapshot, const mmltk::rfdetr::ExportOnnxRequest& request) {
    snapshot.current_view = View::Export;
    snapshot.selected_preset = choose_seed_preset(snapshot.selected_preset, {request.weights_path});
    rfdetr_workflows::apply_export_onnx_request(snapshot.export_state, request);
}

} // namespace

void apply_gui_cli_seed_file(const fs::path& settings_path, const std::vector<std::string>& cli_args) {
    std::vector<std::string> normalized = normalize_seed_cli_args(cli_args);
    if (normalized.empty()) {
        throw std::runtime_error("GUI seeding requires a representable `mmltk rfdetr ...` command");
    }
    if (normalized.front() != "rfdetr") {
        throw std::runtime_error(
            "GUI seeding only supports RF-DETR commands: `mmltk rfdetr train`, `predict`, `validate`, `build-engine`, and `export-onnx`");
    }
    if (normalized.size() < 2U) {
        throw std::runtime_error(
            "GUI seeding requires one of: `mmltk rfdetr train`, `predict`, `validate`, `build-engine`, or `export-onnx`");
    }

    SeedSnapshotState snapshot = load_seed_snapshot(settings_path);
    const std::vector<std::string> command_args(normalized.begin() + 2, normalized.end());
    const std::string& subcommand = normalized[1];

    if (subcommand == "train") {
        apply_train_seed(snapshot, parse_train_seed(command_args, snapshot.selected_preset));
    } else if (subcommand == "predict") {
        apply_predict_seed(snapshot, parse_predict_seed(command_args));
    } else if (subcommand == "validate") {
        apply_validate_seed(snapshot, parse_validate_seed(command_args));
    } else if (subcommand == "build-engine") {
        apply_build_engine_seed(snapshot, parse_build_engine_seed(command_args));
    } else if (subcommand == "export-onnx") {
        apply_export_onnx_seed(snapshot, parse_export_onnx_seed(command_args));
    } else if (subcommand == "compile" ||
               subcommand == "info" ||
               subcommand == "evaluate" ||
               subcommand == "eval" ||
               subcommand == "val" ||
               subcommand == "normalize-weights") {
        throw std::runtime_error(
            "GUI seeding does not support `mmltk rfdetr " + subcommand + "` because the GUI has no matching workflow");
    } else {
        throw std::runtime_error(
            "GUI seeding only supports `mmltk rfdetr train`, `predict`, `validate`, `build-engine`, and `export-onnx`");
    }

    save_seed_snapshot(settings_path, snapshot);
}

} // namespace mmltk::gui
