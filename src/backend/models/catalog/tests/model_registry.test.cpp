#include "src/backend/models/catalog/model_registry.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>
#include "src/backend/models/catalog/artifacts.h"
#include "src/backend/models/catalog/model_descriptor.h"
namespace {
TEST_CASE("catalog is derived from the RF-DETR contract", "[backend][models][catalog]") {
 using namespace mmltk::backend::models::catalog;
 REQUIRE_FALSE(models().empty());
 REQUIRE(models().size() == 1U);
 const auto* rfdetr = find_model("rfdetr");
 REQUIRE(rfdetr != nullptr);
 REQUIRE(rfdetr->display_name == std::string_view{"RF-DETR"});
 REQUIRE(rfdetr->capabilities.weights);
 REQUIRE(rfdetr->capabilities.onnx);
 REQUIRE(rfdetr->capabilities.tensorrt);
 REQUIRE(rfdetr->capabilities.training);
 REQUIRE(rfdetr->capabilities.live);
 REQUIRE(rfdetr->presets.size() == 10U);
 REQUIRE(find_model("missing") == nullptr);
 REQUIRE(find_model_for_preset("rf-detr-seg-medium") == rfdetr);
 REQUIRE(find_model_for_preset("missing") == nullptr);
 REQUIRE(find_model_for_artifact_path(std::filesystem::path{"/tmp/rf-detr-seg-medium.pt"}) == rfdetr);
 REQUIRE(find_model_for_artifact_path(std::filesystem::path{"/tmp/unrelated.bin"}) == nullptr);
}
TEST_CASE("catalog artifact spellings reject unknown vocabulary", "[backend][models][catalog]") {
 using namespace mmltk::backend::models::catalog;
 REQUIRE(artifact_kind_name(ModelArtifactInputKind::Weights) == std::string_view{"weights"});
 REQUIRE(artifact_kind_name(ModelArtifactInputKind::Onnx) == std::string_view{"onnx"});
 REQUIRE(artifact_kind_name(ModelArtifactInputKind::TensorRt) == std::string_view{"tensorrt"});
 REQUIRE(artifact_kind_name(ModelArtifactInputKind::None) == std::string_view{"none"});
 REQUIRE_FALSE(artifact_kind_name(static_cast<ModelArtifactInputKind>(std::numeric_limits<std::uint8_t>::max())).has_value());
}
}  // namespace
