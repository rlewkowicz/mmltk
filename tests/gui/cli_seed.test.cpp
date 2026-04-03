#include "gui/cli_seed.h"
#include "gui/gui_settings.h"

#include <nlohmann/json.hpp>

#include "support/catch2_compat.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

fs::path make_temp_settings_path(const char* label) {
    const fs::path temp_root =
        fs::temp_directory_path() /
        (std::string(label) + "-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    fs::create_directories(temp_root, error);
    return temp_root / "gui.json";
}

nlohmann::json load_json_file(const fs::path& path) {
    std::ifstream stream(path);
    assert(stream.is_open());
    return nlohmann::json::parse(stream);
}

void cleanup_temp_settings_path(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
    fs::remove(path.parent_path(), error);
}

void test_train_seed_sets_local_devices_and_overrides() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-train");
    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
            "--device-ids",
            "2,4",
            "--optimizer",
            "muon",
            "--print-freq",
            "3",
            "--lr",
            "0.0002",
            "--no-amp",
            "--compile-mode",
            "full",
        });

    const nlohmann::json saved = load_json_file(settings_path);
    const nlohmann::json& workflows = saved.at("workflows");
    assert(saved.at("current_view") == 0);
    assert(saved.at("selected_preset") == "rf-detr-seg-medium");
    assert(workflows.at("train").at("train_compiled_path") == "./compiled/train.bin");
    assert(workflows.at("train").at("val_compiled_path") == "./compiled/val.bin");
    assert(workflows.at("train").at("output_dir") == "./engines/out");
    assert(workflows.at("train").at("weights_path") == "./weights/rf-detr-seg-medium.pt");
    assert(workflows.at("train").at("local_device_ids") == nlohmann::json::array({2, 4}));
    assert(workflows.at("train").at("optimizer") == 1);
    assert(workflows.at("train").at("print_freq") == 3);
    assert(workflows.at("train").at("amp") == false);
    assert(workflows.at("train").at("compile_mode") == 2);
    assert(workflows.at("train").at("recipe_overrides").at("lr") == true);

    cleanup_temp_settings_path(settings_path);
}

void test_train_seed_progress_flags_round_trip() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-train-progress");
    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
            "--progress",
        });

    nlohmann::json saved = load_json_file(settings_path);
    assert(saved.at("workflows").at("train").at("progress_bar") == true);

    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
            "--no-progress",
        });

    saved = load_json_file(settings_path);
    assert(saved.at("workflows").at("train").at("progress_bar") == false);

    cleanup_temp_settings_path(settings_path);
}

void test_validate_seed_applies_cli_defaults() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-validate");
    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "validate",
            "--compiled",
            "/tmp/datasets/val.bin",
            "--onnx",
            "/tmp/models/rf-detr-seg-medium.onnx",
        });

    const nlohmann::json saved = load_json_file(settings_path);
    const nlohmann::json& workflows = saved.at("workflows");
    assert(saved.at("current_view") == 1);
    assert(saved.at("selected_preset") == "rf-detr-seg-medium");
    assert(workflows.at("validate").at("compiled_path") == "/tmp/datasets/val.bin");
    assert(workflows.at("validate").at("split") == "val");
    assert(workflows.at("validate").at("report_json_path") == "/tmp/datasets/rfdetr-validation-report.json");
    assert(workflows.at("validate").at("onnx_path") == "/tmp/models/rf-detr-seg-medium.onnx");
    assert(workflows.at("validate").at("tensorrt_path") == "");

    cleanup_temp_settings_path(settings_path);
}

void test_predict_seed_progress_flags_round_trip() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-predict-progress");
    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "predict",
            "--compiled",
            "./compiled/val.bin",
            "--output",
            "./predictions.json",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
            "--progress",
        });

    nlohmann::json saved = load_json_file(settings_path);
    assert(saved.at("workflows").at("predict").at("progress_bar") == true);

    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "predict",
            "--compiled",
            "./compiled/val.bin",
            "--output",
            "./predictions.json",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
            "--no-progress",
        });

    saved = load_json_file(settings_path);
    assert(saved.at("workflows").at("predict").at("progress_bar") == false);

    cleanup_temp_settings_path(settings_path);
}

void test_train_seed_preserves_existing_ui_settings() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-ui");
    {
        nlohmann::json initial;
        initial["schema_version"] = mmltk::gui::kGuiSettingsSchemaVersion;
        initial["current_view"] = 0;
        initial["selected_preset"] = "rf-detr-seg-medium";
        initial["ui"] = {
            {"ui_scale", 1.35},
            {"font_size", 18.0},
            {"secondary_font_size", 15.0},
            {"mono_font_size", 14.0},
            {"property_label_width", 192.0},
            {"density", 2},
        };

        std::ofstream stream(settings_path);
        assert(stream.is_open());
        stream << initial.dump(2) << '\n';
    }

    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
        });

    const nlohmann::json saved = load_json_file(settings_path);
    assert(std::abs(saved.at("ui").at("ui_scale").get<double>() - 1.35) < 1e-5);
    assert(saved.at("ui").at("font_size") == 18.0);
    assert(saved.at("ui").at("secondary_font_size") == 15.0);
    assert(saved.at("ui").at("mono_font_size") == 14.0);
    assert(saved.at("ui").at("property_label_width") == 192.0);
    assert(saved.at("ui").at("density") == 2);

    cleanup_temp_settings_path(settings_path);
}

