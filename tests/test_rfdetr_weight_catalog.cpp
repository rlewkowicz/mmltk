#include "fastloader/rfdetr/model.h"
#include "fastloader/rfdetr/model_config.h"
#include "fastloader/rfdetr/weight_catalog.h"
#if FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER
#include "test_rfdetr_checkpoint_fixture_support.h"
#endif

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
using fastloader::rfdetr::StateDictEntry;
#if FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER
using fastloader::rfdetr::testsupport::save_upstream_python_checkpoint;

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
    assert(fastloader::rfdetr::weight_catalog().size() == 10);

    const auto* seg_medium = fastloader::rfdetr::find_weight_asset("rf-detr-seg-medium.pt");
    assert(seg_medium != nullptr);
    assert(seg_medium->filename == "rf-detr-seg-medium.pt");
    assert(seg_medium->download_url == "https://storage.googleapis.com/rfdetr/rf-detr-seg-m-ft.pth");
    assert(seg_medium->md5_hash == "a49af1562c3719227ad43d0ca53b4c7a");

    const auto* nano = fastloader::rfdetr::find_weight_asset("rf-detr-nano.pth");
    assert(nano != nullptr);
    assert(nano->download_url == "https://storage.googleapis.com/rfdetr/nano_coco/checkpoint_best_regular.pth");

    assert(fastloader::rfdetr::find_weight_asset("rf-detr-base.pth") == nullptr);
    assert(fastloader::rfdetr::find_weight_asset("rf-detr-base-o365.pth") == nullptr);
    assert(fastloader::rfdetr::find_weight_asset("rf-detr-base-2.pth") == nullptr);
    assert(fastloader::rfdetr::find_weight_asset("rf-detr-large.pth") == nullptr);
    assert(fastloader::rfdetr::find_weight_asset("rf-detr-seg-preview.pt") == nullptr);
}

void test_weight_lookup_helpers() {
    const auto resolved = fastloader::rfdetr::resolve_weight_asset_for_path("/tmp/models/rf-detr-seg-small.pt");
    assert(resolved.has_value());
    assert(resolved->filename == "rf-detr-seg-small.pt");
    assert(fastloader::rfdetr::is_registered_weight_asset("rf-detr-seg-small.pt"));
    assert(!fastloader::rfdetr::resolve_weight_asset_for_path("/tmp/models/rf-detr-large.pth").has_value());
    assert(!fastloader::rfdetr::is_registered_weight_asset("rf-detr-large.pth"));
    assert(!fastloader::rfdetr::is_registered_weight_asset("missing.pt"));
    assert(fastloader::rfdetr::find_weight_asset("missing.pt") == nullptr);
}

void test_model_presets() {
    assert(fastloader::rfdetr::model_presets().size() == 10);

    const auto* preset = fastloader::rfdetr::find_model_preset("rf-detr-seg-medium");
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
        fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-large-2026.pth");
    assert(by_weight != nullptr);
    assert(by_weight->preset_name == "rf-detr-large");
    assert(by_weight->patch_size == 16);
    assert(by_weight->num_windows == 2);
    assert(by_weight->positional_encoding_size == 44);
    assert(by_weight->hidden_dim == 256);
    assert(!by_weight->segmentation_head);

    const auto* seg_nano = fastloader::rfdetr::find_model_preset("rf-detr-seg-nano");
    assert(seg_nano != nullptr);
    assert(seg_nano->patch_size == 12);
    assert(seg_nano->num_windows == 1);
    assert(seg_nano->positional_encoding_size == 26);

    const auto* seg_medium_upstream =
        fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-seg-m-ft.pth");
    assert(seg_medium_upstream != nullptr);
    assert(seg_medium_upstream->preset_name == "rf-detr-seg-medium");

    assert(fastloader::rfdetr::find_model_preset("rf-detr-base") == nullptr);
    assert(fastloader::rfdetr::find_model_preset("rf-detr-base-o365") == nullptr);
    assert(fastloader::rfdetr::find_model_preset("rf-detr-base-2") == nullptr);
    assert(fastloader::rfdetr::find_model_preset("rf-detr-large-deprecated") == nullptr);
    assert(fastloader::rfdetr::find_model_preset("rf-detr-seg-preview") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-base.pth") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-base-o365.pth") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-base-2.pth") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-large.pth") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("rf-detr-seg-preview.pt") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("checkpoint_best_regular.pth") == nullptr);

    assert(fastloader::rfdetr::find_model_preset("unknown-preset") == nullptr);
    assert(fastloader::rfdetr::find_model_preset_by_weight_filename("unknown-file.pth") == nullptr);
}

