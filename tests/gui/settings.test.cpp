#include "gui/gui_settings.h"

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

GuiSettingsSnapshot make_snapshot(UiSettingsState& ui,
                                  TrainViewState& train,
                                  ValidateViewState& validate,
                                  PredictViewState& predict,
                                  AnnotateViewState& annotate,
                                  ExportViewState& export_state) {
    return GuiSettingsSnapshot{
        View::Annotate,
        "rf-detr-seg-medium",
        &ui,
        &train,
        &validate,
        &predict,
        &annotate,
        &export_state,
    };
}

nlohmann::json load_json_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    assert(stream.is_open());
    return nlohmann::json::parse(stream);
}

void test_ui_settings_round_trip() {
    UiSettingsState ui;
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
    train.local_device_ids = {0, 2};
    train.progress_bar = true;
    ValidateViewState validate;
    PredictViewState predict;
    predict.progress_bar = true;
    AnnotateViewState annotate;
    ExportViewState export_state;

    GuiSettingsSnapshot snapshot = make_snapshot(ui, train, validate, predict, annotate, export_state);

    const nlohmann::json saved = snapshot_gui_settings(snapshot);
    assert(saved.at("schema_version") == kGuiSettingsSchemaVersion);

    UiSettingsState loaded_ui{};
    TrainViewState loaded_train;
    ValidateViewState loaded_validate;
    PredictViewState loaded_predict;
    AnnotateViewState loaded_annotate;
    ExportViewState loaded_export;
    GuiSettingsSnapshot loaded{
        View::Train,
        {},
        &loaded_ui,
        &loaded_train,
        &loaded_validate,
        &loaded_predict,
        &loaded_annotate,
        &loaded_export,
    };

    apply_gui_settings(saved, loaded);

    assert(loaded.current_view == View::Annotate);
    assert(loaded.selected_preset == "rf-detr-seg-medium");
    assert(loaded_train.local_device_ids == std::vector<int>({0, 2}));
    assert(loaded_train.progress_bar);
    assert(loaded_predict.progress_bar);
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

void test_persistence_rejects_legacy_schema_and_malformed_files() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        ("mmltk-gui-settings-schema-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(temp_root);

    const std::filesystem::path legacy_path = temp_root / "legacy-gui.json";
    {
        nlohmann::json legacy;
        legacy["schema_version"] = 0;
        legacy["current_view"] = 4;
        legacy["selected_preset"] = "legacy";
        legacy["ui"] = {
            {"ui_scale", 1.75},
            {"font_size", 22.0},
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
        View::Train,
        "sentinel",
        &legacy_ui,
        &legacy_train,
        &legacy_validate,
        &legacy_predict,
        &legacy_annotate,
        &legacy_export,
    };

    {
        GuiSettingsPersistence persistence(legacy_path.string());
        const bool loaded = persistence.load(legacy_snapshot);
        assert(!loaded);
    }
    assert(legacy_snapshot.current_view == View::Train);
    assert(legacy_snapshot.selected_preset == "sentinel");
    assert(legacy_ui.ui_scale == 3.0f);
    assert(legacy_train.output_dir == "/keep");

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
        View::Export,
        "malformed",
        &malformed_ui,
        &malformed_train,
        &malformed_validate,
        &malformed_predict,
        &malformed_annotate,
        &malformed_export,
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
    std::filesystem::remove(legacy_path, cleanup_error);
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

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_ui_settings_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_persistence_rejects_legacy_schema_and_malformed_files);
MMLTK_REGISTER_TEST_CASE("[gui][settings]", test_persistence_coalesces_to_latest_snapshot);
