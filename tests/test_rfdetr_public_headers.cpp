#include "fastloader/rfdetr/draw_cuda.h"
#include "fastloader/rfdetr/live_predict.h"
#include "fastloader/rfdetr/predict.h"
#include "fastloader/rfdetr/train.h"
#include "fastloader/rfdetr/validate.h"

#include <cassert>
#include <string>

int main() {
    using namespace fastloader::rfdetr;

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

    LivePredictStatus live_status;
    live_status.active_split_count = live_options.split_count;
    live_status.last_prediction.frame_id = 42;
    live_status.last_prediction.splits.push_back(LiveSplitPrediction{
        0,
        LiveCaptureRegion{100, 50, 213, 360},
        {},
    });

    LivePreviewFormatInfo preview_format;
    preview_format.width = 1280;
    preview_format.height = 720;
    preview_format.bytes_per_line = 3840;

    LivePreviewFrame preview_frame;
    preview_frame.buffer_index = 2;
    preview_frame.frame_id = 77;
    preview_frame.buffer.width_px = 640;
    preview_frame.buffer.height_px = 360;
    preview_frame.buffer.pitch_bytes = 1920;

    Prediction prediction;
    prediction.image_id = 7;
    prediction.category_id = 2;
    prediction.score = 0.75f;
    prediction.bbox_xyxy = {10.0f, 20.0f, 110.0f, 120.0f};

    std::vector<int> labels = {0, 0, 1};
    std::vector<std::uint8_t> colors;
    build_instance_colors_from_zero_based_labels(labels.data(), labels.size(), 91, &colors);

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
    assert(live_status.last_prediction.frame_id == 42);
    assert(live_status.last_prediction.splits.front().source_region.x == 100);
    assert(preview_format.bytes_per_line == 3840);
    assert(preview_frame.buffer.height_px == 360);
    assert(colors.size() == 9);
    assert(colors[0] != colors[3] || colors[1] != colors[4] || colors[2] != colors[5]);
    return 0;
}
