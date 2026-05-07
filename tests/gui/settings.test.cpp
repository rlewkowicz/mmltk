#include "gui/default_state.h"
#include "gui/gui_settings.h"
#include "gui/view_state.h"

#include "support/catch2_compat.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace mmltk::gui;

GuiSettingsSnapshot make_snapshot(UiSettingsState& ui, TrainViewState& train, ValidateViewState& validate,
                                  PredictViewState& predict, AnnotateViewState& annotate,
                                  ExportViewState& export_state) {
    return GuiSettingsSnapshot{
        View::Annotate, "rf-detr-seg-medium", &ui, &train, &validate, &predict, &annotate, &export_state,
    };
}

nlohmann::json load_json_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    assert(stream.is_open());
    return nlohmann::json::parse(stream);
}

void test_ui_settings_round_trip() {
    UiSettingsState ui;
    ui.dark_mode = true;
    ui.ui_scale = 1.35f;
    ui.font_size = 18.0f;
    ui.secondary_font_size = 15.0f;
    ui.mono_font_size = 14.0f;
    ui.property_label_width = 192.0f;
    ui.crop_edge_hit_half_width = 11.0f;
    ui.crop_corner_hit_size = 24.0f;
    ui.crop_handle_radius = 7.5f;
    ui.density = UiDensity::Comfortable;

    TrainViewState train;
    train.train_compiled_path = "/tmp/train.bin";
    train.val_compiled_path = "/tmp/val.bin";
    train.test_compiled_path = "/tmp/test.bin";
    train.output_dir = "/tmp/train-output";
    train.weights_path = "/tmp/weights.pt";
    train.cpu_affinity = "0-3";
    train.progress_bar = true;
    train.local_device_ids = {0, 2};
    train.lanes = 2;
    train.recipe_overrides.lr = true;
    ValidateViewState validate;
    validate.compiled_path = "/tmp/validate.bin";
    validate.source_dir = "/tmp/source";
    validate.onnx_path = "/tmp/models/validate.onnx";
    validate.tensorrt_path = "/tmp/models/validate.engine";
    validate.save_engine_path = "/tmp/models/save.engine";
    validate.profile = true;
    PredictViewState predict;
    predict.source.kind = SourceKind::SingleImage;
    predict.source.single_image_path = "/tmp/input.png";
    predict.weights_path = "/tmp/predict.pt";
    predict.output_path = "/tmp/predictions.json";
    predict.progress_bar = true;
    AnnotateViewState annotate;
    annotate.source.kind = SourceKind::ImageFolder;
    annotate.source.image_directory = "/tmp/images";
    annotate.weights_path = "/tmp/annotate.pt";
    annotate.output_dir = "/tmp/annotated-scenes";
    annotate.full_frame = true;
    ExportViewState export_state;
    export_state.weights_path = "/tmp/export.pt";
    export_state.onnx_path = "/tmp/export.onnx";
    export_state.output_path = "/tmp/export.engine";
    export_state.allow_fp16 = false;

    GuiSettingsSnapshot snapshot = make_snapshot(ui, train, validate, predict, annotate, export_state);

    const nlohmann::json saved = snapshot_gui_settings(snapshot);
    assert(saved.at("schema_version") == kGuiSettingsSchemaVersion);
    assert(saved.at("workflows").at("train").at("dataset_paths").at("train_compiled_path") == "/tmp/train.bin");
    assert(saved.at("workflows").at("train").at("model_artifacts").at("weights_path") == "/tmp/weights.pt");
    assert(saved.at("workflows").at("train").at("execution").at("progress_bar") == true);
    assert(saved.at("workflows").at("train").at("training").at("local_device_ids") == nlohmann::json::array({0, 2}));
    assert(saved.at("workflows").at("train").at("training").at("recipe_overrides").at("lr") == true);
    assert(saved.at("workflows").at("validate").at("dataset_paths").at("compiled_path") == "/tmp/validate.bin");
    assert(saved.at("workflows").at("predict").at("predict").at("output_path") == "/tmp/predictions.json");
    assert(saved.at("workflows").at("annotate").at("annotate").at("output_dir") == "/tmp/annotated-scenes");
    assert(saved.at("workflows").at("export").at("export").at("output_path") == "/tmp/export.engine");

    UiSettingsState loaded_ui{};
    TrainViewState loaded_train;
    ValidateViewState loaded_validate;
    PredictViewState loaded_predict;
    AnnotateViewState loaded_annotate;
    ExportViewState loaded_export;
    GuiSettingsSnapshot loaded{
        View::Train, {}, &loaded_ui, &loaded_train, &loaded_validate, &loaded_predict, &loaded_annotate, &loaded_export,
    };

    apply_gui_settings(saved, loaded);

    assert(loaded.current_view == View::Annotate);
    assert(loaded.selected_preset == "rf-detr-seg-medium");
    assert(loaded_train.train_compiled_path == "/tmp/train.bin");
    assert(loaded_train.weights_path == "/tmp/weights.pt");
    assert(loaded_train.progress_bar);
    assert(loaded_train.local_device_ids == std::vector<int>({0, 2}));
    assert(loaded_train.recipe_overrides.lr);
    assert(loaded_validate.compiled_path == "/tmp/validate.bin");
    assert(loaded_validate.save_engine_path == "/tmp/models/save.engine");
    assert(loaded_predict.source.kind == SourceKind::SingleImage);
    assert(loaded_predict.source.single_image_path == "/tmp/input.png");
    assert(loaded_predict.progress_bar);
    assert(loaded_predict.output_path == "/tmp/predictions.json");
    assert(loaded_annotate.source.kind == SourceKind::ImageFolder);
    assert(loaded_annotate.source.image_directory == "/tmp/images");
    assert(loaded_annotate.full_frame);
    assert(loaded_export.output_path == "/tmp/export.engine");
    assert(!loaded_export.allow_fp16);
    assert(loaded_ui.dark_mode);
    assert(loaded_ui.ui_scale == 1.35f);
    assert(loaded_ui.font_size == 18.0f);
    assert(loaded_ui.secondary_font_size == 15.0f);
    assert(loaded_ui.mono_font_size == 14.0f);
    assert(loaded_ui.property_label_width == 192.0f);
    assert(loaded_ui.crop_edge_hit_half_width == 11.0f);
    assert(loaded_ui.crop_corner_hit_size == 24.0f);
    assert(loaded_ui.crop_handle_radius == 7.5f);
    assert(loaded_ui.density == UiDensity::Comfortable);
}

