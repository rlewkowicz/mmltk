#include "cli_seed.h"

#include "default_state.h"
#include "gui_settings.h"
#include "rfdetr_workflows.h"
#include "view_state.h"

#include "CLI11.hpp"

#include "mmltk/rfdetr/model_config.h"
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
        GuiSettingsSnapshot snapshot = bind_snapshot(state);
        apply_gui_settings(normalize_gui_settings_document(j), snapshot);
        state.current_view = snapshot.current_view;
        state.selected_preset = snapshot.selected_preset;
        return state;
    } catch (const std::exception& error) {
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
    rfdetr_workflows::apply_predict_request(snapshot.predict, state.request);
}

void apply_validate_seed(SeedSnapshotState& snapshot, const ValidateSeedCliState& state) {
    snapshot.current_view = View::Validate;
    snapshot.selected_preset = choose_seed_preset(
        snapshot.selected_preset,
        {state.request.onnx_path, state.request.tensorrt_path});
    rfdetr_workflows::apply_validate_request(snapshot.validate, state.request);
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
    rfdetr_workflows::apply_train_request(snapshot.train, state.request);
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
