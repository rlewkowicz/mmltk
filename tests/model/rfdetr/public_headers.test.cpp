#include "mmltk/model/artifacts.h"
#include "mmltk/rfdetr/module.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk/rfdetr/live_predict.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/validate.h"

#include "rfdetr/inference/backend_factory.h"

#include "support/filesystem_test_utils.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

#include "support/catch2_compat.hpp"

namespace {

namespace fs = std::filesystem;

void test_public_header_types_are_constructible() {
    using namespace mmltk::rfdetr;

    mmltk::model::ModelArtifactRequest generic_request;
    generic_request.onnx_path = "/tmp/model.onnx";

    TrainOptions train_options;
    train_options.batch_size = 2;
    train_options.optimizer = TrainOptimizerKind::Muon;
    train_options.momentum = 0.95;
    train_options.warmup_momentum = 0.8;
    train_options.output_dir = "build/dev/train";

    PredictOptions predict_options;
    predict_options.preset_name = "rf-detr-seg-medium";
    predict_options.source_kind = PredictSourceKind::ImageFiles;
    predict_options.batch_size = 4;
    predict_options.max_dets_per_image = 128;
    predict_options.image_inputs.push_back(PredictImageInput{
        "/tmp/input.png",
        "input.png",
        17,
    });

    ValidationOptions validation_options;
    validation_options.batch_size = 1;
    validation_options.eval_order = "onnx";

    LivePredictOptions live_options;
    live_options.preset_name = "rf-detr-seg-medium";
    live_options.source.device_path = "/dev/video3";
    live_options.source.width = 1280;
    live_options.source.height = 720;
    live_options.source.preview_buffer_count = 3;
    live_options.source.initial_region = LiveCaptureRegion{100, 50, 640, 360};
    live_options.split_count = 3;

    std::unique_ptr<mmltk::live::FrameAnalyzer> (*live_analyzer_factory)(const LivePredictOptions&) =
        &make_live_rfdetr_frame_analyzer;

    Prediction prediction;
    prediction.image_id = 7;
    prediction.category_id = 2;
    prediction.score = 0.75f;
    prediction.bbox_xyxy = {10.0f, 20.0f, 110.0f, 120.0f};

    std::vector<int> labels = {0, 0, 1};
    std::vector<std::uint8_t> colors;
    build_instance_colors_from_zero_based_labels(labels.data(), labels.size(), 91, &colors);

    const auto model_module = mmltk::rfdetr::make_model_module();

    PredictionRecord record;
    record.dataset_index = 3;
    record.image_id = prediction.image_id;
    record.source_name = "input.png";
    record.detections.push_back(prediction);

    ValidationBackendResult backend_result;
    backend_result.model_info.backend = "onnx";
    backend_result.model_info.model_path = "/tmp/model.onnx";
    backend_result.summary.bbox.ap = 0.5;

    ValidationRunResult validation_run;
    validation_run.backends.emplace("onnx", backend_result);

    assert(train_options.batch_size == 2);
    assert(generic_request.selected_input_count() == 1U);
    assert(train_options.optimizer == TrainOptimizerKind::Muon);
    assert(train_options.warmup_momentum == 0.8);
    assert(predict_options.max_dets_per_image == 128);
    assert(predict_options.preset_name == std::string("rf-detr-seg-medium"));
    assert(predict_options.image_inputs.front().image_id == 17);
    assert(validation_run.backends.at("onnx").summary.bbox.ap == 0.5);
    assert(record.detections.front().category_id == 2);
    assert(record.source_name == std::string("input.png"));
    assert(validation_options.eval_order == std::string("onnx"));
    assert(live_options.source.preview_buffer_count == 3);
    assert(live_options.source.initial_region.width == 640);
    assert(live_analyzer_factory != nullptr);
    assert(colors.size() == 9);
    assert(colors[0] != colors[3] || colors[1] != colors[4] || colors[2] != colors[5]);
    assert(model_module != nullptr);
    assert(model_module->module_id() == std::string_view("rfdetr"));
    assert(model_module->capabilities().supports_tensorrt);
    assert(model_module->infer_preset_from_artifact_path("/tmp/rf-detr-seg-medium.onnx") ==
           std::string("rf-detr-seg-medium"));
}

void test_backend_selection_helpers_choose_available_artifacts() {
    using namespace mmltk::rfdetr;

    ResolvedModelArtifacts weights_artifacts;
    weights_artifacts.input_path = "/tmp/model.pt";
    weights_artifacts.weights_path = "/tmp/model.pt";
    assert(choose_backend_name("auto", weights_artifacts) == std::string("weights"));

    bool weights_override_rejected = false;
    try {
        (void)choose_backend_name("onnx", weights_artifacts);
    } catch (const std::runtime_error& error) {
        weights_override_rejected = true;
        assert(std::string_view(error.what()).find("--backend is only valid") != std::string_view::npos);
    }
    assert(weights_override_rejected);

    ResolvedModelArtifacts backend_artifacts;
    backend_artifacts.input_path = "/tmp/model.onnx";
    backend_artifacts.onnx_path = "/tmp/model.onnx";
    backend_artifacts.tensorrt_path = "/tmp/model.engine";
    assert(choose_backend_name("auto", backend_artifacts) == std::string("tensorrt"));
    assert(choose_backend_name("onnx", backend_artifacts) == std::string("onnx"));
    assert(choose_backend_name("tensorrt", backend_artifacts) == std::string("tensorrt"));

    ResolvedModelArtifacts onnx_only_artifacts;
    onnx_only_artifacts.input_path = "/tmp/model.onnx";
    onnx_only_artifacts.onnx_path = "/tmp/model.onnx";
    assert(choose_backend_name("tensorrt", onnx_only_artifacts) == std::string("tensorrt"));

    bool unsupported_backend_rejected = false;
    try {
        (void)choose_backend_name("bogus", backend_artifacts);
    } catch (const std::runtime_error& error) {
        unsupported_backend_rejected = true;
        assert(std::string_view(error.what()).find("unsupported RF-DETR backend") != std::string_view::npos);
    }
    assert(unsupported_backend_rejected);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][public_headers]", test_public_header_types_are_constructible);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][public_headers]", test_backend_selection_helpers_choose_available_artifacts);