void test_train_seed_discards_legacy_schema_and_malformed_settings() {
    const fs::path legacy_settings_path = make_temp_settings_path("mmltk-gui-seed-legacy");
    {
        nlohmann::json legacy;
        legacy["schema_version"] = 0;
        legacy["current_view"] = 4;
        legacy["selected_preset"] = "legacy-preset";
        legacy["ui"] = {
            {"ui_scale", 1.75},
        };

        std::ofstream stream(legacy_settings_path);
        assert(stream.is_open());
        stream << legacy.dump(2) << '\n';
    }

    mmltk::gui::apply_gui_cli_seed_file(
        legacy_settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
        });

    nlohmann::json saved = load_json_file(legacy_settings_path);
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("selected_preset") == "rf-detr-seg-medium");
    assert(std::abs(saved.at("ui").at("ui_scale").get<double>() - 1.0) < 1e-5);
    assert(saved.at("workflows").at("train").at("train_compiled_path") == "./compiled/train.bin");

    cleanup_temp_settings_path(legacy_settings_path);

    const fs::path malformed_settings_path = make_temp_settings_path("mmltk-gui-seed-malformed");
    {
        std::ofstream stream(malformed_settings_path);
        assert(stream.is_open());
        stream << "{";
    }

    mmltk::gui::apply_gui_cli_seed_file(
        malformed_settings_path,
        {
            "rfdetr",
            "train",
            "--train-compiled",
            "./compiled/train.bin",
            "--val-compiled",
            "./compiled/val.bin",
            "--output-dir",
            "./engines/out",
            "--weights",
            "./weights/rf-detr-seg-medium.pt",
        });

    saved = load_json_file(malformed_settings_path);
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("selected_preset") == "rf-detr-seg-medium");
    assert(saved.at("workflows").at("train").at("output_dir") == "./engines/out");

    cleanup_temp_settings_path(malformed_settings_path);
}

void test_build_engine_seed_updates_export_view() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-build-engine");
    mmltk::gui::apply_gui_cli_seed_file(
        settings_path,
        {
            "rfdetr",
            "build-engine",
            "--onnx",
            "./models/rf-detr-seg-medium.onnx",
            "--output",
            "./engines/model.engine",
            "--device-id",
            "3",
            "--no-fp16",
        });

    const nlohmann::json saved = load_json_file(settings_path);
    assert(saved.at("current_view") == 4);
    assert(saved.at("workflows").at("export").at("weights_path") == "");
    assert(saved.at("workflows").at("export").at("onnx_path") == "./models/rf-detr-seg-medium.onnx");
    assert(saved.at("workflows").at("export").at("output_path") == "./engines/model.engine");
    assert(saved.at("workflows").at("export").at("device_id") == 3);
    assert(saved.at("workflows").at("export").at("allow_fp16") == false);
    assert(saved.at("workflows").at("export").at("build_tensorrt") == true);

    cleanup_temp_settings_path(settings_path);
}

void test_unsupported_train_flag_is_rejected() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-train-unsupported");
    bool threw = false;
    try {
        mmltk::gui::apply_gui_cli_seed_file(
            settings_path,
            {
                "rfdetr",
                "train",
                "--train-compiled",
                "./compiled/train.bin",
                "--val-compiled",
                "./compiled/val.bin",
                "--output-dir",
                "./engines/out",
                "--weights",
                "./weights/rf-detr-seg-medium.pt",
                "--ema-decay",
                "0.9",
            });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    cleanup_temp_settings_path(settings_path);
}

void test_unsupported_validate_flag_is_rejected() {
    const fs::path settings_path = make_temp_settings_path("mmltk-gui-seed-validate-unsupported");
    bool threw = false;
    try {
        mmltk::gui::apply_gui_cli_seed_file(
            settings_path,
            {
                "rfdetr",
                "validate",
                "--compiled",
                "./compiled/val.bin",
                "--onnx",
                "./models/rf-detr-seg-medium.onnx",
                "--compile-workers",
                "8",
            });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    cleanup_temp_settings_path(settings_path);
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_sets_local_devices_and_overrides);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_progress_flags_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_validate_seed_applies_cli_defaults);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_predict_seed_progress_flags_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_preserves_existing_ui_settings);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_discards_legacy_schema_and_malformed_settings);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_build_engine_seed_updates_export_view);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_unsupported_train_flag_is_rejected);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_unsupported_validate_flag_is_rejected);
