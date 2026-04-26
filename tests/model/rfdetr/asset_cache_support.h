#pragma once

#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/weight_catalog.h"

#include "support/subprocess_test_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr::testsupport {

namespace fs = std::filesystem;

struct CachedModelAssets {
    std::string preset_name;
    fs::path root_dir;
    fs::path upstream_weights_path;
    fs::path native_checkpoint_path;
    fs::path onnx_path;
    fs::path tensorrt_path;
};

inline fs::path cached_model_assets_root() {
    if (const char* env = std::getenv("MMLTK_RFDETR_TEST_CACHE_DIR"); env != nullptr && env[0] != '\0') {
        return {env};
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".cache" / "mmltk" / "tests" / "rfdetr";
    }
    return fs::temp_directory_path() / "mmltk" / "tests" / "rfdetr";
}

inline const ModelPresetConfig& require_model_preset(std::string_view preset_name) {
    const auto* preset = find_model_preset(preset_name);
    if (preset == nullptr) {
        throw std::runtime_error("unknown RF-DETR test preset: " + std::string(preset_name));
    }
    return *preset;
}

inline const WeightAsset& require_weight_asset(std::string_view canonical_filename) {
    const auto* asset = find_weight_asset(canonical_filename);
    if (asset == nullptr) {
        throw std::runtime_error("missing RF-DETR weight catalog entry: " + std::string(canonical_filename));
    }
    return *asset;
}

inline bool is_nonempty_regular_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error && fs::file_size(path, error) > 0;
}

inline bool is_nonempty_regular_file_newer_than(const fs::path& path, const fs::path& input) {
    if (!is_nonempty_regular_file(path)) {
        return false;
    }
    std::error_code error;
    const auto output_time = fs::last_write_time(path, error);
    if (error) {
        return false;
    }
    const auto input_time = fs::last_write_time(input, error);
    if (error) {
        return false;
    }
    return output_time >= input_time;
}

inline void remove_if_exists(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
}

inline std::string first_whitespace_delimited_token(const std::string& text) {
    const size_t end = text.find_first_of(" \t\r\n");
    return end == std::string::npos ? text : text.substr(0, end);
}

inline void run_checked(const std::vector<std::string>& args, std::string_view step_name) {
    const auto result = mmltk::testsupport::run_subprocess_capture_output(args);
    if (result.exit_code == 0) {
        return;
    }
    throw std::runtime_error(std::string(step_name) + " failed with exit code " + std::to_string(result.exit_code) +
                             "\n" + result.output_text);
}

inline std::string md5_of_file(const fs::path& path) {
    const auto result = mmltk::testsupport::run_subprocess_capture_output({"md5sum", path.string()});
    if (result.exit_code != 0) {
        throw std::runtime_error("md5sum failed for " + path.string() + "\n" + result.output_text);
    }
    return first_whitespace_delimited_token(result.output_text);
}

inline bool validate_tensorrt_engine(const fs::path& tensorrt_path) {
    if (!is_nonempty_regular_file(tensorrt_path)) {
        return false;
    }
    const auto result = mmltk::testsupport::run_subprocess_capture_output({
        mmltk::testsupport::mmltk_cli_path(),
        "rfdetr",
        "info",
        "--tensorrt",
        tensorrt_path.string(),
    });
    return result.exit_code == 0;
}

inline void ensure_downloaded_weight(const fs::path& output_path, const WeightAsset& asset) {
    if (is_nonempty_regular_file(output_path) && md5_of_file(output_path) == asset.md5_hash) {
        return;
    }

    fs::create_directories(output_path.parent_path());
    const fs::path temp_path = output_path.string() + ".part";
    remove_if_exists(temp_path);

    run_checked(
        {
            "curl",
            "-L",
            "--fail",
            "--silent",
            "--show-error",
            "--retry",
            "3",
            "--output",
            temp_path.string(),
            std::string(asset.download_url),
        },
        "RF-DETR weight download");

    const std::string actual_md5 = md5_of_file(temp_path);
    if (actual_md5 != asset.md5_hash) {
        remove_if_exists(temp_path);
        throw std::runtime_error("downloaded RF-DETR weight hash mismatch for " + output_path.string() +
                                 ": expected=" + std::string(asset.md5_hash) + " actual=" + actual_md5);
    }

    remove_if_exists(output_path);
    fs::rename(temp_path, output_path);
}

