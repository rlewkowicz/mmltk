#include "gui/cli_seed.h"
#include "gui/gui_settings.h"
#include "gui/view_state.h"

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
using SeedApplyFn = void (*)(const fs::path&, std::initializer_list<const char*>);

fs::path make_temp_settings_path(const char* label) {
    const fs::path temp_root =
        fs::temp_directory_path() / (std::string(label) + "-" + std::to_string(static_cast<long long>(::getpid())));
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

class TempSettingsFile {
   public:
    explicit TempSettingsFile(const char* label) : path_(make_temp_settings_path(label)) {}

    ~TempSettingsFile() {
        cleanup_temp_settings_path(path_);
    }

    [[nodiscard]] const fs::path& path() const {
        return path_;
    }

   private:
    fs::path path_;
};

std::vector<std::string> append_args(std::vector<std::string> args, std::initializer_list<const char*> extra_args) {
    args.reserve(args.size() + extra_args.size());
    for (const char* arg : extra_args) {
        args.emplace_back(arg);
    }
    return args;
}

std::vector<std::string> base_train_seed_args() {
    return {
        "rfdetr",           "train",
        "--train-compiled", "./compiled/train.bin",
        "--val-compiled",   "./compiled/val.bin",
        "--output-dir",     "./engines/out",
        "--weights",        "./weights/rf-detr-seg-medium.pt",
    };
}

std::vector<std::string> base_validate_seed_args() {
    return {
        "rfdetr", "validate", "--compiled", "./compiled/val.bin", "--onnx", "./models/rf-detr-seg-medium.onnx",
    };
}

std::vector<std::string> absolute_validate_seed_args() {
    return {
        "rfdetr", "validate", "--compiled", "/tmp/datasets/val.bin", "--onnx", "/tmp/models/rf-detr-seg-medium.onnx",
    };
}

std::vector<std::string> predict_seed_args(std::string model_flag, std::string model_path) {
    return {
        "rfdetr",
        "predict",
        "--compiled",
        "./compiled/val.bin",
        "--output",
        "./predictions.json",
        std::move(model_flag),
        std::move(model_path),
    };
}

std::vector<std::string> base_predict_weights_seed_args() {
    return predict_seed_args("--weights", "./weights/rf-detr-seg-medium.pt");
}

std::vector<std::string> predict_onnx_seed_args() {
    return predict_seed_args("--onnx", "./models/rf-detr-seg-medium.onnx");
}

std::vector<std::string> build_engine_seed_args() {
    return {
        "rfdetr",    "build-engine",           "--onnx",      "./models/rf-detr-seg-medium.onnx",
        "--output",  "./engines/model.engine", "--device-id", "3",
        "--no-fp16",
    };
}

void apply_seed_file(const fs::path& settings_path, const std::vector<std::string>& args) {
    mmltk::gui::apply_gui_cli_seed_file(settings_path, args);
}

void apply_train_seed_file(const fs::path& settings_path, std::initializer_list<const char*> extra_args = {}) {
    apply_seed_file(settings_path, append_args(base_train_seed_args(), extra_args));
}

void apply_validate_seed_file(const fs::path& settings_path, std::initializer_list<const char*> extra_args = {}) {
    apply_seed_file(settings_path, append_args(base_validate_seed_args(), extra_args));
}

void apply_predict_weights_seed_file(const fs::path& settings_path,
                                     std::initializer_list<const char*> extra_args = {}) {
    apply_seed_file(settings_path, append_args(base_predict_weights_seed_args(), extra_args));
}

void write_json_file(const fs::path& path, const nlohmann::json& document) {
    std::ofstream stream(path);
    assert(stream.is_open());
    stream << document.dump(2) << '\n';
}

void write_text_file(const fs::path& path, const char* contents) {
    std::ofstream stream(path);
    assert(stream.is_open());
    stream << contents;
}

void expect_progress_flag_round_trip(const char* label, const char* workflow_name, SeedApplyFn apply_seed) {
    const TempSettingsFile settings(label);
    apply_seed(settings.path(), {"--progress"});

    nlohmann::json saved = load_json_file(settings.path());
    assert(saved.at("workflows").at(workflow_name).at("execution").at("progress_bar") == true);

    apply_seed(settings.path(), {"--no-progress"});

    saved = load_json_file(settings.path());
    assert(saved.at("workflows").at(workflow_name).at("execution").at("progress_bar") == false);
}

void expect_unsupported_flag_is_rejected(const char* label, SeedApplyFn apply_seed,
                                         std::initializer_list<const char*> args) {
    const TempSettingsFile settings(label);
    bool threw = false;
    try {
        apply_seed(settings.path(), args);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void test_train_seed_sets_local_devices_and_overrides() {
    const TempSettingsFile settings("mmltk-gui-seed-train");
    apply_train_seed_file(settings.path(), {
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

    const nlohmann::json saved = load_json_file(settings.path());
    const nlohmann::json& workflows = saved.at("workflows");
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("current_view") == 0);
    assert(workflows.at("train").at("model_artifacts").at("preset_name") == "rf-detr-seg-medium");
    assert(workflows.at("train").at("dataset_paths").at("train_compiled_path") == "./compiled/train.bin");
    assert(workflows.at("train").at("dataset_paths").at("val_compiled_path") == "./compiled/val.bin");
    assert(workflows.at("train").at("model_artifacts").at("weights_path") == "./weights/rf-detr-seg-medium.pt");
    assert(workflows.at("train").at("execution").at("compile_mode") == 2);
    assert(workflows.at("train").at("training").at("output_dir") == "./engines/out");
    assert(workflows.at("train").at("training").at("local_device_ids") == nlohmann::json::array({2, 4}));
    assert(workflows.at("train").at("training").at("amp") == false);
    assert(workflows.at("train").at("training").at("optimizer") == 1);
    assert(workflows.at("train").at("training").at("print_freq") == 3);
    assert(workflows.at("train").at("training").at("recipe_overrides").at("lr") == true);
}

void test_train_seed_progress_flags_round_trip() {
    expect_progress_flag_round_trip("mmltk-gui-seed-train-progress", "train", apply_train_seed_file);
}

void test_validate_seed_applies_cli_defaults() {
    const TempSettingsFile settings("mmltk-gui-seed-validate");
    apply_seed_file(settings.path(), absolute_validate_seed_args());

    const nlohmann::json saved = load_json_file(settings.path());
    const nlohmann::json& workflows = saved.at("workflows");
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("current_view") == 1);
    assert(workflows.at("validate").at("model_artifacts").at("preset_name") == "rf-detr-seg-medium");
    assert(workflows.at("validate").at("dataset_paths").at("compiled_path") == "/tmp/datasets/val.bin");
    assert(workflows.at("validate").at("validation").at("split") == "val");
    assert(workflows.at("validate").at("validation").at("report_json_path") ==
           "/tmp/datasets/rfdetr-validation-report.json");
    assert(workflows.at("validate").at("model_artifacts").at("onnx_path") == "/tmp/models/rf-detr-seg-medium.onnx");
    assert(workflows.at("validate").at("model_artifacts").at("tensorrt_path") == "");
}

void test_predict_seed_progress_flags_round_trip() {
    expect_progress_flag_round_trip("mmltk-gui-seed-predict-progress", "predict", apply_predict_weights_seed_file);
}

void test_predict_seed_applies_onnx_model_input() {
    const TempSettingsFile settings("mmltk-gui-seed-predict-onnx");
    apply_seed_file(settings.path(), predict_onnx_seed_args());

    const nlohmann::json saved = load_json_file(settings.path());
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("current_view") == 2);
    const nlohmann::json& predict = saved.at("workflows").at("predict");
    assert(predict.at("model_artifacts").at("preset_name") == "rf-detr-seg-medium");
    assert(predict.at("model_artifacts").at("weights_path") == "");
    assert(predict.at("model_artifacts").at("onnx_path") == "./models/rf-detr-seg-medium.onnx");
    assert(predict.at("model_artifacts").at("tensorrt_path") == "");
    assert(predict.at("model_artifacts").at("input") == 1);
}

void test_train_seed_preserves_existing_ui_settings() {
    const TempSettingsFile settings("mmltk-gui-seed-ui");
    nlohmann::json initial;
    initial["schema_version"] = mmltk::gui::kGuiSettingsSchemaVersion;
    initial["current_view"] = 0;
    initial["ui"] = {
        {"ui_scale", 1.35},
        {"font_size", 18.0},
        {"secondary_font_size", 15.0},
        {"mono_font_size", 14.0},
        {"text_input_font_size", 17.0},
        {"density", 2},
    };
    write_json_file(settings.path(), initial);

    apply_train_seed_file(settings.path());

    const nlohmann::json saved = load_json_file(settings.path());
    assert(std::abs(saved.at("ui").at("ui_scale").get<double>() - 1.35) < 1e-5);
    assert(saved.at("ui").at("font_size") == 18.0);
    assert(saved.at("ui").at("secondary_font_size") == 15.0);
    assert(saved.at("ui").at("mono_font_size") == 14.0);
    assert(saved.at("ui").at("text_input_font_size") == 17.0);
    assert(saved.at("ui").at("density") == 2);
}

void test_train_seed_migrates_schema2_and_malformed_settings() {
    const TempSettingsFile legacy_settings("mmltk-gui-seed-legacy");
    nlohmann::json legacy;
    legacy["schema_version"] = 2;
    legacy["current_view"] = 4;
    legacy["selected_preset"] = "legacy-preset";
    legacy["ui"] = {
        {"ui_scale", 1.75},
    };
    legacy["workflows"] = {
        {"train",
         {
             {"train_compiled_path", "./compiled/train.bin"},
             {"val_compiled_path", "./compiled/val.bin"},
             {"output_dir", "./engines/out"},
             {"weights_path", "./weights/rf-detr-seg-medium.pt"},
         }},
    };
    write_json_file(legacy_settings.path(), legacy);

    apply_train_seed_file(legacy_settings.path());

    nlohmann::json saved = load_json_file(legacy_settings.path());
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("workflows").at("train").at("model_artifacts").at("preset_name") == "rf-detr-seg-medium");
    assert(std::abs(saved.at("ui").at("ui_scale").get<double>() - 1.75) < 1e-5);
    assert(saved.at("workflows").at("train").at("dataset_paths").at("train_compiled_path") == "./compiled/train.bin");
    assert(saved.at("workflows").at("train").at("training").at("output_dir") == "./engines/out");

    const TempSettingsFile malformed_settings("mmltk-gui-seed-malformed");
    write_text_file(malformed_settings.path(), "{");

    apply_train_seed_file(malformed_settings.path());

    saved = load_json_file(malformed_settings.path());
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("workflows").at("train").at("model_artifacts").at("preset_name") == "rf-detr-seg-medium");
    assert(saved.at("workflows").at("train").at("training").at("output_dir") == "./engines/out");
}