void test_fresh_defaults_use_capture_only_annotate() {
    std::string selected_preset;
    TrainViewState train;
    ValidateViewState validate;
    PredictViewState predict;
    AnnotateViewState annotate;
    ExportViewState export_state;

    apply_default_gui_state(selected_preset, train, validate, predict, annotate, export_state);

    assert(predict.model_input == ModelInputMode::Weights);
    assert(!predict.weights_path.empty());
    assert(annotate.model_input == ModelInputMode::None);
    assert(annotate.weights_path.empty());
    assert(annotate.onnx_path.empty());
    assert(annotate.tensorrt_path.empty());
}

void test_model_input_load_normalizes_invalid_values_by_workflow() {
    UiSettingsState ui;
    TrainViewState train;
    ValidateViewState validate;
    PredictViewState predict;
    AnnotateViewState annotate;
    ExportViewState export_state;
    GuiSettingsSnapshot snapshot = make_snapshot(ui, train, validate, predict, annotate, export_state);

    nlohmann::json saved = snapshot_gui_settings(snapshot);
    saved["workflows"]["predict"]["predict"]["model_input"] = 99;
    saved["workflows"]["annotate"]["annotate"]["model_input"] = 99;
    saved["workflows"]["annotate"]["model_artifacts"]["weights_path"] = "/explicit/annotate.pt";

    apply_gui_settings(saved, snapshot);

    assert(predict.model_input == ModelInputMode::Weights);
    assert(annotate.model_input == ModelInputMode::None);
    assert(annotate.weights_path == "/explicit/annotate.pt");
}

