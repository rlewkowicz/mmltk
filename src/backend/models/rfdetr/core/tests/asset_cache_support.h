#pragma once
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
// RF-DETR core test fixture support.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/entrypoints/cli/tests/support/cli_path.h"
#include "src/test_support/subprocess_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/contract/weight_catalog.h"
namespace mmltk::backend::models::rfdetr::testsupport {
namespace fs = std::filesystem;
struct CachedModelAssets {
    std::string preset_name;
    fs::path root_dir;
    fs::path upstream_weights_path;
    fs::path native_checkpoint_path;
    fs::path onnx_path;
    fs::path tensorrt_path;
    bool onnx_metadata_validated = false;
};
inline fs::path cached_model_assets_root() {
    if (const char* env = std::getenv("MMLTK_RFDETR_TEST_CACHE_DIR"); env != nullptr && env[0] != '\0') { return {env}; }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') { return fs::path(home) / ".cache" / "mmltk" / "tests" / "rfdetr"; }
    return fs::temp_directory_path() / "mmltk" / "tests" / "rfdetr";
}
inline const PresetCatalogEntry& require_model_preset(std::string_view preset_name) {
    const auto* preset = find_model_preset(preset_name);
    if (preset == nullptr) { throw std::runtime_error("unknown RF-DETR test preset: " + std::string(preset_name)); }
    return *preset;
}
// CLEANUP-IGNORE -- parallel catalog guards intentionally preserve the concrete asset type and diagnostic.
inline const WeightAsset& require_weight_asset(std::string_view canonical_filename) {
    const auto* asset = find_weight_asset(canonical_filename);
    if (asset == nullptr) { throw std::runtime_error("missing RF-DETR weight catalog entry: " + std::string(canonical_filename)); }
    return *asset;
}
inline bool is_nonempty_regular_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error && fs::file_size(path, error) > 0;
}
inline bool is_nonempty_regular_file_newer_than(const fs::path& path, const fs::path& input) {
    if (!is_nonempty_regular_file(path)) { return false; }
    std::error_code error;
    const auto output_time = fs::last_write_time(path, error);
    if (error) { return false; }
    const auto input_time = fs::last_write_time(input, error);
    if (error) { return false; }
    return output_time >= input_time;
}
inline void remove_if_exists(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
}
inline void run_checked(const std::vector<std::string>& args, std::string_view step_name) {
    const auto result = mmltk::testsupport::run_subprocess_capture_output(args);
    if (result.exit_code == 0) { return; }
    throw std::runtime_error(std::string(step_name) + " failed with exit code " + std::to_string(result.exit_code) + "\n" + result.output_text);
}
inline std::string md5_of_file(const fs::path& path) { return mmltk::common::io::try_file_digests(path, true)->md5; }
bool validate_onnx_model(const fs::path& onnx_path);
inline bool validate_tensorrt_engine(const fs::path& tensorrt_path) {
    if (!is_nonempty_regular_file(tensorrt_path)) { return false; }
    try {
        const auto descriptor = detail::read_class_descriptor(tensorrt_path.string() + ".classes.json");
        if (descriptor.artifact_sha256 != mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(tensorrt_path)) ||
            !ResolvedClassLayout(descriptor.layout).semantic())
            return false;
    } catch (const std::exception&) { return false; }
    const auto result = mmltk::testsupport::run_subprocess_capture_output({
        mmltk::testsupport::mmltk_cli_path(),
        "rfdetr",
        "info",
        "--tensorrt",
        tensorrt_path.string(),
    });
    return result.exit_code == 0;
}
// Regenerates `output_path` atomically: the command produced by `make_command(temp_path)` writes to
// a sibling `.part` file which is verified (optionally) and then renamed over the output.
// The incumbent remains intact until that publication succeeds.
template <typename MakeCommand, typename VerifyTemp>
inline void regenerate_file_atomically(const fs::path& output_path, const std::string_view step_name, MakeCommand&& make_command, VerifyTemp&& verify_temp) {
    fs::create_directories(output_path.parent_path());
    const fs::path temp_path = output_path.string() + ".part";
    remove_if_exists(temp_path);
    run_checked(make_command(temp_path), step_name);
    verify_temp(temp_path);
    fs::rename(temp_path, output_path);
}
template <typename MakeCommand>
inline void regenerate_file_atomically(const fs::path& output_path, const std::string_view step_name, MakeCommand&& make_command) {
    regenerate_file_atomically(output_path, step_name, std::forward<MakeCommand>(make_command), [](const fs::path&) {});
}
inline void ensure_downloaded_weight(const fs::path& output_path, const WeightAsset& asset) {
    if (is_nonempty_regular_file(output_path) && md5_of_file(output_path) == asset.md5_hash) { return; }
    regenerate_file_atomically(
        output_path, "RF-DETR weight download",
        [&asset](const fs::path& temp_path) {
            return std::vector<std::string>{
                "curl", "-L", "--fail", "--silent", "--show-error", "--retry", "3", "--output", temp_path.string(), std::string(asset.download_url),
            };
        },
        [&output_path, &asset](const fs::path& temp_path) {
            const std::string actual_md5 = md5_of_file(temp_path);
            if (actual_md5 != asset.md5_hash) {
                remove_if_exists(temp_path);
                throw std::runtime_error("downloaded RF-DETR weight hash mismatch for " + output_path.string() + ": expected=" + std::string(asset.md5_hash) +
                                         " actual=" + actual_md5);
            }
        });
}
inline void ensure_native_checkpoint(const fs::path& upstream_weights_path, const fs::path& native_checkpoint_path) {
    if (is_nonempty_regular_file_newer_than(native_checkpoint_path, upstream_weights_path)) {
        try {
            const auto checkpoint = decode_model_state(native_checkpoint_path);
            if (is_native_checkpoint_file(native_checkpoint_path) && ResolvedClassLayout(checkpoint.metadata.class_layout).semantic()) return;
        } catch (const std::exception&) {}
    }
    regenerate_file_atomically(native_checkpoint_path, "RF-DETR native checkpoint export", [&upstream_weights_path](const fs::path& temp_path) {
        return std::vector<std::string>{
            mmltk::testsupport::mmltk_cli_path(), "rfdetr", "normalize-weights", "--input", upstream_weights_path.string(), "--output", temp_path.string(),
        };
    });
}
inline void ensure_exported_onnx(const fs::path& native_checkpoint_path, const fs::path& onnx_path) {
    if (is_nonempty_regular_file_newer_than(onnx_path, native_checkpoint_path) && validate_onnx_model(onnx_path)) { return; }
    regenerate_file_atomically(
        onnx_path, "RF-DETR ONNX export",
        [&native_checkpoint_path](const fs::path& temp_path) {
            return std::vector<std::string>{
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
            };
        },
        [](const fs::path& temp_path) {
            if (!validate_onnx_model(temp_path)) { throw std::runtime_error("RF-DETR ONNX export does not expose canonical prediction outputs"); }
        });
}
inline void ensure_built_tensorrt_engine(const fs::path& onnx_path, const fs::path& tensorrt_path) {
    if (is_nonempty_regular_file_newer_than(tensorrt_path, onnx_path) && validate_tensorrt_engine(tensorrt_path)) { return; }
    // The RF-DETR writer owns engine/companion publication as one checked bundle.
    run_checked(
        {mmltk::testsupport::mmltk_cli_path(), "rfdetr", "build-engine", "--onnx", onnx_path.string(), "--output", tensorrt_path.string(), "--device-id", "0"},
        "RF-DETR TensorRT build");
    if (!validate_tensorrt_engine(tensorrt_path)) throw std::runtime_error("invalid generated RF-DETR engine bundle");
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
    assets.onnx_metadata_validated = true;
    ensure_built_tensorrt_engine(assets.onnx_path, assets.tensorrt_path);
    return assets;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
