#include <unistd.h>

#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
// RF-DETR inference JSON coverage.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "catch2_compat.hpp"

import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

void test_prediction_json_writer_emits_expected_payload() {
    using namespace mmltk::backend::models::rfdetr;

    const fs::path output_path = fs::temp_directory_path() / ("mmltk_rfdetr_prediction_json_" + std::to_string(::getpid()) + ".json");

    PredictRequest options;
    options.source_kind = PredictSourceKind::ImageFiles;
    options.output_path = output_path;
    options.weights_path = "/tmp/model.pt";
    options.image_inputs.push_back(PredictImageInput{
        "/tmp/frame-000001.png",
        "camera0/frame-000001.png",
        42,
    });

    Prediction prediction;
    prediction.image_id = 42;
    prediction.category_id = 3;
    prediction.score = 0.9f;
    prediction.bbox_xyxy = {1.0f, 2.0f, 3.0f, 4.0f};
    prediction.has_mask = true;
    encode_mask_values_into(2U, 3U, prediction.mask, [](std::uint32_t index) { return index == 1U || index == 2U || index == 5U; });

    PredictionRecord record;
    record.dataset_index = 0;
    record.image_id = 42;
    record.source_name = "camera0/frame-000001.png";
    record.detections.push_back(prediction);

    PredictionRunResult result;
    result.backend_name = "weights";
    result.artifacts.input_kind = "weights";
    result.artifacts.input_path = "/tmp/model.pt";
    result.artifacts.weights_path = "/tmp/model.pt";
    result.artifacts.config.preset_name = "rf-detr-seg-medium";
    PredictionJsonWriter writer(options);
    writer.Begin(result);
    writer.Append(record);
    writer.Complete();

    std::ifstream stream(output_path);
    MMLTK_ASSERT(stream.is_open());
    const json payload = json::parse(stream);
    MMLTK_ASSERT(payload.at("source_kind") == "image_files");
    MMLTK_ASSERT(payload.at("input_image_count") == 1);
    MMLTK_ASSERT(!payload.contains("compiled_path"));
    MMLTK_ASSERT(payload.at("records").at(0).at("source_name") == "camera0/frame-000001.png");
    MMLTK_ASSERT(payload.at("records").at(0).at("detections").at(0).at("label") == "3");

    MMLTK_ASSERT(payload.at("records").at(0).at("detections").at(0).at("mask_rle") == "1:2 5:1");
    result.class_names = {"first", "middle", "last"};
    record.detections.front().category_id = 1;
    record.detections.push_back(prediction);
    {
        PredictionJsonWriter named_writer(options);
        named_writer.Begin(result);
        named_writer.Append(record);
        named_writer.Complete();
    }
    std::ifstream named_stream(output_path);
    const json named = json::parse(named_stream);
    MMLTK_ASSERT(named.at("records").at(0).at("detections").at(0).at("label") == "first");
    MMLTK_ASSERT(named.at("records").at(0).at("detections").at(1).at("label") == "last");
    std::remove(output_path.c_str());
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][prediction_json]", test_prediction_json_writer_emits_expected_payload);