void test_persistence_migrates_schema2_to_schema3() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        ("mmltk-gui-settings-migration-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(temp_root);

    const std::filesystem::path legacy_path = temp_root / "legacy-gui.json";
    {
        nlohmann::json legacy;
        legacy["schema_version"] = 2;
        legacy["current_view"] = 4;
        legacy["selected_preset"] = "legacy";
        legacy["ui"] = {
            {"ui_scale", 1.75},
            {"font_size", 22.0},
        };
        legacy["workflows"] = {
            {"train",
             {
                 {"train_compiled_path", "/legacy/train.bin"},
                 {"val_compiled_path", "/legacy/val.bin"},
                 {"output_dir", "/legacy/train-output"},
                 {"weights_path", "/legacy/weights.pt"},
                 {"cpu_affinity", "0-1"},
                 {"batch_size", 4},
                 {"amp", false},
                 {"progress_bar", true},
             }},
            {"validate",
             {
                 {"compiled_path", "/legacy/validate.bin"},
                 {"onnx_path", "/legacy/validate.onnx"},
                 {"report_json_path", "/legacy/report.json"},
             }},
            {"predict",
             {
                 {"source",
                  {
                      {"kind", static_cast<int>(SourceKind::SingleImage)},
                      {"single_image_path", "/legacy/input.png"},
                  }},
                 {"weights_path", "/legacy/predict.pt"},
                 {"output_path", "/legacy/predictions.json"},
             }},
        };
        std::ofstream stream(legacy_path);
        assert(stream.is_open());
        stream << legacy.dump(2) << '\n';
    }

    UiSettingsState legacy_ui;
    legacy_ui.ui_scale = 3.0f;
    TrainViewState legacy_train;
    legacy_train.output_dir = "/keep";
    ValidateViewState legacy_validate;
    PredictViewState legacy_predict;
    AnnotateViewState legacy_annotate;
    ExportViewState legacy_export;
    GuiSettingsSnapshot legacy_snapshot{
        View::Train,      "sentinel",      &legacy_ui,       &legacy_train,
        &legacy_validate, &legacy_predict, &legacy_annotate, &legacy_export,
    };

    {
        GuiSettingsPersistence persistence(legacy_path.string());
        const bool loaded = persistence.load(legacy_snapshot);
        assert(loaded);
    }
    const nlohmann::json migrated = load_json_file(legacy_path);
    assert(migrated.at("schema_version") == kGuiSettingsSchemaVersion);
    assert(migrated.at("workflows").at("train").at("dataset_paths").at("train_compiled_path") == "/legacy/train.bin");
    assert(migrated.at("workflows").at("train").at("model_artifacts").at("weights_path") == "/legacy/weights.pt");
    assert(migrated.at("workflows").at("train").at("training").at("amp") == false);
    assert(migrated.at("workflows").at("predict").at("predict").at("output_path") == "/legacy/predictions.json");

    assert(legacy_snapshot.current_view == View::Export);
    assert(legacy_snapshot.selected_preset == "legacy");
    assert(legacy_ui.ui_scale == 1.75f);
    assert(legacy_train.output_dir == "/legacy/train-output");
    assert(!legacy_train.amp);
    assert(legacy_validate.compiled_path == "/legacy/validate.bin");
    assert(legacy_predict.source.kind == SourceKind::SingleImage);
    assert(legacy_predict.source.single_image_path == "/legacy/input.png");
    assert(legacy_predict.output_path == "/legacy/predictions.json");

    std::error_code cleanup_error;
    std::filesystem::remove(legacy_path, cleanup_error);
    std::filesystem::remove(temp_root, cleanup_error);
}

