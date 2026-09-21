#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/settings_system.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstdint>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
[[nodiscard]] contracts::ModelSelection export_model_selection(const contracts::GuiSettingsState& settings, const contracts::ModelArtifactInputKind input,
                                                               std::string artifact) {
 auto projection = contracts::model_settings_projection(settings, contracts::FeatureId::Export);
 REQUIRE(projection);
 projection->key.source = contracts::ModelSelectionSource::Custom;
 projection->key.input = input;
 return {.key = std::move(projection->key), .artifact = std::move(artifact)};
}
[[nodiscard]] contracts::ModelSelection selected_model(const contracts::GuiSettingsState& settings, const contracts::FeatureId workflow) {
 const auto input = subsystems::system::ComputeIntentMaterializer::ModelInputFor(settings, workflow);
 REQUIRE(input);
 return {.key = input->key, .artifact = input->custom_artifact};
}
TEST_CASE("export materialization keeps ONNX branch input and output identities disjoint", "[controller][systems][compute][export]") {
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.request.train_compiled_path = "/tmp/train.bin";
 settings.workflows.export_state.onnx_input_path = "/tmp/source.onnx";
 settings.workflows.export_state.onnx_output_path = "/tmp/exported.onnx";
 settings.workflows.export_state.output_path = "/tmp/exported.engine";
 settings.workflows.export_state.weights_path = "/tmp/source.pt";
 const contracts::ArtifactInspection inspection{
  .compatible = true,
  .splits = {split("/tmp/train.bin")},
  .detail = {},
 };
 settings.workflows.export_state.build_tensorrt = true;
 settings.workflows.export_state.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.export_state.model_input = contracts::ModelArtifactInputKind::Onnx;
 const auto onnx_model = export_model_selection(settings, contracts::ModelArtifactInputKind::Onnx, "/tmp/source.onnx");
 const auto engine = subsystems::system::ComputeIntentMaterializer::Export(settings, inspection, onnx_model);
 REQUIRE(engine);
 REQUIRE(std::holds_alternative<mmltk::backend::models::rfdetr::BuildEngineRequest>(*engine));
 const auto& engine_request = std::get<mmltk::backend::models::rfdetr::BuildEngineRequest>(*engine);
 CHECK(engine_request.onnx_path == "/tmp/source.onnx");
 CHECK(engine_request.output_path == "/tmp/exported.engine");
 settings.workflows.export_state.build_tensorrt = false;
 settings.workflows.export_state.model_input = contracts::ModelArtifactInputKind::Weights;
 const auto weight_model =
  // CLEANUP-IGNORE: The weight-to-ONNX branch asserts a different concrete request variant from TensorRT export.
  export_model_selection(settings, contracts::ModelArtifactInputKind::Weights, "/tmp/source.pt");
 const auto onnx = subsystems::system::ComputeIntentMaterializer::Export(settings, inspection, weight_model);
 REQUIRE(onnx);
 REQUIRE(std::holds_alternative<mmltk::backend::models::rfdetr::ExportOnnxRequest>(*onnx));
 const auto& onnx_request = std::get<mmltk::backend::models::rfdetr::ExportOnnxRequest>(*onnx);
 CHECK(onnx_request.weights_path == "/tmp/source.pt");
 CHECK(onnx_request.output_path == "/tmp/exported.onnx");
 CHECK(settings.workflows.export_state.onnx_input_path == "/tmp/source.onnx");
 CHECK(settings.workflows.export_state.onnx_output_path == "/tmp/exported.onnx");
}
TEST_CASE("model keys separate workflow artifacts from dataset splits and reject stale settings", "[controller][systems][compute][model]") {
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.request.train_compiled_path = "/tmp/train.bin";
 settings.workflows.train.request.val_compiled_path = "/tmp/val.bin";
 settings.workflows.train.request.weights_path = "/tmp/train.pt";
 settings.workflows.train.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.train.model_input = contracts::ModelArtifactInputKind::Weights;
 settings.workflows.validate.request.compiled_path = "/tmp/val.bin";
 settings.workflows.validate.request.onnx_path = "/tmp/validate.onnx";
 settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.validate.model_input = contracts::ModelArtifactInputKind::Onnx;
 settings.workflows.predict.source.compiled_path = "/tmp/train.bin";
 settings.workflows.predict.request.weights_path = "/tmp/predict.pt";
 settings.workflows.predict.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.predict.model_input = contracts::ModelArtifactInputKind::Weights;
 settings.workflows.export_state.build_tensorrt = false;
 settings.workflows.export_state.weights_path = "/tmp/export.pt";
 settings.workflows.export_state.onnx_output_path = "/tmp/export.onnx";
 settings.workflows.export_state.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.export_state.model_input = contracts::ModelArtifactInputKind::Weights;
 REQUIRE(contracts::gui_settings_valid(settings));
 const contracts::ArtifactInspection inspection{
  .compatible = true,
  .splits = {split("/tmp/train.bin"), split("/tmp/val.bin")},
  .detail = {},
 };
 const auto train = selected_model(settings, contracts::FeatureId::Train);
 const auto train_request = subsystems::system::ComputeIntentMaterializer::LocalTrain(settings, inspection, train);
 REQUIRE(train_request);
 CHECK(train_request->train_compiled_path == "/tmp/train.bin");
 CHECK(train_request->weights_path == "/tmp/train.pt");
 CHECK(train_request->test_compiled_path.empty());
 CHECK(settings.workflows.train.request.output_dir.empty());
 CHECK(train_request->output_dir == "./gui-train-output");
 auto explicit_test = settings;
 explicit_test.workflows.train.request.test_compiled_path = "/independent/test.bin";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::LocalTrain(explicit_test, inspection, train));
 auto with_test = inspection;
 with_test.splits.push_back(split("/independent/test.bin"));
 const auto selected_test = subsystems::system::ComputeIntentMaterializer::LocalTrain(explicit_test, with_test, train);
 REQUIRE(selected_test);
 CHECK(selected_test->test_compiled_path == "/independent/test.bin");
 with_test.splits.back().class_names = {{.value = "different"}};
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::LocalTrain(explicit_test, with_test, train));
 explicit_test.workflows.train.request.test_compiled_path.clear();
 REQUIRE(subsystems::system::ComputeIntentMaterializer::LocalTrain(explicit_test, inspection, train));
 auto resume_settings = settings;
 resume_settings.workflows.train.request.resume_path = train.artifact;
 const auto resumed = subsystems::system::ComputeIntentMaterializer::LocalTrain(resume_settings, inspection, train);
 REQUIRE(resumed);
 CHECK(resumed->weights_path.empty());
 CHECK(resumed->resume_path == train.artifact);
 resume_settings.workflows.train.request.resume_path = "/tmp/other-checkpoint.pt";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::LocalTrain(resume_settings, inspection, train));
 const auto validation = selected_model(settings, contracts::FeatureId::Validate);
 const auto validation_request = subsystems::system::ComputeIntentMaterializer::Validation(settings, inspection, validation);
 REQUIRE(validation_request);
 CHECK(validation_request->compiled_path == "/tmp/val.bin");
 CHECK(validation_request->weights_path.empty());
 CHECK(validation_request->onnx_path == "/tmp/validate.onnx");
 CHECK(validation_request->eval_order == "onnx");
 CHECK(validation_request->tensorrt_path.empty());
 const auto predict = selected_model(settings, contracts::FeatureId::Predict);
 const auto predict_request = subsystems::system::ComputeIntentMaterializer::Predict(settings, inspection, predict);
 REQUIRE(predict_request);
 CHECK(predict_request->compiled_path == "/tmp/train.bin");
 CHECK(predict_request->weights_path == "/tmp/predict.pt");
 auto stale = settings;
 stale.workflows.predict.request.preset_name = "rf-detr-small";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Predict(stale, inspection, predict));
 stale = settings;
 stale.workflows.predict.request.weights_path = "/tmp/replaced-predict.pt";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Predict(stale, inspection, predict));
 stale = settings;
 stale.workflows.predict.request.class_layout_path = "/tmp/selected.classes.json";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Predict(stale, inspection, predict));
 const auto rebound = selected_model(stale, contracts::FeatureId::Predict);
 const auto rebound_request = subsystems::system::ComputeIntentMaterializer::Predict(stale, inspection, rebound);
 REQUIRE(rebound_request);
 CHECK(rebound_request->class_layout_path == "/tmp/selected.classes.json");
 auto inconsistent = inspection;
 inconsistent.splits[1].class_names[0].value = "different";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::LocalTrain(settings, inconsistent, train));
}
TEST_CASE("validation materializes inherited and independent dataset sources", "[controller][systems][compute]") {
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.compiled_dataset_dir = "/inferred";
 settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.validate.request.weights_path = "/selected/model.pt";
 const auto model = selected_model(settings, contracts::FeatureId::Validate);
 const contracts::ArtifactInspection inspected{
  .compatible = true, .splits = {split("/inferred/val.bin"), split("/manual.bin"), split("/independent.bin")}, .detail = {}};
 const auto check_path = [&](const char* expected) {
  const auto request = subsystems::system::ComputeIntentMaterializer::Validation(settings, inspected, model);
  REQUIRE(request);
  CHECK(request->compiled_path == expected);
 };
 check_path("/inferred/val.bin");
 CHECK(settings.workflows.validate.request.compiled_path.empty());
 settings.workflows.train.use_compiled_directory_defaults = false;
 settings.workflows.train.request.val_compiled_path = "/manual.bin";
 check_path("/manual.bin");
 settings.workflows.validate.request.compiled_path = "/independent.bin";
 settings.workflows.train.request.val_compiled_path = "/missing.bin";
 check_path("/independent.bin");
 settings.workflows.validate.request.compiled_path.clear();
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Validation(settings, inspected, model));
}
TEST_CASE("compute inputs retain normalized full-path identity and image independence", "[controller][systems][compute]") {
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.validate.request.weights_path = "/selected/model.pt";
 settings.workflows.validate.request.compiled_path = "/second/./compiled.mmltk";
 settings.workflows.validate.request.onnx_path = "/stale/model.onnx";
 settings.workflows.validate.request.tensorrt_path = "/stale/model.engine";
 settings.workflows.validate.request.save_engine_path = "/stale/generated.engine";
 auto selection = selected_model(settings, contracts::FeatureId::Validate);
 const contracts::ArtifactInspection inspected{.compatible = true, .splits = {split("/first/compiled.mmltk"), split("/second/compiled.mmltk")}, .detail = {}};
 const auto validation = subsystems::system::ComputeIntentMaterializer::Validation(settings, inspected, selection);
 REQUIRE(validation);
 CHECK(validation->compiled_path == "/second/compiled.mmltk");
 CHECK(validation->eval_order == "weights");
 CHECK(validation->onnx_path.empty());
 CHECK(validation->tensorrt_path.empty());
 CHECK(validation->save_engine_path.empty());
 settings.workflows.validate.request.compiled_path = "/third/compiled.mmltk";
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Validation(settings, inspected, selection));
 settings.workflows.predict.model_source = contracts::ModelSelectionSource::Custom;
 settings.workflows.predict.request.weights_path = "/selected/model.pt";
 settings.workflows.predict.source.kind = contracts::SourceKind::SingleImage;
 settings.workflows.predict.source.single_image_path = "/images/frame.png";
 settings.workflows.predict.request.batch_size = 128U;
 settings.workflows.train.request.train_compiled_path.clear();
 selection = selected_model(settings, contracts::FeatureId::Predict);
 const auto image = subsystems::system::ComputeIntentMaterializer::Predict(settings, {}, selection);
 REQUIRE(image);
 CHECK(image->source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::ImageFiles);
 REQUIRE(image->image_inputs.size() == 1U);
 CHECK(image->image_inputs.front().image_path == "/images/frame.png");
 CHECK(image->compiled_path.empty());
 CHECK(image->batch_size == 1U);
 settings.workflows.predict.source.kind = contracts::SourceKind::VideoFile;
 settings.workflows.predict.source.video_file_path = "/videos/local.mp4";
 const auto video = subsystems::system::ComputeIntentMaterializer::Predict(settings, {}, selection);
 REQUIRE(video);
 CHECK(video->source_kind == mmltk::backend::models::rfdetr::PredictSourceKind::VideoFile);
 CHECK(video->video_path == "/videos/local.mp4");
 CHECK(video->image_inputs.empty());
 CHECK(video->compiled_path.empty());
 CHECK(video->batch_size == 1U);
 settings.workflows.predict.source.kind = contracts::SourceKind::VideoStream;
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Predict(settings, {}, selection));
 settings.workflows.predict.source.kind = contracts::SourceKind::CompiledDataset;
 settings.workflows.predict.source.compiled_path.clear();
 CHECK_FALSE(subsystems::system::ComputeIntentMaterializer::Predict(settings, inspected, selection));
}
TEST_CASE("model input materialization exhausts the canonical compatibility catalog", "[controller][systems][compute][model]") {
 const auto check_case = [](const contracts::FeatureId workflow, const contracts::ModelSelectionSource source,
                            const contracts::ModelArtifactInputKind selected_input, const bool build_tensorrt) {
  auto settings = contracts::default_gui_settings_state();
  const contracts::ModelArtifactSelectionState artifacts{
   .weights_path = "/tmp/model.pt",
   .onnx_path = "/tmp/model.onnx",
   .tensorrt_path = "/tmp/model.engine",
   .preset_name = std::string{contracts::kDefaultModelPresetName},
   .resolution = contracts::kDefaultModelResolution,
   .source = source,
   .input = selected_input,
  };
  switch (workflow) {
   case contracts::FeatureId::Train: contracts::apply_model_artifacts(settings.workflows.train, artifacts); break;
   case contracts::FeatureId::Validate: contracts::apply_model_artifacts(settings.workflows.validate, artifacts); break;
   case contracts::FeatureId::Predict: contracts::apply_model_artifacts(settings.workflows.predict, artifacts); break;
   case contracts::FeatureId::Export:
    settings.workflows.export_state.build_tensorrt = build_tensorrt;
    contracts::apply_model_artifacts(settings.workflows.export_state, artifacts);
    break;
   case contracts::FeatureId::Annotate:
   case contracts::FeatureId::Live:
   case contracts::FeatureId::Explore: FAIL("test case requires a ModelSystem workflow");
  }
  settings.workflows.train.request.class_layout_path = "/tmp/train.classes.json";
  settings.workflows.validate.request.class_layout_path = "/tmp/validate.classes.json";
  settings.workflows.predict.request.class_layout_path = "/tmp/predict.classes.json";
  settings.workflows.export_state.class_layout_path = "/tmp/export.classes.json";
  const auto projection = contracts::model_settings_projection(settings, workflow);
  REQUIRE(projection);
  CHECK(projection->key.workflow == workflow);
  CHECK(projection->key.source == source);
  CHECK(projection->key.input == selected_input);
  CHECK(projection->key.preset == contracts::kDefaultModelPresetName);
  CHECK(projection->key.resolution == contracts::kDefaultModelResolution);
  const std::string workflow_name = workflow == contracts::FeatureId::Train      ? "train"
                                    : workflow == contracts::FeatureId::Validate ? "validate"
                                     : workflow == contracts::FeatureId::Predict ? "predict"
                                                                                 : "export";
  CHECK(projection->key.class_layout_path == "/tmp/" + workflow_name + ".classes.json");
  CHECK(projection->export_build_tensorrt == build_tensorrt);
  // Distinct raw drafts prove source meaning independently of relation-fed fixtures.
  auto draft = settings;
  draft.workflows.train.request.preset_name = "train-draft";
  draft.workflows.validate.request.preset_name = "validate-draft";
  draft.workflows.predict.request.preset_name = "predict-draft";
  draft.workflows.export_state.preset_name = "export-draft";
  draft.workflows.train.request.resolution = 100;
  draft.workflows.validate.request.resolution = 101;
  draft.workflows.predict.request.resolution = 102;
  draft.workflows.export_state.model_resolution = 104;
  const auto draft_projection = contracts::model_settings_projection(draft, workflow);
  REQUIRE(draft_projection);
  CHECK(draft_projection->key.preset == workflow_name + "-draft");
  CHECK(draft_projection->key.resolution == 100U + static_cast<std::uint32_t>(workflow));
  const auto* compatibility = workflow == contracts::FeatureId::Export ? contracts::find_model_selection_compatibility(workflow, selected_input, build_tensorrt)
                                                                       : contracts::find_model_selection_compatibility(workflow, selected_input);
  const bool expected =
   selected_input != contracts::ModelArtifactInputKind::None && compatibility != nullptr && contracts::model_selection_source_allowed(*compatibility, source);
  CAPTURE(workflow, source, selected_input, build_tensorrt);
  const auto result = subsystems::system::ComputeIntentMaterializer::ModelInputFor(settings, workflow);
  CHECK(result.has_value() == expected);
  CHECK(projection->compatible == expected);
  if (!result) return;
  CHECK(result->key == projection->key);
  CHECK(result->key.workflow == workflow);
  CHECK(result->key.source == source);
  CHECK(result->key.input == selected_input);
  CHECK(result->key.input != contracts::ModelArtifactInputKind::None);
  CHECK(result->key.valid());
  REQUIRE(compatibility != nullptr);
  CHECK(contracts::model_selection_source_allowed(*compatibility, result->key.source));
  if (source == contracts::ModelSelectionSource::Canonical) {
   CHECK(result->custom_artifact.empty());
  } else {
   switch (selected_input) {
    case contracts::ModelArtifactInputKind::Weights: CHECK(result->custom_artifact == artifacts.weights_path); break;
    case contracts::ModelArtifactInputKind::Onnx: CHECK(result->custom_artifact == artifacts.onnx_path); break;
    case contracts::ModelArtifactInputKind::TensorRt: CHECK(result->custom_artifact == artifacts.tensorrt_path); break;
    case contracts::ModelArtifactInputKind::None: FAIL("materialized model input cannot be None");
   }
  }
 };
 for (const auto workflow : {contracts::FeatureId::Train, contracts::FeatureId::Validate, contracts::FeatureId::Predict}) {
  for (const auto source : {contracts::ModelSelectionSource::Canonical, contracts::ModelSelectionSource::Custom}) {
   for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx, contracts::ModelArtifactInputKind::TensorRt,
                            contracts::ModelArtifactInputKind::None}) {
    check_case(workflow, source, input, false);
   }
  }
 }
 for (const bool build_tensorrt : {false, true}) {
  for (const auto source : {contracts::ModelSelectionSource::Canonical, contracts::ModelSelectionSource::Custom}) {
   for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx, contracts::ModelArtifactInputKind::TensorRt,
                            contracts::ModelArtifactInputKind::None}) {
    check_case(contracts::FeatureId::Export, source, input, build_tensorrt);
   }
  }
 }
 auto unsupported = contracts::default_gui_settings_state();
 for (const auto workflow : {contracts::FeatureId::Annotate, contracts::FeatureId::Live, contracts::FeatureId::Explore}) {
  const auto rejected = subsystems::system::ComputeIntentMaterializer::ModelInputFor(unsupported, workflow);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().detail == "workflow does not support model selection");
 }
 const auto malformed =
  subsystems::system::ComputeIntentMaterializer::ModelInputFor(unsupported, static_cast<contracts::FeatureId>(std::numeric_limits<std::uint8_t>::max()));
 REQUIRE_FALSE(malformed);
 CHECK(malformed.error().detail == "model selection is incomplete");
}
TEST_CASE("model selection shutdown cancels and joins one active acquisition without publishing a selection", "[controller][systems][model]") {
 const auto root = mmltk::testsupport::make_temp_root("ordinary-model");
 SettingsSystem settings;
 REQUIRE(settings.Load(install_settings(root)).applied());
 std::ofstream(root / "weights.pt").put('\0');
 auto gate = std::make_shared<mmltk::testsupport::StopGate>();
 std::promise<contracts::ModelUiState> terminal;
 std::atomic_size_t terminals = 0U;
 std::atomic_size_t progress = 0U;
 std::atomic_bool malformed_progress = false;
 ModelSystem model{settings, [gate] { return std::make_unique<BlockingModelRuntime>(gate); },
                   [&](ModelSystem::event_type event) {
                    if (std::holds_alternative<ModelProgressChanged>(event)) {
                     const auto& value = std::get<ModelProgressChanged>(event);
                     if (value.progress.stage != contracts::ModelProgressStage::Verifying) malformed_progress = true;
                     ++progress;
                    } else {
                     ++terminals;
                     terminal.set_value(std::get<ModelChanged>(std::move(event)).snapshot);
                    }
                   }};
 CHECK_THROWS_AS(model.Select({.workflow = contracts::FeatureId::Explore}), contracts::InvalidIntentError);
 const auto admitted = model.Select({.workflow = contracts::FeatureId::Train});
 CHECK(admitted.active);
 CHECK_THROWS_AS(model.Select({.workflow = contracts::FeatureId::Train}), contracts::BusyError);
 model.Shutdown();
 const auto settled = terminal.get_future().get();
 CHECK(settled.terminal.outcome == contracts::ModelSelectionOutcome::Cancelled);
 CHECK_FALSE(settled.selection.valid());
 CHECK_FALSE(model.snapshot().active);
 CHECK_FALSE(model.selection().valid());
 CHECK(terminals == 1U);
 CHECK(progress == 1U);
 CHECK_FALSE(malformed_progress);
}
TEST_CASE("artifact model inspection observes cancellation before each opaque input loader", "[controller][systems][model]") {
 namespace contracts = mmltk::controller::contracts;
 const mmltk::testsupport::ScopedTempDir root("model-inspection-stop");
 const auto artifact = root.path() / "selected.model";
 {
  std::ofstream output(artifact);
  output << "loader must not consume these bytes";
 }
 for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx, contracts::ModelArtifactInputKind::TensorRt}) {
  contracts::ModelSelectionKey key{
   .workflow = contracts::FeatureId::Predict, .source = contracts::ModelSelectionSource::Custom, .input = input, .preset = "nano", .resolution = 64};
  REQUIRE(key.valid());
  mmltk::controller::ArtifactModelRuntime runtime;
  std::stop_source source;
  bool verified = false;
  const auto verifying = [&](const auto& progress) {
   CHECK(progress.stage == contracts::ModelProgressStage::Verifying);
   verified = true;
   source.request_stop();
  };
  CHECK_THROWS_WITH(runtime.Acquire(key, artifact, 0, source.get_token(), verifying), "model selection cancelled");
  CHECK(verified);
 }
}
}  // namespace
}  // namespace mmltk::controller
