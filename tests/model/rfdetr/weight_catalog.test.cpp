#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/weight_catalog.h"
#include "rfdetr/train_recipe.h"
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
#include "asset_cache_support.h"
#include "checkpoint_fixture_support.h"
#endif

#include "support/catch2_compat.hpp"
#include "support/filesystem_test_utils.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
using mmltk::rfdetr::StateDictEntry;
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
using mmltk::rfdetr::testsupport::save_upstream_python_checkpoint;

void write_upstream_checkpoint(const std::filesystem::path& path) {
    save_upstream_python_checkpoint(path,
                                    {
                                        StateDictEntry{
                                            "query_feat.weight",
                                            torch::zeros({2600, 256}, torch::TensorOptions().dtype(torch::kFloat32)),
                                        },
                                        StateDictEntry{
                                            "class_embed.bias",
                                            torch::linspace(-1.0f,
                                                            1.0f,
                                                            91,
                                                            torch::TensorOptions().dtype(torch::kFloat32)),
                                        },
                                        StateDictEntry{
                                            "class_embed.weight",
                                            torch::ones({91, 256}, torch::TensorOptions().dtype(torch::kFloat32)),
                                        },
                                    });
}
#endif

void test_known_weight_assets() {
    assert(mmltk::rfdetr::weight_catalog().size() == 10);

    const auto* seg_medium = mmltk::rfdetr::find_weight_asset("rf-detr-seg-medium.pt");
    assert(seg_medium != nullptr);
    assert(seg_medium->filename == "rf-detr-seg-medium.pt");
    assert(seg_medium->download_url == "https://storage.googleapis.com/rfdetr/rf-detr-seg-m-ft.pth");
    assert(seg_medium->md5_hash == "a49af1562c3719227ad43d0ca53b4c7a");

    const auto* nano = mmltk::rfdetr::find_weight_asset("rf-detr-nano.pth");
    assert(nano != nullptr);
    assert(nano->download_url == "https://storage.googleapis.com/rfdetr/nano_coco/checkpoint_best_regular.pth");

    assert(mmltk::rfdetr::find_weight_asset("rf-detr-base.pth") == nullptr);
    assert(mmltk::rfdetr::find_weight_asset("rf-detr-base-o365.pth") == nullptr);
    assert(mmltk::rfdetr::find_weight_asset("rf-detr-base-2.pth") == nullptr);
    assert(mmltk::rfdetr::find_weight_asset("rf-detr-large.pth") == nullptr);
    assert(mmltk::rfdetr::find_weight_asset("rf-detr-seg-preview.pt") == nullptr);
}

void test_weight_lookup_helpers() {
    const auto resolved = mmltk::rfdetr::resolve_weight_asset_for_path("/tmp/models/rf-detr-seg-small.pt");
    if (!resolved.has_value()) {
        throw std::runtime_error("expected rf-detr-seg-small.pt to resolve to a registered weight asset");
    }
    const auto& resolved_asset = *resolved;
    assert(resolved_asset.filename == "rf-detr-seg-small.pt");
    assert(mmltk::rfdetr::is_registered_weight_asset("rf-detr-seg-small.pt"));
    assert(!mmltk::rfdetr::resolve_weight_asset_for_path("/tmp/models/rf-detr-large.pth").has_value());
    assert(!mmltk::rfdetr::is_registered_weight_asset("rf-detr-large.pth"));
    assert(!mmltk::rfdetr::is_registered_weight_asset("missing.pt"));
    assert(mmltk::rfdetr::find_weight_asset("missing.pt") == nullptr);
}

void test_model_presets() {
    assert(mmltk::rfdetr::model_presets().size() == 10);

    const auto* preset = mmltk::rfdetr::find_model_preset("rf-detr-seg-medium");
    assert(preset != nullptr);
    assert(preset->segmentation_head);
    assert(preset->resolution == 432);
    assert(preset->patch_size == 12);
    assert(preset->num_windows == 2);
    assert(preset->positional_encoding_size == 36);
    assert(preset->dec_layers == 5);
    assert(preset->num_queries == 200);
    assert(preset->num_select == 200);
    assert(preset->num_classes == 91);
    assert(preset->hidden_dim == 256);
    assert(preset->group_detr == 13);
    assert(preset->two_stage);
    assert(preset->canonical_weight_filename == "rf-detr-seg-medium.pt");

    const auto* by_weight =
        mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-large-2026.pth");
    assert(by_weight != nullptr);
    assert(by_weight->preset_name == "rf-detr-large");
    assert(by_weight->patch_size == 16);
    assert(by_weight->num_windows == 2);
    assert(by_weight->positional_encoding_size == 44);
    assert(by_weight->hidden_dim == 256);
    assert(!by_weight->segmentation_head);

    const auto* seg_nano = mmltk::rfdetr::find_model_preset("rf-detr-seg-nano");
    assert(seg_nano != nullptr);
    assert(seg_nano->patch_size == 12);
    assert(seg_nano->num_windows == 1);
    assert(seg_nano->positional_encoding_size == 26);

    const auto* seg_medium_upstream =
        mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-seg-m-ft.pth");
    assert(seg_medium_upstream != nullptr);
    assert(seg_medium_upstream->preset_name == "rf-detr-seg-medium");
    const auto* seg_medium_legacy =
        mmltk::rfdetr::infer_model_preset_from_path("/tmp/engines/output-seg-med/1train/checkpoint.pt");
    assert(seg_medium_legacy != nullptr);
    assert(seg_medium_legacy->preset_name == "rf-detr-seg-medium");

    assert(mmltk::rfdetr::find_model_preset("rf-detr-base") == nullptr);
    assert(mmltk::rfdetr::find_model_preset("rf-detr-base-o365") == nullptr);
    assert(mmltk::rfdetr::find_model_preset("rf-detr-base-2") == nullptr);
    assert(mmltk::rfdetr::find_model_preset("rf-detr-large-deprecated") == nullptr);
    assert(mmltk::rfdetr::find_model_preset("rf-detr-seg-preview") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-base.pth") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-base-o365.pth") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-base-2.pth") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-large.pth") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("rf-detr-seg-preview.pt") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);

    assert(mmltk::rfdetr::find_model_preset("unknown-preset") == nullptr);
    assert(mmltk::rfdetr::find_model_preset_by_weight_filename("unknown-file.pth") == nullptr);
}

