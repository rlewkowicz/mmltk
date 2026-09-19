#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include "src/backend/models/rfdetr/inference/prediction_raw_preparation.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"
// RF-DETR inference JSON coverage.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
import mmltk.backend.models.rfdetr.inference.prediction;
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
    prediction.class_reference = 2;
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
    result.artifacts.class_layout = unresolved_class_layout(91);
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
    REQUIRE((stream.is_open()));
    const json payload = json::parse(stream);
    REQUIRE((payload.at("source_kind") == "image_files"));
    REQUIRE((payload.at("input_image_count") == 1));
    REQUIRE((!payload.contains("compiled_path")));
    REQUIRE((payload.at("records").at(0).at("source_name") == "camera0/frame-000001.png"));
    REQUIRE((payload.at("records").at(0).at("detections").at(0).at("label") == "2"));
    REQUIRE((payload.at("records").at(0).at("detections").at(0).at("mask_rle") == "1:2 5:1"));
    result.class_catalog = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"first", "middle", "last"});
    result.class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::Foreground;
    result.artifacts.class_layout = native_training_class_layout(*result.class_catalog);
    record.detections.front().class_reference = 0;
    record.detections.push_back(prediction);
    {
        PredictionJsonWriter named_writer(options);
        named_writer.Begin(result);
        named_writer.Append(record);
        named_writer.Complete();
    }
    std::ifstream named_stream(output_path);
    const json named = json::parse(named_stream);
    REQUIRE((named.at("records").at(0).at("detections").at(0).at("label") == "first"));
    REQUIRE((named.at("records").at(0).at("detections").at(1).at("label") == "last"));
    std::remove(output_path.c_str());
}
}  // namespace
TEST_CASE("test_prediction_json_writer_emits_expected_payload", "[model][rfdetr][prediction_json]") { test_prediction_json_writer_emits_expected_payload(); }
TEST_CASE("optional raw failure preserves semantic mask JSON and the next frame", "[model][rfdetr][prediction_json]") {
    using namespace mmltk::backend::models::rfdetr;
    namespace runtime = mmltk::backend::ml::runtime;
    const auto path = fs::temp_directory_path() / ("prediction-raw-failure-" + std::to_string(::getpid()) + ".json");
    PredictRequest request;
    request.output_path = path;
    PredictionJsonWriter writer(request);
    PredictionRunResult result;
    result.artifacts.class_layout = unresolved_class_layout(1U);
    writer.Begin(result);
    PredictionRecord record{.image_id = 9};
    record.detections.push_back({.class_reference = 0, .score = .9F, .has_mask = true});
    encode_mask_values_into(2U, 2U, record.detections.front().mask, [](auto) { return true; });
    runtime::AnalysisAnnotationStorage annotation;
    std::string failure;
    static std::size_t settlements;
    settlements = 0U;
    const auto settle = +[](cudaStream_t) {
        ++settlements;
        return cudaSuccess;
    };
    for (int stage = 0; stage < 3; ++stage) {
        annotation.source_region = {.width = 2U, .height = 2U};
        annotation.value_capacity = 1U;
        annotation.count.SetKnown(1U, annotation.value_capacity);
        annotation.boxes_xyxy.address = 11U;
        annotation.class_references.address = 12U;
        annotation.confidences.address = 13U;
        annotation.masks.address = 14U;
        failure.clear();
        PredictionRawPreparation raw(true, annotation, failure, nullptr, settle);
        raw.Execute([&] {
            if (stage == 0) throw std::bad_alloc{};
            if (stage == 1) throw std::runtime_error("compact selected scalars");
            throw std::runtime_error("copy dense selected masks");
        });
        CHECK_FALSE(raw.available());
        CHECK_FALSE(failure.empty());
        CHECK(annotation.count.value() == 0U);
        CHECK(annotation.boxes_xyxy.address == 0U);
        CHECK(annotation.class_references.address == 0U);
        CHECK(annotation.confidences.address == 0U);
        CHECK(annotation.masks.address == 0U);
        CHECK(annotation.source_region.width == 2U);
        raw.Execute([] { FAIL("a failed current preview cannot submit more work"); });
        writer.Append(record);
    }
    CHECK(settlements == 3U);
    failure.clear();
    PredictionRawPreparation healthy(true, annotation, failure, nullptr, settle);
    healthy.Execute([&] {
        annotation.value_capacity = 1U;
        annotation.count.SetKnown(1U, annotation.value_capacity);
        annotation.masks.address = 99U;
    });
    CHECK(healthy.available());
    CHECK(failure.empty());
    CHECK(annotation.masks.address == 99U);
    CHECK(settlements == 3U);
    writer.Append(record);
    writer.Complete();
    std::ifstream file(path);
    const auto output = json::parse(file);
    REQUIRE(output.at("records").size() == 4U);
    for (const auto& saved : output.at("records")) CHECK(saved.at("detections").at(0).at("mask_rle") == "0:4");
    fs::remove(path);
}
