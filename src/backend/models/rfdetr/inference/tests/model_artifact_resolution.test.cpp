#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <type_traits>
#include <utility>
#include "src/backend/ml/runtime/analysis_provider.h"

#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"

import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;

namespace rfdetr = mmltk::backend::models::rfdetr;

static_assert(std::is_same_v<decltype(rfdetr::MakeRfdetrAnalysisProvider(std::declval<const rfdetr::RfdetrAnalysisOptions&>())),
                             std::shared_ptr<mmltk::backend::ml::runtime::AnalysisProvider>>);

TEST_CASE("inference owns ONNX and TensorRT artifact selection", "[model][rfdetr][artifact_resolution]") {
    rfdetr::ModelArtifactRequest onnx;
    onnx.onnx_path = "/tmp/rf-detr-nano.onnx";
    const auto resolved_onnx = rfdetr::resolve_inference_artifact(onnx, "auto");
    REQUIRE(resolved_onnx.kind == rfdetr::InferenceArtifactKind::Onnx);
    REQUIRE(resolved_onnx.backend_name == "onnx");

    rfdetr::ModelArtifactRequest engine;
    engine.tensorrt_path = "/tmp/rf-detr-nano.engine";
    const auto resolved_engine = rfdetr::resolve_inference_artifact(engine, "auto");
    REQUIRE(resolved_engine.kind == rfdetr::InferenceArtifactKind::TensorRt);
    REQUIRE_FALSE(resolved_engine.compile_onnx_to_tensorrt);

    const auto described = rfdetr::describe_inference_artifact(onnx, resolved_onnx, 384U);
    REQUIRE(described.config.preset_name == "rf-detr-nano");
    REQUIRE(described.automatic_num_queries_cap == described.config.num_queries);
}

TEST_CASE("inference rejects ambiguous model artifact authority", "[model][rfdetr][artifact_resolution]") {
    rfdetr::ModelArtifactRequest ambiguous;
    ambiguous.onnx_path = "/tmp/rf-detr-nano.onnx";
    ambiguous.tensorrt_path = "/tmp/rf-detr-nano.engine";

    const auto onnx = rfdetr::resolve_inference_artifact(ambiguous, "onnx");
    REQUIRE(onnx.kind == rfdetr::InferenceArtifactKind::Onnx);
    REQUIRE(onnx.path.filename() == "rf-detr-nano.onnx");

    const auto tensorrt = rfdetr::resolve_inference_artifact(ambiguous, "tensorrt");
    REQUIRE(tensorrt.kind == rfdetr::InferenceArtifactKind::TensorRt);
    REQUIRE(tensorrt.path.filename() == "rf-detr-nano.engine");
    REQUIRE_FALSE(tensorrt.compile_onnx_to_tensorrt);

    REQUIRE_THROWS_AS(rfdetr::resolve_inference_artifact(ambiguous, "auto"), std::invalid_argument);
}