#if FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER
void test_weight_artifact_resolution() {
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "fastloader_rfdetr_weight_resolution";
    fs::remove_all(root);
    fs::create_directories(root);
    write_upstream_checkpoint(root / "rf-detr-seg-medium.pt");

    const auto resolved =
        fastloader::rfdetr::resolve_upstream_weight_artifacts(root / "rf-detr-seg-medium.pt");
    assert(resolved.config.preset_name == "rf-detr-seg-medium");
    assert(resolved.input_kind == "upstream-python");
    assert(resolved.input_path.filename() == "rf-detr-seg-medium.pt");
    assert(resolved.config.resolution == 432);
    assert(resolved.config.segmentation);
    assert(resolved.config.num_classes == 91);
    assert(resolved.config.num_queries == 200);
    assert(resolved.config.num_select == 200);
    assert(resolved.config.dec_layers == 5);
    assert(resolved.config.group_detr == 13);
    assert(resolved.config.two_stage);
    assert(resolved.artifact_root == root);
    assert(resolved.onnx_path.empty());
    assert(resolved.tensorrt_path.empty());

    write_upstream_checkpoint(root / "rf-detr-seg-m-ft.pth");
    const auto upstream_named =
        fastloader::rfdetr::resolve_upstream_weight_artifacts(root / "rf-detr-seg-m-ft.pth");
    assert(upstream_named.config.preset_name == "rf-detr-seg-medium");
    assert(upstream_named.input_kind == "upstream-python");
    assert(upstream_named.input_path.filename() == "rf-detr-seg-m-ft.pth");

    fastloader::rfdetr::ModelArtifactRequest onnx_request;
    fs::create_directories(root / "engines" / "output-seg-medium");
    std::ofstream(root / "engines" / "output-seg-medium" / "inference_model.sim.onnx").put('\n');
    onnx_request.onnx_path = root / "engines" / "output-seg-medium" / "inference_model.sim.onnx";
    const auto onnx_resolved = fastloader::rfdetr::resolve_model_artifacts(onnx_request);
    assert(onnx_resolved.input_kind == "onnx");
    assert(onnx_resolved.input_path.filename() == "inference_model.sim.onnx");
    assert(onnx_resolved.onnx_path.filename() == "inference_model.sim.onnx");
    assert(onnx_resolved.config.preset_name == "rf-detr-seg-medium");

    fastloader::rfdetr::ModelArtifactRequest tensorrt_request;
    std::ofstream(root / "engines" / "output-seg-medium" / "inference_model.engine").put('\n');
    tensorrt_request.tensorrt_path = root / "engines" / "output-seg-medium" / "inference_model.engine";
    const auto tensorrt_resolved = fastloader::rfdetr::resolve_model_artifacts(tensorrt_request);
    assert(tensorrt_resolved.input_kind == "tensorrt");
    assert(tensorrt_resolved.input_path.filename() == "inference_model.engine");
    assert(tensorrt_resolved.tensorrt_path.filename() == "inference_model.engine");
    assert(tensorrt_resolved.config.preset_name == "rf-detr-seg-medium");

    write_upstream_checkpoint(root / "renamed-seg-medium.pth");
    bool threw = false;
    try {
        static_cast<void>(fastloader::rfdetr::resolve_upstream_weight_artifacts(root / "renamed-seg-medium.pth"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        fastloader::rfdetr::ModelArtifactRequest invalid_request;
        invalid_request.onnx_path = root / "engines" / "output-seg-medium" / "inference_model.sim.onnx";
        invalid_request.tensorrt_path = root / "engines" / "output-seg-medium" / "inference_model.engine";
        static_cast<void>(fastloader::rfdetr::resolve_model_artifacts(invalid_request));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    fs::remove_all(root);
}
#endif

} // namespace

int main() {
    test_known_weight_assets();
    test_weight_lookup_helpers();
    test_model_presets();
#if FASTLOADER_RFDETR_PYTHON_CHECKPOINT_LOADER
    test_weight_artifact_resolution();
#endif
    return 0;
}