void test_persistence_rejects_unsupported_schema_and_malformed_files() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        ("mmltk-gui-settings-schema-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(temp_root);

    const std::filesystem::path unsupported_path = temp_root / "unsupported-gui.json";
    {
        nlohmann::json unsupported;
        unsupported["schema_version"] = 1;
        unsupported["current_view"] = 4;
        std::ofstream stream(unsupported_path);
        assert(stream.is_open());
        stream << unsupported.dump(2) << '\n';
    }

    UiSettingsState unsupported_ui;
    unsupported_ui.ui_scale = 3.0f;
    TrainViewState unsupported_train;
    unsupported_train.output_dir = "/keep";
    ValidateViewState unsupported_validate;
    PredictViewState unsupported_predict;
    AnnotateViewState unsupported_annotate;
    ExportViewState unsupported_export;
    GuiSettingsSnapshot unsupported_snapshot{
        View::Train,           "sentinel",           &unsupported_ui,       &unsupported_train,
        &unsupported_validate, &unsupported_predict, &unsupported_annotate, &unsupported_export,
    };

    {
        GuiSettingsPersistence persistence(unsupported_path.string());
        const bool loaded = persistence.load(unsupported_snapshot);
        assert(!loaded);
    }
    assert(unsupported_snapshot.current_view == View::Train);
    assert(unsupported_snapshot.selected_preset == "sentinel");
    assert(unsupported_ui.ui_scale == 3.0f);
    assert(unsupported_train.output_dir == "/keep");

    const std::filesystem::path malformed_path = temp_root / "malformed-gui.json";
    {
        std::ofstream stream(malformed_path);
        assert(stream.is_open());
        stream << "{ this is not valid json";
    }

    UiSettingsState malformed_ui;
    malformed_ui.ui_scale = 4.0f;
    TrainViewState malformed_train;
    malformed_train.output_dir = "/keep-malformed";
    ValidateViewState malformed_validate;
    PredictViewState malformed_predict;
    AnnotateViewState malformed_annotate;
    ExportViewState malformed_export;
    GuiSettingsSnapshot malformed_snapshot{
        View::Export,        "malformed",        &malformed_ui,       &malformed_train,
        &malformed_validate, &malformed_predict, &malformed_annotate, &malformed_export,
    };

    {
        GuiSettingsPersistence persistence(malformed_path.string());
        const bool loaded = persistence.load(malformed_snapshot);
        assert(!loaded);
    }
    assert(malformed_snapshot.current_view == View::Export);
    assert(malformed_snapshot.selected_preset == "malformed");
    assert(malformed_ui.ui_scale == 4.0f);
    assert(malformed_train.output_dir == "/keep-malformed");

    std::error_code cleanup_error;
    std::filesystem::remove(malformed_path, cleanup_error);
    std::filesystem::remove(temp_root, cleanup_error);
}

void test_persistence_coalesces_to_latest_snapshot() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        ("mmltk-gui-settings-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(temp_root);
    const std::filesystem::path settings_path = temp_root / "gui.json";

    UiSettingsState ui;
    TrainViewState train;
    ValidateViewState validate;
    PredictViewState predict;
    AnnotateViewState annotate;
    ExportViewState export_state;
    GuiSettingsSnapshot snapshot = make_snapshot(ui, train, validate, predict, annotate, export_state);

    {
        GuiSettingsPersistence persistence(settings_path.string());

        ui.ui_scale = 1.10f;
        persistence.notify_frame(snapshot);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        persistence.notify_frame(snapshot);
        persistence.flush();

        const nlohmann::json first_saved = load_json_file(settings_path);
        assert(first_saved.at("ui").at("ui_scale") == 1.10f);

        ui.ui_scale = 1.25f;
        persistence.notify_frame(snapshot);
        ui.ui_scale = 1.50f;
        persistence.notify_frame(snapshot);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        persistence.notify_frame(snapshot);
        persistence.flush();
    }

    const nlohmann::json saved = load_json_file(settings_path);
    assert(saved.at("ui").at("ui_scale") == 1.50f);
    assert(!std::filesystem::exists(settings_path.string() + ".tmp"));

    std::error_code cleanup_error;
    std::filesystem::remove(settings_path, cleanup_error);
    std::filesystem::remove(temp_root, cleanup_error);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_ui_settings_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_fresh_defaults_use_capture_only_annotate);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_model_input_load_normalizes_invalid_values_by_workflow);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_persistence_migrates_schema2_to_schema3);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_persistence_rejects_unsupported_schema_and_malformed_files);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_persistence_coalesces_to_latest_snapshot);