inline void ensure_native_checkpoint(const fs::path& upstream_weights_path, const fs::path& native_checkpoint_path) {
    if (is_nonempty_regular_file_newer_than(native_checkpoint_path, upstream_weights_path)) {
        return;
    }

    fs::create_directories(native_checkpoint_path.parent_path());
    const fs::path temp_path = native_checkpoint_path.string() + ".part";
    remove_if_exists(temp_path);

    run_checked(
        {
            mmltk::testsupport::mmltk_cli_path(),
            "rfdetr",
            "normalize-weights",
            "--input",
            upstream_weights_path.string(),
            "--output",
            temp_path.string(),
        },
        "RF-DETR native checkpoint export");

    remove_if_exists(native_checkpoint_path);
    fs::rename(temp_path, native_checkpoint_path);
}

inline void ensure_exported_onnx(const fs::path& native_checkpoint_path, const fs::path& onnx_path) {
    if (is_nonempty_regular_file_newer_than(onnx_path, native_checkpoint_path)) {
        return;
    }

    fs::create_directories(onnx_path.parent_path());
    const fs::path temp_path = onnx_path.string() + ".part";
    remove_if_exists(temp_path);

    run_checked(
        {
            mmltk::testsupport::mmltk_cli_path(),
            "rfdetr",
            "export-onnx",
            "--weights",
            native_checkpoint_path.string(),
            "--output",
            temp_path.string(),
            "--device-id",
            "0",
            "--simplify",
        },
        "RF-DETR ONNX export");

    remove_if_exists(onnx_path);
    fs::rename(temp_path, onnx_path);
}

inline void ensure_built_tensorrt_engine(const fs::path& onnx_path, const fs::path& tensorrt_path) {
    if (is_nonempty_regular_file_newer_than(tensorrt_path, onnx_path) && validate_tensorrt_engine(tensorrt_path)) {
        return;
    }

    fs::create_directories(tensorrt_path.parent_path());
    const fs::path temp_path = tensorrt_path.string() + ".part";
    remove_if_exists(temp_path);
    remove_if_exists(tensorrt_path);

    run_checked(
        {
            mmltk::testsupport::mmltk_cli_path(),
            "rfdetr",
            "build-engine",
            "--onnx",
            onnx_path.string(),
            "--output",
            temp_path.string(),
            "--device-id",
            "0",
        },
        "RF-DETR TensorRT build");

    remove_if_exists(tensorrt_path);
    fs::rename(temp_path, tensorrt_path);
}

inline CachedModelAssets ensure_cached_model_assets(std::string_view preset_name = "rf-detr-nano") {
    const auto& preset = require_model_preset(preset_name);
    const auto& asset = require_weight_asset(preset.canonical_weight_filename);

    CachedModelAssets assets;
    assets.preset_name = std::string(preset.preset_name);
    assets.root_dir = cached_model_assets_root() / assets.preset_name;
    assets.upstream_weights_path = assets.root_dir / std::string(asset.filename);
    assets.native_checkpoint_path = assets.root_dir / (assets.preset_name + ".native.pt");
    assets.onnx_path = assets.root_dir / "inference_model.sim.onnx";
    assets.tensorrt_path = assets.root_dir / "inference_model.engine";

    ensure_downloaded_weight(assets.upstream_weights_path, asset);
    ensure_native_checkpoint(assets.upstream_weights_path, assets.native_checkpoint_path);
    ensure_exported_onnx(assets.native_checkpoint_path, assets.onnx_path);
    ensure_built_tensorrt_engine(assets.onnx_path, assets.tensorrt_path);
    return assets;
}

}  // namespace mmltk::rfdetr::testsupport