#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
void test_weight_artifact_resolution() {
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "mmltk_rfdetr_weight_resolution";
    mmltk::testsupport::remove_path_recursively_best_effort(root);
    fs::create_directories(root);
    const auto cached_assets = mmltk::rfdetr::testsupport::ensure_cached_model_assets("rf-detr-nano");

    const auto resolved = mmltk::rfdetr::resolve_upstream_weight_artifacts(cached_assets.native_checkpoint_path);
    assert(resolved.config.preset_name == "rf-detr-nano");
    assert(resolved.input_kind == "native-pt");
    assert(resolved.input_path.filename() == "rf-detr-nano.native.pt");
    assert(resolved.config.resolution == 384);
    assert(!resolved.config.segmentation);
    assert(resolved.config.num_classes == 91);
    assert(resolved.config.num_queries == 300);
    assert(resolved.config.num_select == 300);
    assert(resolved.config.dec_layers == 2);
    assert(resolved.config.group_detr == 13);
    assert(resolved.config.two_stage);
    assert(resolved.artifact_root == cached_assets.root_dir);
    assert(resolved.onnx_path.empty());
    assert(resolved.tensorrt_path.empty());

    write_upstream_checkpoint(root / "rf-detr-seg-m-ft.pth");
    const auto upstream_named =
        mmltk::rfdetr::resolve_upstream_weight_artifacts(root / "rf-detr-seg-m-ft.pth");
    assert(upstream_named.config.preset_name == "rf-detr-seg-medium");
    assert(upstream_named.input_kind == "upstream-python");
    assert(upstream_named.input_path.filename() == "rf-detr-seg-m-ft.pth");

    fs::create_directories(root / "engines" / "output-seg-med" / "1train");
    write_upstream_checkpoint(root / "engines" / "output-seg-med" / "1train" / "checkpoint.pt");
    const auto legacy_named =
        mmltk::rfdetr::resolve_upstream_weight_artifacts(root / "engines" / "output-seg-med" / "1train" / "checkpoint.pt");
    assert(legacy_named.config.preset_name == "rf-detr-seg-medium");
    assert(legacy_named.input_kind == "upstream-python");
    assert(legacy_named.input_path.filename() == "checkpoint.pt");
    assert(
        mmltk::rfdetr::infer_train_recipe_preset_name_from_path(
            root / "engines" / "output-seg-med" / "1train" / "checkpoint.pt") ==
        "rf-detr-seg-medium");

    mmltk::rfdetr::ModelArtifactRequest onnx_request;
    onnx_request.onnx_path = cached_assets.onnx_path;
    const auto onnx_resolved = mmltk::rfdetr::resolve_model_artifacts(onnx_request);
    assert(onnx_resolved.input_kind == "onnx");
    assert(onnx_resolved.input_path.filename() == "inference_model.sim.onnx");
    assert(onnx_resolved.onnx_path.filename() == "inference_model.sim.onnx");
    assert(onnx_resolved.config.preset_name == "rf-detr-nano");

    mmltk::rfdetr::ModelArtifactRequest tensorrt_request;
    tensorrt_request.tensorrt_path = cached_assets.tensorrt_path;
    const auto tensorrt_resolved = mmltk::rfdetr::resolve_model_artifacts(tensorrt_request);
    assert(tensorrt_resolved.input_kind == "tensorrt");
    assert(tensorrt_resolved.input_path.filename() == "inference_model.engine");
    assert(tensorrt_resolved.tensorrt_path.filename() == "inference_model.engine");
    assert(tensorrt_resolved.config.preset_name == "rf-detr-nano");

    write_upstream_checkpoint(root / "renamed-seg-medium.pth");
    const auto renamed = mmltk::rfdetr::resolve_upstream_weight_artifacts(root / "renamed-seg-medium.pth");
    assert(renamed.config.preset_name == "rf-detr-seg-medium");

    write_upstream_checkpoint(root / "checkpoint-untyped.pth");
    bool threw = false;
    try {
        static_cast<void>(mmltk::rfdetr::resolve_upstream_weight_artifacts(root / "checkpoint-untyped.pth"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        mmltk::rfdetr::ModelArtifactRequest invalid_request;
        invalid_request.onnx_path = cached_assets.onnx_path;
        invalid_request.tensorrt_path = cached_assets.tensorrt_path;
        static_cast<void>(mmltk::rfdetr::resolve_model_artifacts(invalid_request));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    mmltk::testsupport::remove_path_recursively_best_effort(root);
}
#endif

} // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][weight_catalog]", test_known_weight_assets);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][weight_catalog]", test_weight_lookup_helpers);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][weight_catalog]", test_model_presets);
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][weight_catalog]", test_weight_artifact_resolution);
#endif
