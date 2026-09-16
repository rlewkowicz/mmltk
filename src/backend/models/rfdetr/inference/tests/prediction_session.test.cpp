#include "src/backend/models/rfdetr/inference/prediction_delivery.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include <onnx/onnx_pb.h>
#include <cuda_runtime_api.h>
#include <nlohmann/json.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/models/rfdetr/inference/dataset_batch_lease.h"
#include <array>
#include <algorithm>
#include <span>
#include <vector>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "src/backend/models/rfdetr/inference/inference_preprocessor.h"
#include "src/backend/models/rfdetr/inference/prediction_capacity.h"
#include "src/backend/models/rfdetr/inference/prediction_raw_preparation.h"
#include <catch2/catch_test_macros.hpp>
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.model_export;
namespace rfdetr = mmltk::backend::models::rfdetr;
namespace tensor = mmltk::backend::ml::torch_api;
TEST_CASE("compiled preprocessing preserves unit-range float CHW channels", "[model][rfdetr][prediction][gpu]") {
    const auto source = tensor::tensor({0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 0.125F}).to(tensor::kCUDA);
    const mmltk::backend::data::Batch batch{.num_images = 1U, .device_images = source.data_ptr<float>()};
    rfdetr::InferenceBatchPreprocessor preprocessor(1, 1, 2, 0, tensor::kFloat);
    const auto result = preprocessor.Run(batch).reshape({6}).to(tensor::kCPU);
    REQUIRE(tensor::equal(result, source.to(tensor::kCPU)));
    const auto address = preprocessor.Run(batch).data_ptr();
    REQUIRE(preprocessor.Run(batch).data_ptr() == address);
    CHECK(address == batch.device_images);
    rfdetr::InferenceBatchPreprocessor half_precision(1, 1, 2, 0, tensor::kHalf);
    REQUIRE(tensor::equal(half_precision.Run(batch).reshape({6}).to(tensor::kFloat).to(tensor::kCPU), source.to(tensor::kCPU)));
    const auto half_address = half_precision.Run(batch).data_ptr();
    CHECK(half_address != batch.device_images);
    CHECK(half_precision.Run(batch).data_ptr() == half_address);
    auto invalid = batch;
    invalid.num_images = 2U;
    CHECK_THROWS_AS(preprocessor.Run(invalid), std::invalid_argument);
    invalid.num_images = 1U;
    invalid.device_images = nullptr;
    CHECK_THROWS_AS(preprocessor.Run(invalid), std::invalid_argument);
}
TEST_CASE("unfinished prediction output preserves the completed file", "[model][rfdetr][prediction_json]") {
    const auto output = std::filesystem::temp_directory_path() / ("prediction-cancel-" + std::to_string(::getpid()) + ".json");
    {
        std::ofstream original(output);
        original << "previous";
    }
    rfdetr::PredictRequest request;
    request.output_path = output;
    {
        rfdetr::PredictionJsonWriter writer(request);
        writer.Begin({.artifacts = {.class_layout = rfdetr::unresolved_class_layout(2)}});
        writer.Append({.image_id = 7});
    }
    std::ifstream preserved(output);
    std::string content;
    preserved >> content;
    REQUIRE(content == "previous");
    const auto prefix = output.filename().string() + ".tmp.";
    for (const auto& entry : std::filesystem::directory_iterator(output.parent_path())) REQUIRE_FALSE(entry.path().filename().string().starts_with(prefix));
    std::filesystem::remove(output);
}
TEST_CASE("cancelled prediction does not bind an artifact or deliver records", "[model][rfdetr][prediction]") {
    rfdetr::PredictRequest request;
    request.source_kind = rfdetr::PredictSourceKind::ImageFiles;
    request.image_inputs.push_back({"/not-opened.png", "frame", 9});
    request.weights_path = "/not-opened.pt";
    request.output_path = "/not-written.json";
    std::stop_source stop;
    stop.request_stop();
    rfdetr::PredictionSession session;
    bool delivered = false;
    const auto result = session.RunResolved(request, {}, {.native_handle = 1U, .valid = true},
                                            {
                                                .stop = stop.get_token(),
                                                .completed = [&](const auto&, auto, const auto&) { delivered = true; },
                                            });
    CHECK(result.cancelled);
    CHECK(result.processed_images == 0U);
    CHECK_FALSE(delivered);
}
namespace {
void write_prediction_model(const std::filesystem::path& path, std::int64_t queries = 2, bool include_masks = true,
                            std::optional<rfdetr::ModelClassLayout> layout = {}) {
    namespace onnx = mmltk_onnx;
    onnx::ModelProto model;
    model.set_ir_version(8);
    model.add_opset_import()->set_version(13);
    if (layout) {
        auto* metadata = model.add_metadata_props();
        metadata->set_key("mmltk.rfdetr.class_layout");
        metadata->set_value(rfdetr::encode_class_layout(*layout));
    }
    auto* graph = model.mutable_graph();
    graph->set_name("prediction-delivery");
    const auto value = [](onnx::ValueInfoProto* destination, const std::string& name, std::span<const std::int64_t> dimensions) {
        destination->set_name(name);
        auto* type = destination->mutable_type()->mutable_tensor_type();
        type->set_elem_type(onnx::TensorProto::FLOAT);
        for (auto extent : dimensions) type->mutable_shape()->add_dim()->set_dim_value(extent);
    };
    value(graph->add_input(), "images", std::array<std::int64_t, 4>{1, 3, 8, 8});
    auto* mean = graph->add_node();
    mean->set_op_type("ReduceMean");
    mean->add_input("images");
    mean->add_output("mean");
    auto* keepdims = mean->add_attribute();
    keepdims->set_name("keepdims");
    keepdims->set_type(onnx::AttributeProto::INT);
    keepdims->set_i(0);
    auto* zero = graph->add_initializer();
    zero->set_name("zero");
    zero->set_data_type(onnx::TensorProto::FLOAT);
    zero->add_float_data(0.0F);
    auto* multiply = graph->add_node();
    multiply->set_op_type("Mul");
    multiply->add_input("mean");
    multiply->add_input("zero");
    multiply->add_output("offset");
    const auto output = [&](const std::string& name, std::span<const std::int64_t> dimensions, std::span<const float> values) {
        value(graph->add_output(), name, dimensions);
        auto* constants = graph->add_initializer();
        constants->set_name(name + "_values");
        constants->set_data_type(onnx::TensorProto::FLOAT);
        for (auto extent : dimensions) constants->add_dims(extent);
        for (auto scalar : values) constants->add_float_data(scalar);
        auto* add = graph->add_node();
        add->set_op_type("Add");
        add->add_input(constants->name());
        add->add_input("offset");
        add->add_output(name);
    };
    std::vector<float> logits(queries * 2, -10.F), boxes(queries * 4, .5F), masks(queries * 4, -1.F);
    logits[0] = 10.F;
    logits[3] = 9.F;
    boxes[2] = boxes[3] = 1.F;
    std::fill_n(masks.begin(), 4, 1.F);
    output("pred_logits", std::array<std::int64_t, 3>{1, queries, 2}, logits);
    output("pred_boxes", std::array<std::int64_t, 3>{1, queries, 4}, boxes);
    if (include_masks) output("pred_masks", std::array<std::int64_t, 4>{1, queries, 2, 2}, masks);
    std::ofstream file(path, std::ios::binary);
    REQUIRE(model.SerializeToOstream(&file));
}
}  // namespace
TEST_CASE("prediction delivers bounded ordered images masks and receiver-owned pixels", "[model][rfdetr][prediction][gpu]") {
    const auto root = std::filesystem::temp_directory_path() / ("prediction-delivery-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    const mmltk::testsupport::ScopedTestCleanup files{[&] { std::filesystem::remove_all(root); }};
    write_prediction_model(root / "rf-detr-nano.onnx");
    const auto image = root / "red.ppm";
    {
        std::ofstream file(image, std::ios::binary);
        file << "P6\n2 2\n255\n";
        for (int pixel = 0; pixel < 4; ++pixel) {
            file.put(static_cast<char>(255));
            file.put(0);
            file.put(0);
        }
    }
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release_stream{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    rfdetr::PredictRequest request;
    request.source_kind = rfdetr::PredictSourceKind::ImageFiles;
    request.onnx_path = root / "rf-detr-nano.onnx";
    request.output_path = root / "completed.json";
    request.resolution = 8;
    request.allow_fp16 = false;
    request.include_masks = true;
    request.max_dets_per_image = 2;
    request.limit_images = 2;
    for (int identity : {21, 22, 23}) request.image_inputs.push_back({image, "red", identity});
    rfdetr::PredictionSession session;
    const mmltk::backend::ml::runtime::BorrowedCommandStream command{reinterpret_cast<std::uintptr_t>(stream), true};
    std::vector<std::int64_t> identities;
    std::shared_ptr<void> retained_owner;
    const std::uint8_t* retained = nullptr;
    auto observe = [&](const rfdetr::PredictionRecord& record, rfdetr::PredictionPixels pixels, const auto& annotations) {
        identities.push_back(record.image_id);
        REQUIRE(record.detections.size() == 2U);
        CHECK(record.detections[0].class_reference == 0);
        CHECK(record.detections[1].class_reference == 1);
        CHECK(record.detections[0].has_mask);
        CHECK(record.detections[0].mask.area == 4U);
        CHECK(record.detections[1].mask.area == 0U);
        CHECK(annotations.masks.address != 0U);
        CHECK(annotations.masks_available);
        CHECK(pixels.width == 2U);
        CHECK(pixels.height == 2U);
        CHECK(pixels.chw == nullptr);
        REQUIRE(pixels.rgb8 != nullptr);
        REQUIRE(pixels.custody);
        if (!retained) {
            retained_owner = pixels.custody;
            retained = pixels.rgb8;
        }
    };
    REQUIRE(session.RunAndWrite(request, command, {.source_pixels = true, .completed = observe}).processed_images == 2U);
    REQUIRE(identities == std::vector<std::int64_t>{21, 22});
    REQUIRE(retained != nullptr);
    CHECK(retained[0] == 255U);
    CHECK(retained[1] == 0U);
    std::ifstream completed(request.output_path);
    const auto original = nlohmann::json::parse(completed);
    REQUIRE(original.at("records").size() == 2U);
    CHECK(original.at("records").at(0).at("detections").at(0).at("label") == "0");
    identities.clear();
    std::stop_source stop;
    CHECK(session
              .RunAndWrite(request, command,
                           {.stop = stop.get_token(),
                            .source_pixels = true,
                            .completed =
                                [&](const auto& record, auto source, const auto& annotations) {
                                    observe(record, source, annotations);
                                    stop.request_stop();
                                }})
              .processed_images == 1U);
    CHECK(identities == std::vector<std::int64_t>{21});
    std::ifstream preserved(request.output_path);
    CHECK(nlohmann::json::parse(preserved) == original);
    const auto already_stopped = session.RunAndWrite(request, command,
                                                     {.stop = stop.get_token(),
                                                      .begin = [](const auto&) { FAIL("cancelled prediction began execution"); },
                                                      .completed = [](const auto&, auto, const auto&) { FAIL("cancelled prediction delivered a record"); },
                                                      .progress = [](auto, auto) { FAIL("cancelled prediction advanced progress"); }});
    CHECK(already_stopped.cancelled);
    CHECK(already_stopped.processed_images == 0U);
    std::ifstream still_preserved(request.output_path);
    CHECK(nlohmann::json::parse(still_preserved) == original);
    request.include_masks = false;
    request.limit_images = 1;
    const auto without_masks = session.Run(request, command, {.completed = [](const auto& record, auto source, const auto& annotations) {
                                               REQUIRE_FALSE(record.detections.empty());
                                               CHECK_FALSE(record.detections.front().has_mask);
                                               CHECK(annotations.value_count == 0U);
                                               CHECK(annotations.boxes_xyxy.address == 0U);
                                               CHECK(annotations.class_references.address == 0U);
                                               CHECK(annotations.confidences.address == 0U);
                                               CHECK(annotations.masks.address == 0U);
                                               CHECK(source.chw == nullptr);
                                           }});
    CHECK(without_masks.processed_images == 1U);
    CHECK_FALSE(without_masks.cancelled);
    request.limit_images = 3U;
    std::size_t selected_count = 0U;
    const auto selective =
        session.Run(request, command,
                    {
                        .demand =
                            [](std::int64_t index) {
                                return rfdetr::PredictionDemand{.source_pixels = index == 1, .encoded_masks = index == 1, .preview_masks = index == 1};
                            },
                        .completed =
                            [&](const auto& record, auto pixels, const auto& annotations) {
                                const bool selected = record.dataset_index == 1;
                                CHECK(record.detections.front().has_mask == selected);
                                CHECK((pixels.rgb8 != nullptr) == selected);
                                CHECK(annotations.masks_available == selected);
                                selected_count += selected ? 1U : 0U;
                            },
                    });
    CHECK(selective.processed_images == 3U);
    CHECK(selected_count == 1U);
    for (const bool encoded : {false, true})
        for (const bool gpu_masks : {false, true}) {
            request.limit_images = 1U;
            const auto mode = session.Run(
                request, command,
                {
                    .demand = [=](auto) { return rfdetr::PredictionDemand{.source_pixels = true, .encoded_masks = encoded, .preview_masks = gpu_masks}; },
                    .completed =
                        [&](const auto& record, auto pixels, const auto& annotations) {
                            REQUIRE(pixels.rgb8);
                            REQUIRE_FALSE(record.detections.empty());
                            CHECK(record.detections.front().has_mask == encoded);
                            CHECK(record.detections.front().mask.runs.empty() == !encoded);
                            CHECK(annotations.masks_available == gpu_masks);
                            CHECK((annotations.masks.address != 0U) == gpu_masks);
                        },
                });
            CHECK(mode.processed_images == 1U);
        }
    request.limit_images = 1U;
    std::size_t preview_refusals = 0U;
    const auto preview_limited = session.RunAndWrite(request, command,
                                                     {
                                                         .source_pixels = true,
                                                         .maximum_pixel_width = 1U,
                                                         .maximum_pixel_height = 1U,
                                                         .completed =
                                                             [&](const auto& record, auto source, const auto&) {
                                                                 CHECK(source.chw == nullptr);
                                                                 CHECK(record.detections.size() == 2U);
                                                                 ++preview_refusals;
                                                             },
                                                     });
    CHECK(preview_refusals == 1U);
    CHECK(preview_limited.processed_images == 1U);
    CHECK_FALSE(preview_limited.cancelled);
    std::ifstream semantic_output(request.output_path);
    CHECK(nlohmann::json::parse(semantic_output).at("records").size() == 1U);
    const auto oversized = root / "oversized.ppm";
    {
        std::ofstream file(oversized, std::ios::binary);
        file << "P6\n3 2\n255\n";
        for (int byte = 0; byte < 18; ++byte) file.put(0);
    }
    request.limit_images = 0U;
    for (bool reverse : {false, true}) {
        request.image_inputs = {{image, "small", 11}, {oversized, "large", 12}};
        if (reverse) std::reverse(request.image_inputs.begin(), request.image_inputs.end());
        std::vector<std::int64_t> delivered;
        const auto mixed = session.RunAndWrite(
            request, command,
            {.source_pixels = true, .maximum_pixel_width = 2U, .maximum_pixel_height = 2U, .completed = [&](const auto& record, auto current, const auto&) {
                 delivered.push_back(record.image_id);
                 if (record.image_id == 11) {
                     REQUIRE(current.rgb8);
                     CHECK(current.rgb8[0] == 255U);
                     CHECK(current.width == 2U);
                 } else {
                     CHECK(current.chw == nullptr);
                     CHECK(current.rgb8 == nullptr);
                     CHECK(current.width == 0U);
                     CHECK_FALSE(current.preview_failure.empty());
                 }
             }});
        CHECK(mixed.processed_images == 2U);
        CHECK(delivered == (reverse ? std::vector<std::int64_t>{12, 11} : std::vector<std::int64_t>{11, 12}));
    }
    request.include_masks = true;
    request.threshold = 0.9999F;
    const auto mask_only_images = session
                                      .Run(request, command,
                                           {.completed =
                                                [](const auto& record, auto, const auto& annotations) {
                                                    CHECK(record.detections.size() == 1U);
                                                    CHECK(annotations.value_count == 0U);
                                                    CHECK(annotations.boxes_xyxy.address == 0U);
                                                    CHECK(record.detections.front().class_reference == 0);
                                                    CHECK(record.detections.front().has_mask);
                                                    CHECK(record.detections.front().mask.area > 0U);
                                                }})
                                      .processed_images;
    CHECK(mask_only_images == 2U);
    const auto preview_images = session
                                    .Run(request, command,
                                         {.source_pixels = true,
                                          .completed =
                                              [&](const auto& record, auto pixels, const auto& annotations) {
                                                  REQUIRE(record.detections.size() == 1U);
                                                  CHECK(annotations.value_count == 1U);
                                                  REQUIRE(annotations.class_references.address != 0U);
                                                  REQUIRE(pixels.custody);
                                                  std::int32_t category = -1;
                                                  REQUIRE(cudaMemcpyAsync(&category, reinterpret_cast<const void*>(annotations.class_references.address),
                                                                          sizeof(category), cudaMemcpyDeviceToHost, stream) == cudaSuccess);
                                                  REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
                                                  CHECK(category == record.detections.front().class_reference);
                                              }})
                                    .processed_images;
    CHECK(preview_images == 2U);
    CHECK_FALSE(session.HasUnsafeCustody());
    rfdetr::PredictionSession context_poisoned;
    CHECK_THROWS_AS(
        context_poisoned.Run(request, command, {.completed = [](const auto&, auto, const auto&) { throw mmltk::frameworks::gpu::CudaContextFailure(true); }}),
        mmltk::backend::ml::runtime::CudaOperationError);
    CHECK(context_poisoned.HasUnsafeCustody());
    bool context_rebound = false;
    CHECK_THROWS_AS(context_poisoned.Run(request, command, {.begin = [&](const auto&) { context_rebound = true; }}),
                    mmltk::backend::ml::runtime::CudaOperationError);
    CHECK_FALSE(context_rebound);
    rfdetr::PredictionSession poisoned;
    CHECK_FALSE(poisoned.HasUnsafeCustody());
    std::ifstream prior_file(request.output_path);
    const auto prior = nlohmann::json::parse(prior_file);
    CHECK_THROWS_AS(poisoned.RunAndWrite(request, command,
                                         {.completed =
                                              [](const auto&, auto, const auto&) {
                                                  throw mmltk::backend::ml::runtime::CudaOperationError{cudaErrorUnknown, "unobservable source transaction"};
                                              }}),
                    mmltk::backend::ml::runtime::CudaOperationError);
    std::ifstream after_failure(request.output_path);
    CHECK(nlohmann::json::parse(after_failure) == prior);
    bool rebound = false;
    CHECK_THROWS_AS(poisoned.Run(request, command, {.begin = [&](const auto&) { rebound = true; }}), mmltk::backend::ml::runtime::CudaOperationError);
    CHECK_FALSE(rebound);
    CHECK(poisoned.HasUnsafeCustody());
    CHECK(poisoned.Close() != mmltk::backend::ml::runtime::kRuntimeSuccess);
    CHECK(poisoned.HasUnsafeCustody());
}
TEST_CASE("prediction output commit reports a destination failure and removes temporary data", "[model][rfdetr][prediction_json]") {
    const auto root = std::filesystem::temp_directory_path() / ("prediction-output-failure-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / "destination");
    const mmltk::testsupport::ScopedTestCleanup files{[&] { std::filesystem::remove_all(root); }};
    rfdetr::PredictRequest request;
    request.output_path = root / "destination";
    {
        rfdetr::PredictionJsonWriter writer(request);
        writer.Begin({.artifacts = {.class_layout = rfdetr::unresolved_class_layout(2)}});
        CHECK_THROWS_AS(writer.Complete(), std::filesystem::filesystem_error);
    }
    CHECK(std::filesystem::is_directory(request.output_path));
    for (const auto& entry : std::filesystem::directory_iterator(root)) CHECK(entry.path() == request.output_path);
}
TEST_CASE("prediction candidate and mask capacities are checked before allocation", "[model][rfdetr][prediction]") {
    using rfdetr::PredictionCapacity;
    CHECK_THROWS_AS(PredictionCapacity::Resolve(0, 1, 2, 3, 2, 2, false), std::invalid_argument);
    CHECK(PredictionCapacity::Resolve(rfdetr::kMaximumPredictionCandidates, 1, 2, 3, 2, 2, true).candidates == 6U);
    CHECK_THROWS_AS(PredictionCapacity::Resolve(rfdetr::kMaximumPredictionCandidates + 1U, 1, 2, 3, 2, 2, false), std::invalid_argument);
    CHECK_THROWS_AS(PredictionCapacity::Resolve(1, 1, 2, 3, 65536, 65536, true), std::invalid_argument);
    CHECK(PredictionCapacity::Resolve(1000, 1, 1000, 3, 4096, 4096, true).mask_bytes == 4096U * 4096U);
    const auto full_hd = PredictionCapacity::Resolve(500, 1, 300, 80, 1920, 1080, true);
    CHECK(full_hd.candidates == 500U);
    CHECK(full_hd.mask_bytes == 1920U * 1080U * sizeof(std::uint8_t));
    CHECK(rfdetr::checked_prediction_extent(500U, full_hd.mask_bytes, rfdetr::kMaximumPredictionTensorBytes) == 1036800000U);
    CHECK(rfdetr::checked_prediction_extent(1U, rfdetr::kMaximumPredictionTensorBytes, rfdetr::kMaximumPredictionTensorBytes) ==
          rfdetr::kMaximumPredictionTensorBytes);
    CHECK_THROWS_AS(rfdetr::checked_prediction_extent(1U, rfdetr::kMaximumPredictionTensorBytes + 1U, rfdetr::kMaximumPredictionTensorBytes),
                    std::invalid_argument);
    rfdetr::EncodedMask encoded;
    rfdetr::encode_mask_values_into(1U, 4U, encoded, [](auto pixel) { return pixel % 2U == 0U; }, 2U);
    CHECK(encoded.runs.size() == 2U);
    CHECK_THROWS_AS(rfdetr::encode_mask_values_into(1U, 6U, encoded, [](auto pixel) { return pixel % 2U == 0U; }, 2U), std::invalid_argument);
    CHECK_THROWS_AS(PredictionCapacity::Resolve(1, std::numeric_limits<std::size_t>::max(), 2, 3, 2, 2, false), std::invalid_argument);
    rfdetr::EncodedMask mask;
    CHECK_THROWS_AS(rfdetr::encode_mask_values_into(65536, 65536, mask, [](auto) { return false; }), std::invalid_argument);
}
TEST_CASE("validation timing counts accepted executions independently of planned images", "[inference]") {
    using namespace mmltk::backend::models::rfdetr;
    ValidationRunResult result;
    result.images = 100U;
    result.FinalizeTiming();
    CHECK(result.processed_images == 0U);
    CHECK(result.total_timing->img_per_s == 0.0);
    result.backends["onnx"].timing = {.seconds = 2.0, .images = 7U};
    result.cancelled = true;
    result.FinalizeTiming();
    CHECK(result.images == 100U);
    CHECK(result.processed_images == 7U);
    CHECK(result.total_timing->img_per_s == 3.5);
    result.backends["tensorrt"].timing = {.seconds = 1.0, .images = 5U};
    result.FinalizeTiming();
    CHECK(result.total_timing->images == 12U);
    CHECK(result.total_timing->img_per_s == 4.0);
    CHECK(result.backends.at("onnx").timing.images == 7U);
}
TEST_CASE("full HD prediction materializes masks only for threshold survivors", "[model][rfdetr][prediction][gpu]") {
    const auto root = std::filesystem::temp_directory_path() / ("prediction-hd-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    const mmltk::testsupport::ScopedTestCleanup files{[&] { std::filesystem::remove_all(root); }};
    const auto image = root / "frame.ppm";
    {
        std::ofstream file(image, std::ios::binary);
        file << "P6\n1920 1080\n255\n";
        const std::vector<char> rgb(1920U * 1080U * 3U);
        file.write(rgb.data(), rgb.size());
    }
    write_prediction_model(root / "model.onnx", 300);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    rfdetr::PredictRequest request;
    request.source_kind = rfdetr::PredictSourceKind::ImageFiles;
    request.image_inputs = {{image, "full HD", 42}};
    request.onnx_path = root / "model.onnx";
    request.preset_name = "rf-detr-nano";
    request.output_path = root / "result.json";
    request.resolution = 8;
    request.allow_fp16 = false;
    request.max_dets_per_image = 500;
    request.threshold = .5F;
    rfdetr::PredictionSession session;
    const auto result = session.RunAndWrite(request, {reinterpret_cast<std::uintptr_t>(stream), true},
                                            {.completed = [](const auto& record, auto pixels, const auto& annotations) {
                                                REQUIRE(record.detections.size() == 2U);
                                                CHECK(record.detections[0].mask.area == 1920U * 1080U);
                                                CHECK(record.detections[1].mask.area == 0U);
                                                CHECK(annotations.value_count == 0U);
                                                CHECK(annotations.boxes_xyxy.address == 0U);
                                                CHECK(annotations.class_references.address == 0U);
                                                CHECK(annotations.confidences.address == 0U);
                                                CHECK(annotations.masks.address == 0U);
                                                CHECK(pixels.rgb8 == nullptr);
                                            }});
    CHECK(result.processed_images == 1U);
}
TEST_CASE("bbox-only runtime consumers do not turn mask capacity into demand", "[model][rfdetr][prediction][gpu]") {
    namespace runtime = mmltk::backend::ml::runtime;
    const auto root = std::filesystem::temp_directory_path() / ("prediction-bbox-consumer-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    const mmltk::testsupport::ScopedTestCleanup files{[&] { std::filesystem::remove_all(root); }};
    write_prediction_model(root / "rf-detr-nano.onnx");
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    c10::cuda::CUDAStreamGuard guard(c10::cuda::getStreamFromExternal(stream, 0));
    const auto cuda = tensor::TensorOptions().device(tensor::kCUDA).dtype(tensor::kFloat);
    auto input = tensor::zeros({1, 3, 8, 8}, cuda);
    auto boxes = tensor::empty({2, 4}, cuda);
    auto labels = tensor::empty({2}, cuda.dtype(tensor::kInt));
    auto scores = tensor::empty({2}, cuda);
    auto masks = tensor::ones({2, 2, 2}, cuda.dtype(tensor::kUInt8));
    masks.mul_(77);
    std::array<runtime::AnalysisAnnotationStorage, 1> annotations{{{.source_region = {.width = 2, .height = 2},
                                                                    .value_capacity = 2,
                                                                    .boxes_xyxy = {.address = reinterpret_cast<std::uintptr_t>(boxes.data_ptr()),
                                                                                   .capacity_bytes = 32,
                                                                                   .shape = {.rank = 2, .extents = {2, 4}},
                                                                                   .element_type = runtime::AnalysisElementType::Float32},
                                                                    .class_references = {.address = reinterpret_cast<std::uintptr_t>(labels.data_ptr()),
                                                                                         .capacity_bytes = 8,
                                                                                         .shape = {.rank = 1, .extents = {2}},
                                                                                         .element_type = runtime::AnalysisElementType::Int32},
                                                                    .confidences = {.address = reinterpret_cast<std::uintptr_t>(scores.data_ptr()),
                                                                                    .capacity_bytes = 8,
                                                                                    .shape = {.rank = 1, .extents = {2}},
                                                                                    .element_type = runtime::AnalysisElementType::Float32},
                                                                    .masks = {.address = reinterpret_cast<std::uintptr_t>(masks.data_ptr()),
                                                                              .capacity_bytes = 8,
                                                                              .shape = {.rank = 3, .extents = {2, 2, 2}},
                                                                              .element_type = runtime::AnalysisElementType::Uint8}}}};
    rfdetr::ModelArtifactRequest artifacts;
    artifacts.onnx_path = root / "rf-detr-nano.onnx";
    auto backend = rfdetr::make_rfdetr_runtime_backend({.artifacts = artifacts,
                                                        .backend = "onnx",
                                                        .device = 0,
                                                        .command_stream = {reinterpret_cast<std::uintptr_t>(stream), true},
                                                        .static_resolution = 8,
                                                        .maximum_detections = 2,
                                                        .allow_fp16 = false});
    const runtime::RuntimeTensorBuffer input_buffer{.device_data = input.data_ptr(),
                                                    .capacity_bytes = 3U * 8U * 8U * sizeof(float),
                                                    .shape = {.rank = 4, .extents = {1, 3, 8, 8}},
                                                    .element_type = runtime::RuntimeElementType::Float32};
    const auto run = [&](rfdetr::RfdetrRuntimeBackend& selected, bool include_masks) {
        auto submission = selected.Run(input_buffer, annotations, {}, include_masks);
        selected.ReleaseAfterCompletion(std::move(submission));
        CHECK(annotations[0].value_count == 2U);
        CHECK(annotations[0].class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot);
        CHECK(labels[0].item<int>() == 0);
        CHECK(labels[1].item<int>() == 1);
    };
    run(*backend, false);
    CHECK_FALSE(annotations[0].masks_available);
    CHECK(masks.eq(77).all().item<bool>());
    run(*backend, true);
    CHECK(annotations[0].masks_available);
    CHECK(masks[0].eq(1).all().item<bool>());
    CHECK(masks[1].eq(0).all().item<bool>());
    const auto produced = masks.clone();
    run(*backend, false);
    CHECK_FALSE(annotations[0].masks_available);
    CHECK(tensor::equal(masks, produced));
    write_prediction_model(root / "bbox.onnx", 2, false);
    artifacts.onnx_path = root / "bbox.onnx";
    auto bbox_backend = rfdetr::make_rfdetr_runtime_backend({.artifacts = artifacts,
                                                             .backend = "onnx",
                                                             .device = 0,
                                                             .command_stream = {reinterpret_cast<std::uintptr_t>(stream), true},
                                                             .static_resolution = 8,
                                                             .maximum_detections = 2,
                                                             .allow_fp16 = false});
    annotations[0].masks_available = true;
    run(*bbox_backend, false);
    CHECK_FALSE(annotations[0].masks_available);
    CHECK(tensor::equal(masks, produced));
    CHECK(bbox_backend->Close() == runtime::kRuntimeSuccess);
    CHECK(backend->Close() == runtime::kRuntimeSuccess);
}
TEST_CASE("encoded mask allowance counts emitted runs across masks", "[model][rfdetr][prediction]") {
    std::size_t remaining = 4U;
    std::array<rfdetr::EncodedMask, 3> masks;
    rfdetr::encode_mask_values_into(1U, 6U, masks[0], [](auto pixel) { return pixel % 2U == 0U; }, remaining);
    REQUIRE(masks[0].runs.size() == 3U);
    REQUIRE(masks[0].runs.capacity() > masks[0].runs.size());
    remaining -= masks[0].runs.size();
    rfdetr::encode_mask_values_into(1U, 6U, masks[1], [](auto) { return false; }, remaining);
    CHECK(masks[1].runs.empty());
    CHECK(masks[1].runs.capacity() == 0U);
    rfdetr::encode_mask_values_into(1U, 6U, masks[2], [](auto) { return true; }, remaining);
    remaining -= masks[2].runs.size();
    CHECK(remaining == 0U);
    CHECK_THROWS_AS(rfdetr::encode_mask_values_into(1U, 1U, masks[1], [](auto) { return true; }, remaining), std::invalid_argument);
}
TEST_CASE("mask chunks account for model gather expansion device bools and registered host capacity", "[model][rfdetr][prediction]") {
    using rfdetr::PredictionMaskChunk;
    using rfdetr::SelectedMaskCapacity;
    const auto source_dominant = PredictionMaskChunk::Resolve(100, 1, 1, 1024, 1024, 4);
    CHECK(source_dominant.count == 2U);
    CHECK(source_dominant.capacity.Total() <= source_dominant.retained_limit);
    const auto half_precision = PredictionMaskChunk::Resolve(100, 1, 1, 1024, 1024, 2);
    CHECK(half_precision.count == 3U);
    CHECK(half_precision.capacity.Total() <= half_precision.retained_limit);
    const auto model_dominant = PredictionMaskChunk::Resolve(5000, 256, 256, 1, 1, 4);
    CHECK(model_dominant.count == 63U);
    CHECK(model_dominant.capacity.Total() <= model_dominant.retained_limit);
    CHECK_THROWS_AS(SelectedMaskCapacity::Resolve(5000, 256, 256, 1, 1, 4), std::invalid_argument);
    const auto exact = PredictionMaskChunk::Resolve(99, 1024, 1024, 1024, 1024, 4);
    CHECK(exact.count == 1U);
    const auto exact_bytes = rfdetr::SelectedMaskCapacity::Resolve(1, 1, 1, 2, 1398101, 4);
    CHECK(exact_bytes.Total() == rfdetr::kPredictionMaskChunkBytes);
    CHECK(PredictionMaskChunk::Resolve(2, 1, 1, 2, 1398101, 4).count == 1U);
    CHECK(PredictionMaskChunk::Resolve(2, 1, 2, 2, 1398101, 4).count == 1U);
    const auto oversized_one = PredictionMaskChunk::Resolve(2, 16384, 16384, 1, 1, 4);
    CHECK(oversized_one.count == 1U);
    CHECK(oversized_one.capacity.bytes[0] == rfdetr::kMaximumPredictionTensorBytes);
    CHECK(oversized_one.capacity.Total() <= oversized_one.retained_limit);
    CHECK_THROWS_AS(PredictionMaskChunk::Resolve(2, 16385, 16384, 1, 1, 4), std::invalid_argument);
    CHECK_THROWS_AS(PredictionMaskChunk::Resolve(2, 1, 1, std::numeric_limits<std::size_t>::max(), 2, 4), std::invalid_argument);
    CHECK_THROWS_AS(PredictionMaskChunk::Resolve(0, 1, 1, 1, 1, 4), std::invalid_argument);
    SelectedMaskCapacity overflow{{std::numeric_limits<std::size_t>::max(), 1U, 0U, 0U}};
    CHECK_THROWS_AS(overflow.Total(), std::invalid_argument);
}
TEST_CASE("selected mask workspace reuses allocation and resets dtype and shape high water", "[model][rfdetr][prediction]") {
    rfdetr::SelectedMaskWorkspace workspace;
    const auto queries = tensor::tensor({0L, 1L}, tensor::TensorOptions().dtype(tensor::kLong)).reshape({1, 2});
    auto logits = tensor::ones({1, 2, 4, 4});
    auto masks = workspace.Materialize(logits, queries, 8, 8);
    const auto address = masks.data_ptr();
    CHECK(masks.all().item<bool>());
    masks = tensor::Tensor{};
    CHECK(workspace.Materialize(logits, queries, 8, 8).data_ptr() == address);
    const auto first = workspace.RetainedCapacity(128U);
    CHECK(first.bytes == rfdetr::SelectedMaskCapacity::Resolve(2, 4, 4, 8, 8, 4).bytes);
    logits = -tensor::ones({1, 2, 8, 8}, tensor::TensorOptions().dtype(tensor::kHalf));
    CHECK_FALSE(workspace.Materialize(logits, queries, 4, 4).any().item<bool>());
    CHECK(workspace.RetainedCapacity(32U).bytes == rfdetr::SelectedMaskCapacity::Resolve(2, 8, 8, 4, 4, 2).bytes);
    workspace.ResetSettled();
    CHECK(workspace.RetainedCapacity(0U).Total() == 0U);
}
TEST_CASE("raw preparation settles submitted tensor copies and reports unobservable work", "[model][rfdetr][prediction][gpu]") {
    namespace runtime = mmltk::backend::ml::runtime;
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    const auto stream = c10::cuda::getCurrentCUDAStream(0).stream();
    const auto source = tensor::ones({2, 2}, tensor::TensorOptions().device(tensor::kCUDA));
    auto destination = tensor::zeros_like(source);
    runtime::AnalysisAnnotationStorage annotation;
    std::string failure;
    rfdetr::PredictionRawPreparation raw(true, annotation, failure, stream);
    raw.Execute([&] {
        destination.copy_(source, true);
        annotation.masks.address = reinterpret_cast<std::uintptr_t>(destination.data_ptr());
        throw std::runtime_error("dense copy failure after submission");
    });
    CHECK_FALSE(raw.available());
    CHECK(annotation.masks.address == 0U);
    CHECK(destination.eq(1).all().item<bool>());
    rfdetr::PredictionRawPreparation unsafe(true, annotation, failure, stream, +[](cudaStream_t) { return cudaErrorUnknown; });
    // These are the exact allocations the session retains on the typed failure;
    // no replacement tensor stands in for an in-flight source or destination.
    const auto source_address = source.data_ptr();
    const auto destination_address = destination.data_ptr();
    CHECK_THROWS_AS(unsafe.Execute([&] {
        destination.copy_(source, true);
        annotation.masks.address = reinterpret_cast<std::uintptr_t>(destination.data_ptr());
        throw std::runtime_error("unobservable dense copy");
    }),
                    runtime::CudaOperationError);
    CHECK_FALSE(unsafe.available());
    CHECK(annotation.masks.address == 0U);
    CHECK(source.data_ptr() == source_address);
    CHECK(destination.data_ptr() == destination_address);
    // The fake settlement failure did not poison actual hardware. Finish the
    // real work before these test-owned allocations leave scope.
    REQUIRE(cudaStreamSynchronize(stream) == cudaSuccess);
}
TEST_CASE("compiled prediction batch unwind closes shared source custody without losing the primary failure", "[model][rfdetr][prediction][gpu][custody]") {
    namespace data = mmltk::backend::data;
    namespace gpu = mmltk::frameworks::gpu;
    using data::testsupport::FixtureSpec;
    const mmltk::testsupport::ScopedTempDir root("prediction-batch-custody");
    const FixtureSpec fixture{root.path().string(), "train", 4, 4, 2};
    data::testsupport::create_synthetic_dataset(fixture);
    data::CompilerConfig config;
    config.source_dir = data::testsupport::dataset_dir(fixture);
    config.output_dir = data::testsupport::compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = fixture.width;
    config.target_height = fixture.height;
    config.num_workers = 1;
    const auto plan = data::DatasetCompiler::prepare(config, {config.split});
    data::DatasetCompiler::compile(plan, 0U);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup destroy{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    const data::DatasetLoader::Config loading{.compiled_path = data::testsupport::compiled_bin_path(fixture),
                                              .batch_size = 1U,
                                              .shuffle = false,
                                              .prefetch_factor = 2,
                                              .gather_workers = 1,
                                              .loading = data::data_loading_options(true)};
    for (const bool unsafe : {false, true})
        for (const bool explicit_release : {false, true}) {
            auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(rfdetr::DatasetBatchLease::kSourceRetirementCapacity);
            auto loader = std::make_shared<data::DatasetLoader>(loading, authority,
            unsafe ? +[](cudaEvent_t event, cudaStream_t value) -> cudaError_t {
                const auto recorded = cudaEventRecord(event, value);
                if (recorded != cudaSuccess) return recorded;
                const auto status = cudaStreamSynchronize(value);
                return status == cudaSuccess ? cudaErrorUnknown : status;
            } : &cudaEventRecord);
            std::weak_ptr<data::DatasetLoader> exact_loader = loader;
            data::Batch batch{};
            struct PrimaryFailure final : std::runtime_error {
                PrimaryFailure() : std::runtime_error("ordinary prediction failure") {}
            };
            bool primary_preserved = false;
            try {
                rfdetr::DatasetBatchLease transaction(loader, reinterpret_cast<std::uintptr_t>(stream), authority);
                CHECK_NOTHROW(transaction.Release());
                CHECK(authority->fact().reservations == 2U);
                CHECK(authority->admission_open());
                loader->begin_epoch();
                REQUIRE(loader->next_batch(batch));
                transaction.Adopt(batch);
                loader->wait_batch(batch);
                if (explicit_release) {
                    if (unsafe)
                        CHECK_THROWS_AS(transaction.Release(), mmltk::backend::ml::runtime::CudaOperationError);
                    else
                        transaction.Release();
                    CHECK_NOTHROW(transaction.Release());
                }
                throw PrimaryFailure{};
            } catch (const PrimaryFailure&) { primary_preserved = true; }
            REQUIRE(primary_preserved);
            CHECK(authority->admission_open() == !unsafe);
            CHECK(authority->fact().occupancy == (unsafe ? 1U : 0U));
            CHECK(authority->fact().reservations == 1U);         // exact compiled stream still belongs to loader
            if (unsafe) CHECK_FALSE(loader->next_batch(batch));  // failed release stopped/joined the live source
            loader->stop_workers();                              // idempotent after failed cleanup; settles normal asynchronous release
            CHECK_FALSE(loader->next_batch(batch));
            loader.reset();
            CHECK(exact_loader.expired() == !unsafe);
            if (unsafe) {
                for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
                    CHECK_FALSE(authority->Reserve());
                    CHECK_THROWS(data::DatasetLoader(loading, authority));
                }
                CHECK(authority->fact().occupancy == 1U);
                CHECK(authority->fact().reservations == 1U);
            } else {
                CHECK(authority->fact().reservations == 0U);
                data::DatasetLoader restarted(loading, authority);
            }
        }
}
TEST_CASE("ONNX metadata survives simplification and same-path prediction rebind", "[model][rfdetr][layout][gpu]") {
    namespace catalog = mmltk::backend::data::catalog;
    const auto root = mmltk::testsupport::make_temp_root("prediction-class-rebind");
    const mmltk::testsupport::ScopedTestCleanup files{[&] { std::filesystem::remove_all(root); }};
    const auto model = root / "rf-detr-nano.onnx", image = root / "frame.ppm";
    {
        std::ofstream file(image, std::ios::binary);
        file << "P6\n1 1\n255\n";
        file.put(127);
        file.put(0);
        file.put(0);
    }
    write_prediction_model(model);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream{};
    REQUIRE(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);
    const mmltk::testsupport::ScopedTestCleanup release{[&] { static_cast<void>(cudaStreamDestroy(stream)); }};
    rfdetr::PredictRequest request;
    request.source_kind = rfdetr::PredictSourceKind::ImageFiles;
    request.onnx_path = model;
    request.output_path = root / "predictions.json";
    request.resolution = 8;
    request.allow_fp16 = false;
    request.include_masks = true;
    request.max_dets_per_image = 2;
    request.image_inputs = {{image, "frame", 7}};
    rfdetr::PredictionSession session;
    const mmltk::backend::ml::runtime::BorrowedCommandStream command{reinterpret_cast<std::uintptr_t>(stream), true};
    const auto raw_images = session
                                .Run(request, command,
                                     {.completed =
                                          [](const auto& record, auto, const auto&) {
                                              REQUIRE(record.detections.size() == 2);
                                              CHECK(record.detections[0].class_domain == catalog::ClassReferenceDomain::RawOutputSlot);
                                              CHECK(record.detections[0].class_reference == 0);
                                          }})
                                .processed_images;
    CHECK(raw_images == 1);
    auto layout = rfdetr::native_training_class_layout(catalog::ClassCatalog({"first", "last"}));
    layout.slots.pop_back();
    layout.slots[0].foreground_index = 1;
    layout.slots[1].foreground_index = 0;
    layout.provenance = {rfdetr::ClassLayoutOrigin::Embedded, "explicit test producer", {}};
    write_prediction_model(model, 2, true, layout);
    CHECK(rfdetr::load_onnx_model_info(model).class_layout == layout);
    rfdetr::simplify_onnx_model_file(model);
    CHECK(rfdetr::load_onnx_model_info(model).class_layout == layout);
    const auto mapped_images = session
                                   .Run(request, command,
                                        {.completed =
                                             [](const auto& record, auto, const auto&) {
                                                 REQUIRE(record.detections.size() == 2);
                                                 CHECK(record.detections[0].class_domain == catalog::ClassReferenceDomain::Foreground);
                                                 CHECK(record.detections[0].class_reference == 1);
                                                 CHECK(record.detections[1].class_reference == 0);
                                                 CHECK(record.detections[0].mask.area == 1);
                                                 CHECK(record.detections[1].mask.area == 0);
                                             }})
                                   .processed_images;
    CHECK(mapped_images == 1);
    CHECK(session.Close() == mmltk::backend::ml::runtime::kRuntimeSuccess);
}
TEST_CASE("ONNX output roles accept explicit producer names without shape guessing", "[model][rfdetr][layout][onnx]") {
    namespace onnx = mmltk_onnx;
    const mmltk::testsupport::ScopedTempDir root("rfdetr-named-output-roles");
    const auto path = root.path() / "custom.onnx";
    write_prediction_model(path, 2, false);
    onnx::ModelProto model;
    {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(model.ParseFromIstream(&input));
    }
    const std::array<std::pair<std::string, std::string>, 2> names{{{"pred_logits", "scores_from_producer"}, {"pred_boxes", "locations_from_producer"}}};
    for (const auto& [from, to] : names) {
        for (auto& output : *model.mutable_graph()->mutable_output())
            if (output.name() == from) output.set_name(to);
        for (auto& node : *model.mutable_graph()->mutable_node())
            for (auto& output : *node.mutable_output())
                if (output == from) output = to;
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(model.SerializeToOstream(&output));
    }
    CHECK_THROWS(rfdetr::load_onnx_model_info(path));
    const std::array roles{rfdetr::RfdetrNamedOutputRole{"scores_from_producer", rfdetr::RfdetrOutputRole::Logits},
                           rfdetr::RfdetrNamedOutputRole{"locations_from_producer", rfdetr::RfdetrOutputRole::Boxes}};
    const auto admitted = rfdetr::load_onnx_model_info(path, roles);
    CHECK(admitted.num_classes == 2);
    CHECK(admitted.num_queries == 2);
    CHECK_FALSE(admitted.has_masks);
    CHECK_FALSE(admitted.class_layout);
    auto layout = rfdetr::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat", "dog"}));
    layout.slots.pop_back();
    const auto companion = std::filesystem::path(path.string() + ".classes.json");
    namespace io = mmltk::common::io;
    {
        std::ofstream output(companion);
        output << rfdetr::encode_class_descriptor(
            {1, io::sha256_hex(io::sha256_file(path)), layout, std::vector<rfdetr::RfdetrNamedOutputRole>(roles.begin(), roles.end())});
    }
    rfdetr::simplify_onnx_model_file(path);
    const auto descriptor = rfdetr::detail::read_class_descriptor(companion);
    CHECK(descriptor.artifact_sha256 == io::sha256_hex(io::sha256_file(path)));
    CHECK(descriptor.layout == layout);
    CHECK((rfdetr::rfdetr_output_roles(rfdetr::load_onnx_model_info(path, descriptor.output_roles)) == descriptor.output_roles));
}
TEST_CASE("Same-path ONNX simplification rebinds authoritative companion layout and roles", "[model][rfdetr][layout][onnx]") {
    namespace io = mmltk::common::io;
    const mmltk::testsupport::ScopedTempDir root("onnx-companion-simplification");
    const auto path = root.path() / "model.onnx";
    const auto companion = std::filesystem::path(path.string() + ".classes.json");
    write_prediction_model(path, 2, false);
    auto layout = rfdetr::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"dog", "cat"}));
    layout.slots.pop_back();
    layout.slots[0].foreground_index = 1;
    layout.slots[1].foreground_index = 0;
    const auto roles = rfdetr::rfdetr_output_roles(rfdetr::load_onnx_model_info(path));
    {
        std::ofstream output(companion);
        output << rfdetr::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(path)), layout, roles});
    }
    rfdetr::simplify_onnx_model_file(path);
    const auto descriptor = rfdetr::detail::read_class_descriptor(companion);
    CHECK(descriptor.artifact_sha256 == io::sha256_hex(io::sha256_file(path)));
    CHECK(descriptor.layout == layout);
    CHECK((descriptor.output_roles == roles));
    const auto info = rfdetr::load_onnx_model_info(path, descriptor.output_roles);
    CHECK(rfdetr::ClassArtifactAdmission(path).Resolve(info.num_classes, info.class_layout) == layout);
    CHECK((rfdetr::rfdetr_output_roles(info) == roles));
    // Malformed graph serialization must preserve both admitted files on failure.
    {
        std::ofstream output(path, std::ios::trunc);
        output << "not a protobuf";
    }
    {
        std::ofstream output(companion);
        output << rfdetr::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(path)), layout, roles});
    }
    const auto old_artifact = io::sha256_file(path), old_companion = io::sha256_file(companion);
    CHECK_THROWS(rfdetr::simplify_onnx_model_file(path));
    CHECK(io::sha256_file(path) == old_artifact);
    CHECK(io::sha256_file(companion) == old_companion);
}
TEST_CASE("Validation binds a consumed ONNX descriptor before TensorRT-only materialization", "[model][rfdetr][layout][gpu]") {
    namespace data = mmltk::backend::data;
    namespace io = mmltk::common::io;
    const mmltk::testsupport::ScopedTempDir root("validation-source-descriptor");
    const data::testsupport::FixtureSpec fixture{root.path().string(), "train", 8, 8, 8, 1, 0, true};
    data::testsupport::create_synthetic_dataset(fixture);
    const auto categories = std::filesystem::path(data::testsupport::dataset_dir(fixture)) / "categories.json";
    nlohmann::json catalog;
    {
        std::ifstream input(categories);
        input >> catalog;
    }
    catalog["classes"].erase(catalog["classes"].begin() + 2, catalog["classes"].end());
    for (int index = 3; index <= fixture.num_images; ++index) {
        auto filename = std::to_string(index);
        filename.insert(0U, 6U - filename.size(), '0');
        const auto path = std::filesystem::path(data::testsupport::dataset_dir(fixture)) / fixture.split / (filename + ".jsonl");
        nlohmann::json annotation;
        {
            std::ifstream input(path);
            input >> annotation;
        }
        annotation["class"] = index % 2 == 0 ? "person" : "ret";
        {
            std::ofstream output(path);
            output << annotation.dump() << '\n';
        }
    }
    {
        std::ofstream output(categories);
        output << catalog;
    }
    data::CompilerConfig config;
    config.source_dir = data::testsupport::dataset_dir(fixture);
    config.output_dir = data::testsupport::compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = fixture.width;
    config.target_height = fixture.height;
    config.num_workers = 1;
    data::DatasetCompiler::compile(data::DatasetCompiler::prepare(config, {config.split}), 0U);
    const auto source = root.path() / "rf-detr-nano.onnx";
    write_prediction_model(source, 2, false);
    auto layout = rfdetr::native_training_class_layout(data::catalog::ClassCatalog({"person", "ret"}));
    layout.slots.pop_back();
    const auto descriptor = root.path() / "selected.classes.json";
    const auto encoded = rfdetr::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(source)), layout});
    {
        std::ofstream output(descriptor);
        output << encoded;
    }
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    const auto stream = c10::cuda::getCurrentCUDAStream(0).stream();
    const mmltk::backend::ml::runtime::BorrowedCommandStream command{reinterpret_cast<std::uintptr_t>(stream), true};
    rfdetr::ValidateRequest request;
    request.onnx_path = source;
    request.class_layout_path = descriptor;
    request.compiled_path = data::testsupport::compiled_bin_path(fixture);
    request.save_engine_path = root.path() / "model.engine";
    request.eval_order = "tensorrt";
    request.resolution = 8;
    request.num_queries = request.eval_max_dets = 2;
    request.limit_images = 1;
    request.allow_fp16 = false;
    request.write_report_json = false;
    request.log_mode = rfdetr::ValidationLogMode::Quiet;
    request.h2d_dataloader = true;
    rfdetr::ValidationSession session;
    const auto selected_digest = io::sha256_file(descriptor);
    const auto only = session.Run(request, command);
    CHECK(only.eval_order == std::vector<std::string>{"tensorrt"});
    CHECK(only.processed_images == 1U);
    CHECK(only.backends.at("tensorrt").artifacts.preset_name == "rf-detr-nano");
    CHECK(only.backends.at("tensorrt").artifacts.class_layout == layout);
    const auto engine_descriptor = rfdetr::detail::read_class_descriptor(request.save_engine_path.string() + ".classes.json");
    CHECK(engine_descriptor.layout == layout);
    CHECK(engine_descriptor.artifact_sha256 == io::sha256_hex(io::sha256_file(request.save_engine_path)));
    for (const auto order : {"onnx,tensorrt", "tensorrt,onnx"}) {
        request.eval_order = order;
        const auto mixed = session.Run(request, command);
        CHECK(mixed.eval_order.front() == std::string(order).substr(0, std::string(order).find(',')));
        CHECK(mixed.processed_images == 2U);
        CHECK(mixed.delta_tensorrt_minus_onnx.has_value());
        CHECK(mixed.backends.at("onnx").artifacts.class_layout == layout);
        CHECK(mixed.backends.at("tensorrt").artifacts.class_layout == layout);
    }
    request.eval_order = "onnx";
    for (const std::size_t population : {3U, 7U}) {
        request.limit_images = population;
        std::vector<std::uint32_t> chosen, captured;
        std::size_t selections = 0U;
        const auto sampled = session.Run(request, command,
                                         {
                                             .mask_metrics = false,
                                             .samples_selected =
                                                 [&](auto indices, auto) {
                                                     ++selections;
                                                     chosen.assign(indices.begin(), indices.end());
                                                 },
                                             .sample =
                                                 [&](rfdetr::ValidationSampleView sample) {
                                                     REQUIRE(sample.pixels.chw);
                                                     REQUIRE(sample.pixels.custody);
                                                     CHECK(sample.annotations.class_catalog);
                                                     CHECK_FALSE(sample.ground_truth.empty());
                                                     captured.push_back(static_cast<std::uint32_t>(sample.prediction.dataset_index));
                                                 },
                                         });
        CHECK(selections == 1U);
        CHECK(chosen.size() == std::min<std::size_t>(6U, population));
        CHECK(std::ranges::adjacent_find(chosen) == chosen.end());
        CHECK(chosen.back() < population);
        CHECK(captured == chosen);
        CHECK(sampled.processed_images == population);
        CHECK_FALSE(sampled.backends.at("onnx").summary.mask);
        CHECK_FALSE(sampled.backends.at("onnx").details.empty());
    }
    std::stop_source cancelled;
    const auto partial = session.Run(request, command, {.stop = cancelled.get_token(), .progress = [&](auto processed, auto) {
                                                            if (processed == 1U) cancelled.request_stop();
                                                        }});
    CHECK(partial.cancelled);
    CHECK(partial.processed_images == 1U);
    CHECK(partial.backends.at("onnx").timing.images == 1U);
    request.limit_images = 1U;
    CHECK(io::sha256_file(descriptor) == selected_digest);
    const auto preserved = io::sha256_file(request.save_engine_path);
    auto unrelated = rfdetr::decode_class_descriptor(encoded);
    unrelated.artifact_sha256 = std::string(64, '0');
    {
        std::ofstream output(descriptor);
        output << rfdetr::encode_class_descriptor(unrelated);
    }
    CHECK_THROWS_AS(session.Run(request, command), std::invalid_argument);
    CHECK(io::sha256_file(request.save_engine_path) == preserved);
    CHECK(session.Close() == mmltk::backend::ml::runtime::kRuntimeSuccess);
}
TEST_CASE("Stopped synchronous export engine and validation skip production and preserve bundles", "[model][rfdetr][layout][cancellation]") {
    namespace io = mmltk::common::io;
    const mmltk::testsupport::ScopedTempDir root("rfdetr-stopped-producers");
    const auto output = root.path() / "previous.model";
    const auto companion = std::filesystem::path(output.string() + ".classes.json");
    const auto layout = rfdetr::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat"}));
    {
        std::ofstream file(output);
        file << "previous complete artifact";
    }
    const auto original = io::sha256_file(output);
    {
        std::ofstream file(companion);
        file << rfdetr::encode_class_descriptor({1, io::sha256_hex(original), layout});
    }
    const auto original_companion = io::sha256_file(companion);
    std::stop_source source;
    source.request_stop();
    rfdetr::ExportOnnxRequest export_request;
    export_request.weights_path = root.path() / "not-opened.pt";
    export_request.output_path = output;
    rfdetr::ExportOnnxSession exporter;
    CHECK_NOTHROW(exporter.Run(export_request, {}, source.get_token()));
    rfdetr::BuildEngineRequest engine;
    engine.onnx_path = root.path() / "not-opened.onnx";
    engine.output_path = output;
    CHECK_NOTHROW(rfdetr::build_tensorrt_engine(engine, {}, {}, source.get_token()));
    rfdetr::ValidateRequest validation;
    validation.onnx_path = engine.onnx_path;
    validation.class_layout_path = root.path() / "not-opened.classes.json";
    validation.compiled_path = root.path() / "not-opened.bin";
    validation.save_engine_path = output;
    validation.eval_order = "tensorrt";
    rfdetr::ValidationSession validator;
    const auto result = validator.Run(validation, {}, {.stop = source.get_token()});
    CHECK(result.cancelled);
    CHECK(result.processed_images == 0U);
    CHECK(result.backends.empty());
    CHECK(io::sha256_file(output) == original);
    CHECK(io::sha256_file(companion) == original_companion);
    for (const auto& entry : std::filesystem::directory_iterator(root.path())) CHECK_FALSE(entry.is_directory());
}
TEST_CASE("ONNX inspection completes against its retained descriptor proof", "[model][rfdetr][layout][onnx]") {
    namespace io = mmltk::common::io;
    const mmltk::testsupport::ScopedTempDir root("onnx-retained-descriptor");
    const auto path = root.path() / "model.onnx";
    const auto companion = std::filesystem::path(path.string() + ".classes.json");
    write_prediction_model(path, 2, false);
    auto layout = rfdetr::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat", "dog"}));
    layout.slots.pop_back();
    const auto encoded = rfdetr::encode_class_descriptor({1, io::sha256_hex(io::sha256_file(path)), layout});
    {
        std::ofstream output(companion);
        output << encoded;
    }
    const rfdetr::ClassArtifactAdmission admission(path);
    const auto info = rfdetr::load_onnx_model_info(path, admission.output_roles());
    CHECK(admission.Resolve(info.num_classes, info.class_layout) == layout);
    SECTION("replacement after loading rejects the original parsed facts") {
        const auto replacement = root.path() / "replacement.json";
        {
            std::ofstream output(replacement);
            output << encoded;
        }
        std::filesystem::rename(replacement, companion);
        CHECK_THROWS(admission.Resolve(info.num_classes, info.class_layout));
    }
    SECTION("stop after loading rejects completion without changing the source") {
        std::stop_source source;
        source.request_stop();
        CHECK_THROWS_AS(admission.Resolve(info.num_classes, info.class_layout, source.get_token()), rfdetr::ArtifactPublicationCancelled);
        CHECK_NOTHROW(admission.RequireUnchanged());
    }
}