void test_build_engine_seed_updates_export_view() {
    const TempSettingsFile settings("mmltk-gui-seed-build-engine");
    apply_seed_file(settings.path(), build_engine_seed_args());

    const nlohmann::json saved = load_json_file(settings.path());
    assert(saved.at("schema_version") == mmltk::gui::kGuiSettingsSchemaVersion);
    assert(saved.at("current_view") == 4);
    assert(saved.at("workflows").at("export").at("model_artifacts").at("weights_path") == "");
    assert(saved.at("workflows").at("export").at("model_artifacts").at("onnx_path") ==
           "./models/rf-detr-seg-medium.onnx");
    assert(saved.at("workflows").at("export").at("execution").at("device_id") == 3);
    assert(saved.at("workflows").at("export").at("execution").at("allow_fp16") == false);
    assert(saved.at("workflows").at("export").at("export").at("output_path") == "./engines/model.engine");
    assert(saved.at("workflows").at("export").at("export").at("build_tensorrt") == true);
}

void test_unsupported_train_flag_is_rejected() {
    expect_unsupported_flag_is_rejected("mmltk-gui-seed-train-unsupported", apply_train_seed_file,
                                        {"--ema-decay", "0.9"});
}

void test_unsupported_validate_flag_is_rejected() {
    expect_unsupported_flag_is_rejected("mmltk-gui-seed-validate-unsupported", apply_validate_seed_file,
                                        {"--compile-workers", "8"});
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_sets_local_devices_and_overrides);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_progress_flags_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_validate_seed_applies_cli_defaults);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_predict_seed_progress_flags_round_trip);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_predict_seed_applies_onnx_model_input);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_preserves_existing_ui_settings);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_train_seed_migrates_schema2_and_malformed_settings);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_build_engine_seed_updates_export_view);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_unsupported_train_flag_is_rejected);
MMLTK_REGISTER_TEST_CASE("[gui][cli_seed]", test_unsupported_validate_flag_is_rejected);
