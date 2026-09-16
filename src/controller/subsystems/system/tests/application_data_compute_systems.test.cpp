#include "src/common/io/file_digest.h"
#include "prediction_test_support.h"
#include "application_data_test_support.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_event_publisher.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include <cuda_runtime_api.h>
#include "src/test_support/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cuda.h>
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_failure.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <sched.h>
#include <system_error>
#include "src/common/system/tests/denied_syscall.h"
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include "src/test_support/filesystem_test_utils.hpp"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/persistence_storage.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/validate/validation_system.h"
#include "src/controller/subsystems/export/export_system.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/system/detail/predict_revision.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/train/training_system.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
TEST_CASE("Predict revision capacity preserves cancellation and terminal observations", "[controller][systems][predict]") {
    using Revision = detail::PredictRevision;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t reserved = 0U; reserved <= 5U; ++reserved) CHECK_THROWS_AS(Revision::Admit(maximum - reserved), contracts::FailedError);
    const auto admitted = Revision::Admit(maximum - 7U);
    REQUIRE(admitted == maximum - 6U);
    const auto progressed = Revision::Progress(admitted, false);
    REQUIRE(progressed == maximum - 5U);
    CHECK_FALSE(Revision::Progress(*progressed, false));
    const auto cancelled = Revision::Cancel(*progressed);
    REQUIRE(cancelled == maximum - 4U);
    CHECK_FALSE(Revision::Progress(*cancelled, true));
    const auto settled = Revision::Complete(*cancelled);
    REQUIRE(settled == maximum - 3U);
    CHECK_FALSE(Revision::Complete(*settled));
    const auto copied = Revision::Frame(*settled, false, false);
    REQUIRE(copied == maximum - 2U);
    const auto latest = Revision::Frame(*copied, false, false);
    REQUIRE(latest == maximum - 1U);
    CHECK_FALSE(Revision::Frame(*latest, false, false));
    CHECK(Revision::Fail(*latest) == maximum);
    CHECK_FALSE(Revision::Fail(maximum));
    CHECK(Revision::Fail(*cancelled) == maximum - 3U);
    CHECK(Revision::Complete(Revision::Admit(maximum - 6U)) == maximum - 4U);
    const auto earlier_cancel = Revision::Cancel(admitted);
    REQUIRE(earlier_cancel == maximum - 5U);
    CHECK(Revision::Progress(*earlier_cancel, true) == maximum - 4U);
}
template <class Event>
class TerminalSequence final {
   public:
    void Publish(Event event) {
        switch (next_.fetch_add(1U)) {
            case 0U: first_.set_value(std::move(event)); break;
            case 1U: second_.set_value(std::move(event)); break;
            default: third_.set_value(std::move(event)); break;
        }
    }
    [[nodiscard]] std::future<Event> First() { return first_.get_future(); }
    [[nodiscard]] std::future<Event> Second() { return second_.get_future(); }
    [[nodiscard]] std::future<Event> Third() { return third_.get_future(); }

   private:
    std::promise<Event> first_;
    std::promise<Event> second_;
    std::promise<Event> third_;
    std::atomic_size_t next_ = 0U;
};
TEST_CASE("compute runtime admission preserves valid progress and reports malformed or failed work", "[controller][systems][compute]") {
    const auto scenario = GENERATE(0, 1, 2, 3);
    std::vector<std::uint64_t> delivered;
    const auto terminal = run_checked_compute(
        [&](const ComputeProgressSink& progress) {
            std::jthread reporter([&] {
                progress({1U, 1U, 2U, "first"});
                if (scenario == 1) progress({2U, 3U, 2U, "invalid"});
                progress({3U, 2U, 2U, "last"});
            });
            reporter.join();
            if (scenario == 3) throw std::runtime_error("runtime failure detail");
            return contracts::make_compute_terminal(scenario == 2 ? contracts::ComputeOperationOutcome::Running : contracts::ComputeOperationOutcome::Succeeded,
                                                    0U, 2U);
        },
        [&](const contracts::ComputeProgress& progress) { delivered.push_back(progress.sequence); });
    CHECK(delivered == std::vector<std::uint64_t>{1U, 3U});
    CHECK(terminal.valid_worker_terminal());
    if (scenario == 0) {
        CHECK(terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
        CHECK(terminal.completed == 2U);
    } else {
        CHECK(terminal.outcome == contracts::ComputeOperationOutcome::Failed);
        CHECK(terminal.detail == (scenario == 3 ? "runtime failure detail" : "compute runtime returned an invalid terminal"));
    }
}
void queue_failed_dark_mode_update(SettingsSystem& settings, const std::filesystem::path& root) {
    const auto settings_path = root / "settings.json";
    REQUIRE(std::filesystem::remove(settings_path));
    REQUIRE(std::filesystem::create_directory(settings_path));
    contracts::SettingsUpdateRequest request;
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "ui.dark_mode",
        .value = mmltk::frameworks::serialization::wire::FlatValue{true},
    });
    CHECK_THROWS_AS(settings.Update(std::move(request)), contracts::FailedError);
    CHECK_FALSE(settings.snapshot().settings_state.ui.dark_mode);
}
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
TEST_CASE("compute inputs retain normalized full-path identity and image independence", "[controller][systems][compute]") {
    auto settings = contracts::default_gui_settings_state();
    settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
    settings.workflows.validate.request.weights_path = "/selected/model.pt";
    settings.workflows.validate.request.compiled_path = "/second/./compiled.mmltk";
    settings.workflows.validate.request.onnx_path = "/stale/model.onnx";
    settings.workflows.validate.request.tensorrt_path = "/stale/model.engine";
    settings.workflows.validate.request.save_engine_path = "/stale/generated.engine";
    auto selection = selected_model(settings, contracts::FeatureId::Validate);
    const contracts::ArtifactInspection inspected{
        .compatible = true, .splits = {split("/first/compiled.mmltk"), split("/second/compiled.mmltk")}, .detail = {}};
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
                                          : workflow == contracts::FeatureId::Predict  ? "predict"
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
        const auto* compatibility = workflow == contracts::FeatureId::Export
                                        ? contracts::find_model_selection_compatibility(workflow, selected_input, build_tensorrt)
                                        : contracts::find_model_selection_compatibility(workflow, selected_input);
        const bool expected = selected_input != contracts::ModelArtifactInputKind::None && compatibility != nullptr &&
                              contracts::model_selection_source_allowed(*compatibility, source);
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
            for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx,
                                     contracts::ModelArtifactInputKind::TensorRt, contracts::ModelArtifactInputKind::None}) {
                check_case(workflow, source, input, false);
            }
        }
    }
    for (const bool build_tensorrt : {false, true}) {
        for (const auto source : {contracts::ModelSelectionSource::Canonical, contracts::ModelSelectionSource::Custom}) {
            for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx,
                                     contracts::ModelArtifactInputKind::TensorRt, contracts::ModelArtifactInputKind::None}) {
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
[[nodiscard]] contracts::ProviderOffer provider_offer() {
    return {.offer_id = 17,
            .gpu_name = "A100",
            .gpu_count = 4,
            .gpu_ram_gib = 80.0,
            .hourly_price = 1.0,
            .reliability = 0.99,
            .location = "US",
            .family = contracts::ProviderGpuFamily::A100};
}
class UnsafePredictRuntime final : public PredictRuntime {
   public:
    UnsafePredictRuntime(bool on_close, std::shared_ptr<int> custody, bool preview_terminal = false)
        : on_close_(on_close), preview_terminal_(preview_terminal), custody_(std::move(custody)) {}
    void Close() noexcept override {
        ++*custody_;
        unsafe_ = true;
    }
    [[nodiscard]] bool HasUnsafeCustody() const noexcept override { return unsafe_; }
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token, const ComputeProgressSink&, const ProductSink&,
                                   const PlaybackGate&, VisualExtent, const ContextProvider&, const PreviewRetirement& retirement) override {
        if (preview_terminal_) {
            auto lease = mmltk::frameworks::gpu::ReserveTerminalCudaLease(*retirement);
            auto retained = custody_;
            std::move(lease).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(retained)), cudaErrorUnknown);
            throw mmltk::frameworks::gpu::CudaContextFailure(true);
        }
        unsafe_ = !on_close_;
        throw std::runtime_error("test prediction failure");
    }

   private:
    bool on_close_;
    bool preview_terminal_;
    bool unsafe_ = false;
    std::shared_ptr<int> custody_;
};
class FakeDialogRuntime final : public FileDialogRuntime {
   public:
    FakeDialogRuntime(std::shared_ptr<StopGate> gate, const bool fail) : gate_(std::move(gate)), fail_(fail) {}
    services::FileDialogSelection Open(const services::ResolvedFileDialog& dialog, const std::stop_token stop) override {
        if (!gate_->Wait(stop)) return {.target = dialog.target};
        if (fail_) throw contracts::FailedError("file dialog failed");
        return {.target = dialog.target, .result = services::FileDialogSelected{"/tmp/input"}};
    }

   private:
    std::shared_ptr<StopGate> gate_;
    bool fail_ = false;
};
class FakeTrainingRuntime final : public TrainingRuntime {
   public:
    FakeTrainingRuntime(std::shared_ptr<StopGate> gate, const bool fail, const bool inconclusive = false)
        : gate_(std::move(gate)), fail_(fail), inconclusive_(inconclusive) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, const std::stop_token stop,
                                     const std::function<void(const services::TrainProcessProgress&)>& progress) override {
        progress({.progress = fail_ ? contracts::ComputeProgress{.sequence = 0U, .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
                                    : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 1U, .status = "trained"}});
        if (!gate_->Wait(stop)) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
        if (fail_)
            return {.outcome = static_cast<contracts::ComputeOperationOutcome>(255U),
                    .output = std::string(contracts::kComputePathCapacity + 1U, 'x'),
                    .detail = {}};
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, const std::stop_token stop) override {
        if (!gate_->Wait(stop)) return {.outcome = contracts::ProviderQueryOutcome::Cancelled};
        if (fail_)
            return {.outcome = static_cast<contracts::ProviderQueryOutcome>(255U),
                    .offers = std::vector<contracts::ProviderOffer>(contracts::kProviderOfferCapacity + 1U),
                    .detail = std::string(contracts::kProviderDetailCapacity + 1U, 'x')};
        return {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {provider_offer()}};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, std::stop_token) override {
        if (inconclusive_) return contracts::provider_effect_inconclusive("provider result is unknown");
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }

   private:
    std::shared_ptr<StopGate> gate_;
    bool fail_ = false;
    bool inconclusive_ = false;
};
[[nodiscard]] TrainingSystem::RuntimeFactory reconstructing_training_runtime(std::shared_ptr<StopGate>& gate, std::atomic_size_t& constructions) {
    return [&gate, &constructions] {
        const bool fail = constructions++ == 0U;
        return std::make_unique<FakeTrainingRuntime>(gate, fail);
    };
}
struct QueryCancellationProbe final {
    std::promise<void> started;
    std::promise<void> cancellation_observed;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};
class BlockingCancellationTrainingRuntime final : public TrainingRuntime {
   public:
    explicit BlockingCancellationTrainingRuntime(std::shared_ptr<QueryCancellationProbe> probe) : probe_(std::move(probe)) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                     const std::function<void(const services::TrainProcessProgress&)>&) override {
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, const std::stop_token stop) override {
        probe_->started.set_value();
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        static_cast<void>(condition.wait(lock, stop, [] { return false; }));
        probe_->cancellation_observed.set_value();
        probe_->released.wait();
        return {.outcome = contracts::ProviderQueryOutcome::Cancelled};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, std::stop_token) override {
        return contracts::provider_effect_not_applied("unused");
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return contracts::provider_effect_not_applied("unused");
    }

   private:
    std::shared_ptr<QueryCancellationProbe> probe_;
};
class UnusedWeightOperations final : public services::ArtifactWeightOperations {
   public:
    [[nodiscard]] std::optional<services::ArtifactWeightAsset> find(std::string_view) const override { return std::nullopt; }
    void download(std::string_view, const std::filesystem::path&, const services::ArtifactCancellationToken&,
                  services::ArtifactWeightProgressObserver) const override {}
};
class DiagnosticCompiler final : public services::ArtifactCompilerOperations {
   private:
    void compile_directory(const std::filesystem::path&, const std::filesystem::path&, std::uint32_t, bool, mmltk::common::concurrency::CancellationObservation,
                           services::ArtifactProgressObserver) const override {}
    void compile_benchmark(const std::filesystem::path&, std::uint32_t, bool, mmltk::common::concurrency::CancellationObservation,
                           services::ArtifactProgressObserver, services::ArtifactBenchmarkTraceObserver trace) const override {
        trace("benchmark.direct", R"({"records":1})");
    }
};  // CLEANUP-IGNORE: Diagnostic compiler and blocking training runtime are separate typed dependency fakes.
class BlockingRemoteRuntime final : public TrainingRuntime {
   public:
    explicit BlockingRemoteRuntime(std::shared_ptr<StopGate> remote_gate) : remote_gate_(std::move(remote_gate)) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                     const std::function<void(const services::TrainProcessProgress&)>&) override {
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, std::stop_token) override {
        return {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {provider_offer()}};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, const std::stop_token stop) override {
        const bool released = remote_gate_->Wait(stop);
        if (!released || stop.stop_requested()) return contracts::provider_effect_not_applied("remote effect cancelled", true);
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }

   private:
    std::shared_ptr<StopGate> remote_gate_;
};
TEST_CASE("ordinary settings and file dialog expose direct state, Busy, Stop, and reconstruction", "[controller][systems][services]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-services");
    std::size_t settings_events = 0U;
    SettingsSystem settings{[&settings_events](SettingsSystem::event_type) { ++settings_events; }};
    REQUIRE(settings.Load(install_settings(root)).applied());
    CHECK(settings.snapshot().revision == 1U);
    CHECK(settings_events == 1U);
    const auto explore_preferences = settings.explore_settings_candidate().preferences.policy;
    CHECK(explore_preferences.filter.minimum_instances == 0U);
    CHECK(explore_preferences.filter.maximum_instances == 10'000U);
    CHECK(explore_preferences.filter.order == ExploreOrder::Sequential);
    CHECK(explore_preferences.filter.minimum_compiled_index == 0U);
    CHECK(explore_preferences.filter.maximum_compiled_index == std::numeric_limits<std::uint64_t>::max());
    CHECK(explore_preferences.overlay.class_selection.mode == ExploreClassSelectionMode::All);
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    std::promise<FileDialogSystem::event_type> first_completion;
    std::promise<FileDialogSystem::event_type> second_completion;
    std::promise<FileDialogSystem::event_type> third_completion;
    std::atomic_size_t completions = 0U;
    FileDialogSystem dialog{[&] {
                                ++constructions;
                                return std::make_unique<FakeDialogRuntime>(gate, constructions.load() == 1U);
                            },
                            [&](FileDialogSystem::event_type event) {
                                switch (completions++) {
                                    case 0U: first_completion.set_value(std::move(event)); throw std::runtime_error("observer failure");
                                    case 1U: second_completion.set_value(std::move(event)); break;
                                    default: third_completion.set_value(std::move(event)); break;
                                }
                            }};
    const services::FileDialogOpen selector{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{services::file_dialog_catalog().entries().front().stable_id}}};
    const auto admission = dialog.Open(selector);
    CHECK(admission.generation == 1U);
    CHECK(admission.active);
    CHECK(admission.valid());
    CHECK(admission.target == selector.target);
    CHECK_FALSE(admission.selection);
    CHECK_THROWS_AS(dialog.Open(selector), contracts::BusyError);
    gate->Release();
    const auto failed_dialog = first_completion.get_future().get();
    REQUIRE(std::holds_alternative<FileDialogFailed>(failed_dialog));
    CHECK(std::get<FileDialogFailed>(failed_dialog).snapshot.generation == 1U);
    CHECK(std::get<FileDialogFailed>(failed_dialog).snapshot.target == selector.target);
    CHECK_FALSE(dialog.snapshot().active);
    CHECK(dialog.snapshot().valid());
    CHECK_FALSE(dialog.snapshot().selection);
    gate = std::make_shared<StopGate>();
    static_cast<void>(dialog.Open(selector));
    const auto stopping = dialog.Stop();
    CHECK(stopping.generation == 2U);
    CHECK(stopping.active);
    CHECK(stopping.cancellation_requested);
    CHECK(stopping.valid());
    CHECK(stopping.target == selector.target);
    CHECK_FALSE(stopping.selection);
    CHECK(dialog.Stop().generation == stopping.generation);
    CHECK(dialog.Stop().cancellation_requested);
    CHECK(std::holds_alternative<FileDialogCompleted>(second_completion.get_future().get()));
    REQUIRE(dialog.snapshot().selection);
    CHECK(dialog.snapshot().valid());
    CHECK(dialog.snapshot().selection->valid_for(selector.target));
    CHECK_FALSE(dialog.snapshot().selection->selected());
    gate->Release();
    // CLEANUP-IGNORE: The fake dialog service independently injects selected and cancelled outcomes; no native modal is
    // automated.
    static_cast<void>(dialog.Open(selector));
    CHECK(std::holds_alternative<FileDialogCompleted>(third_completion.get_future().get()));
    REQUIRE(dialog.snapshot().selection);
    CHECK(dialog.snapshot().valid());
    CHECK(dialog.snapshot().selection->valid_for(selector.target));
    REQUIRE(dialog.snapshot().selection->selected());
    CHECK(std::get<services::FileDialogSelected>(dialog.snapshot().selection->result).path == "/tmp/input");
    CHECK(constructions == 2U);
}
TEST_CASE("production direct adapters reject unavailable physical dependencies", "[controller][systems][production-adapters]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-production-adapters");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    std::promise<FileDialogSystem::event_type> dialog_terminal;
    FileDialogSystem dialog{[&] { return std::make_unique<NativeFileDialogRuntime>(services::FileDialogClient{}, settings); },
                            [&](FileDialogSystem::event_type event) { dialog_terminal.set_value(std::move(event)); }};
    static_cast<void>(dialog.Open(services::FileDialogOpen{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{services::file_dialog_catalog().entries().front().stable_id}}}));
    CHECK(std::holds_alternative<FileDialogFailed>(dialog_terminal.get_future().get()));
    ArtifactDatasetRuntime artifacts;
    const auto rejected = artifacts.Compile({}, {}, {});
    CHECK_FALSE(rejected.inspection.available());
    CHECK_THROWS_AS(CudaValidationRuntime(DirectComputeConfiguration{}), contracts::UnavailableError);
    CHECK_THROWS_AS(CudaExportRuntime(DirectComputeConfiguration{}), contracts::UnavailableError);
    CHECK_THROWS_AS(CudaPredictRuntime(DirectComputeConfiguration{}), contracts::UnavailableError);
    NativeTrainingRuntime training({.provider = {}, .training_executable = root / "missing-trainer"});
    CHECK_THROWS_AS(training.Query(settings.provider_preferences(), {}), contracts::UnavailableError);
    CHECK_THROWS(training.Train({}, {}, {}));
}
TEST_CASE("provider result materialization enforces reflected bounds and identity", "[controller][systems][provider-materialization]") {
    services::VastOfferSummary offer;
    offer.offer_id = 7;
    offer.gpu_name = "H100";
    offer.num_gpus = 4;
    offer.gpu_ram = 80.0;
    offer.dph = 1.0;
    offer.reliability = 0.99;
    offer.geolocation = "US";
    std::vector offers{offer};
    const auto valid = services::materialize_provider_query_result(offers);
    REQUIRE(valid.outcome == contracts::ProviderQueryOutcome::Succeeded);
    REQUIRE(valid.offers.size() == 1U);
    CHECK(valid.offers.front().offer_id == 7);
    offers.front().gpu_name.assign(129U, 'x');
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers.front() = offer;
    offers.front().dph = std::numeric_limits<double>::quiet_NaN();
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers = {offer, offer};
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers.assign(contracts::kProviderOfferCapacity + 1U, offer);
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    CHECK(contracts::normalize_provider_query_result({.outcome = static_cast<contracts::ProviderQueryOutcome>(255U)}).outcome ==
          contracts::ProviderQueryOutcome::Failed);
    CHECK(contracts::normalize_provider_query_result(
              {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {valid.offers.front(), valid.offers.front()}})
              .outcome == contracts::ProviderQueryOutcome::Failed);
}
TEST_CASE("artifact dataset runtime forwards only its configured diagnostics observer", "[controller][systems][dataset][diagnostics]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset-diagnostics");
    UnusedWeightOperations weights;
    DiagnosticCompiler compiler;
    std::atomic_size_t traces = 0U;
    const services::ArtifactCompileRequest request{.kind = services::ArtifactCompileKind::Benchmark,
                                                   .source = {},
                                                   .output = root / "enabled",
                                                   .preset = "rf-detr-base",
                                                   .resolution = 560U,
                                                   .overwrite = true};
    const auto diagnostics =
        services::ArtifactDiagnosticObserver{.benchmark = {.context = &traces, .report = [](const void* context, std::string_view, std::string_view) noexcept {
                                                               ++*const_cast<std::atomic_size_t*>(static_cast<const std::atomic_size_t*>(context));
                                                           }}};
    ArtifactDatasetRuntime enabled{services::ArtifactStore{root / "cache", weights, compiler}, diagnostics};
    static_cast<void>(enabled.Compile(request, {}, {}));
    CHECK(traces == 1U);
    ArtifactDatasetRuntime disabled{services::ArtifactStore{root / "cache", weights, compiler}};
    auto disabled_request = request;
    disabled_request.output = root / "disabled";
    static_cast<void>(disabled.Compile(disabled_request, {}, {}));
    CHECK(traces == 1U);
}
TEST_CASE("local run linearizes Stop with admission and installed worker", "[controller][systems][run]") {
    direct::LocalRun run;
    std::promise<void> preparing;
    std::promise<void> release_prepare;
    std::promise<bool> observed_stop;
    std::condition_variable_any stop_condition;
    std::mutex stop_mutex;
    auto release = release_prepare.get_future().share();
    auto launch = std::async(std::launch::async, [&] {
        run.Start({
            .prepare =
                [&] {
                    preparing.set_value();
                    release.wait();
                },
            .work = [&](const std::stop_token stop) -> direct::LocalRun::Notification {
                std::unique_lock lock(stop_mutex);
                const bool completed = stop_condition.wait(lock, stop, [] { return false; });
                observed_stop.set_value(!completed && stop.stop_requested());
                return {};
            },
        });
    });
    preparing.get_future().wait();
    auto admitted = run.CurrentStopSource();
    REQUIRE(admitted.stop_possible());
    CHECK(run.Stop());
    release_prepare.set_value();
    launch.get();
    CHECK(observed_stop.get_future().get());
    run.StopAndJoin();
    auto second_gate = std::make_shared<StopGate>();
    std::promise<void> second_started;
    std::promise<bool> second_cancelled;
    run.Start({
        .work = [&](const std::stop_token stop) -> direct::LocalRun::Notification {
            second_started.set_value();
            static_cast<void>(second_gate->Wait(stop));
            second_cancelled.set_value(stop.stop_requested());
            return {};
        },
    });
    second_started.get_future().wait();
    CHECK_FALSE(admitted.request_stop());
    CHECK_FALSE(run.CurrentStopSource().stop_requested());
    second_gate->Release();
    CHECK_FALSE(second_cancelled.get_future().get());
}
TEST_CASE("local run preserves admission after prepare throws", "[controller][systems][run]") {
    direct::LocalRun run;
    direct::LocalRun::Job rejected{
        .prepare = [] { throw contracts::FailedError("admission failed"); },
        .work = [](std::stop_token) -> direct::LocalRun::Notification { return {}; },
    };
    CHECK_THROWS_AS(run.Start(std::move(rejected)), contracts::FailedError);
    CHECK_FALSE(run.active());
    CHECK_FALSE(run.CurrentStopSource().stop_possible());
    std::promise<void> completed;
    run.Start({
        .work = [&](std::stop_token) -> direct::LocalRun::Notification {
            completed.set_value();
            return {};
        },
    });
    completed.get_future().wait();
}
TEST_CASE("settings serializes competing durable updates", "[controller][systems][settings]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-concurrent");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    std::barrier start{3};
    std::atomic_int applied = 0;
    auto update = [&](contracts::SettingsValueUpdate value) {
        start.arrive_and_wait();
        try {
            contracts::SettingsUpdateRequest request;
            request.updates.emplace_back(std::move(value));
            static_cast<void>(settings.Update(std::move(request)));
            ++applied;
        } catch (...) {}
    };
    std::jthread first{update, contracts::SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}}};
    std::jthread second{update, contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius",
                                                               .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}}};
    start.arrive_and_wait();
    first.join();
    second.join();
    CHECK(applied == 2);
    const auto snapshot = settings.snapshot();
    CHECK(snapshot.revision == 3U);
    CHECK(snapshot.settings_state.ui.dark_mode);
    CHECK(snapshot.settings_state.ui.annotation_brush_radius == 9);
}
TEST_CASE("settings rejects empty updates without persisting and accepts relation clears", "[controller][systems][settings]") {
    using TrainRequest = mmltk::backend::models::rfdetr::TrainRequest;
    using TrainRecipeRelation = mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
    constexpr auto lr = mmltk::frameworks::reflection::member_path<&TrainRequest::lr>;
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-empty-update");
    const services::SettingsLocation location{(root / "gui.json").string()};
    std::size_t events = 0U;
    SettingsSystem settings{[&events](SettingsSystem::event_type) { ++events; }};
    REQUIRE(settings.Load(location).applied());
    const auto before = settings.snapshot();
    const auto events_before = events;
    CHECK_THROWS_AS(settings.Update({}), contracts::InvalidIntentError);
    CHECK(settings.snapshot() == before);
    CHECK(events == events_before);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == before);
    contracts::SettingsUpdateRequest pin;
    pin.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.train.request.lr",
        .value = mmltk::frameworks::serialization::wire::FlatValue{0.002},
    });
    const auto pinned = settings.Update(std::move(pin));
    CHECK(TrainRecipeRelation::template overridden<lr>(pinned.settings_state.workflows.train.request.recipe_overrides));
    contracts::SettingsUpdateRequest clear;
    clear.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.train.request.lr",
        .value = mmltk::frameworks::serialization::wire::FlatValue{std::monostate{}},
    });
    const auto cleared = settings.Update(std::move(clear));
    CHECK(cleared.revision == pinned.revision + 1U);
    CHECK_FALSE(TrainRecipeRelation::template overridden<lr>(cleared.settings_state.workflows.train.request.recipe_overrides));
    CHECK(cleared.settings_state.workflows.train.request.lr ==
          mmltk::backend::models::rfdetr::train_recipe(cleared.settings_state.workflows.train.request.optimizer).lr);
}
TEST_CASE("Explore catalog identity changes only through successful catalog persistence", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    const auto before = settings.snapshot();
    contracts::SettingsUpdateRequest request;
    request.updates.emplace_back(contracts::SettingsValueUpdate{
        .path = "workflows.explore.class_catalog_identity",
        .value = mmltk::frameworks::serialization::wire::FlatValue{std::uint64_t{0x1234U}},
    });
    CHECK_THROWS_AS(settings.Update(std::move(request)), contracts::InvalidIntentError);
    CHECK(settings.snapshot() == before);
    constexpr ExploreClassCatalogIdentity identity = 0x9123'4567'89ab'cdefULL;
    const auto candidate = settings.explore_settings_candidate();
    settings.persist_explore_class_catalog(candidate, identity, candidate.preferences.policy);
    CHECK(settings.snapshot().settings_state.workflows.explore.class_catalog_identity == identity);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot().settings_state.workflows.explore.class_catalog_identity == identity);
}
TEST_CASE("successful Explore catalog persistence includes a pending Settings recovery", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog-pending");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    queue_failed_dark_mode_update(settings, root);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    constexpr ExploreClassCatalogIdentity identity = 0x1234'5678U;
    const auto candidate = settings.explore_settings_candidate();
    settings.persist_explore_class_catalog(candidate, identity, candidate.preferences.policy);
    const auto committed = settings.snapshot();
    CHECK(committed.settings_state.ui.dark_mode);
    CHECK(committed.settings_state.workflows.explore.class_catalog_identity == identity);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == committed);
}
TEST_CASE("failed Explore catalog persistence preserves the exact pending Settings recovery", "[controller][systems][settings][explore]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-explore-catalog-retry");
    const auto location = install_settings(root);
    SettingsSystem settings;
    REQUIRE(settings.Load(location).applied());
    queue_failed_dark_mode_update(settings, root);
    constexpr ExploreClassCatalogIdentity rejected_identity = 0x8765'4321U;
    const auto candidate = settings.explore_settings_candidate();
    CHECK_THROWS_AS(settings.persist_explore_class_catalog(candidate, rejected_identity, candidate.preferences.policy), contracts::FailedError);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    REQUIRE(settings.Retry().applied());
    const auto recovered = settings.snapshot();
    CHECK(recovered.settings_state.ui.dark_mode);
    CHECK(recovered.settings_state.workflows.explore.class_catalog_identity == 0U);
    SettingsSystem reloaded;
    REQUIRE(reloaded.Load(location).applied());
    CHECK(reloaded.snapshot() == recovered);
}
TEST_CASE("settings retry cannot overwrite a newer committed update", "[controller][systems][settings]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-settings-retry");
    std::atomic_bool block_changed = false;
    std::promise<void> update_committed;
    std::promise<void> release_update;
    auto release = release_update.get_future().share();
    SettingsSystem settings{[&](SettingsSystem::event_type event) {
        if (block_changed && std::holds_alternative<SettingsChanged>(event)) {
            update_committed.set_value();
            release.wait();
        }
    }};
    REQUIRE(settings.Load(install_settings(root)).applied());
    queue_failed_dark_mode_update(settings, root);
    REQUIRE(std::filesystem::remove(root / "settings.json"));
    block_changed = true;
    std::jthread update{[&] {
        contracts::SettingsUpdateRequest request;
        request.updates.emplace_back(
            contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}});
        static_cast<void>(settings.Update(std::move(request)));
    }};
    update_committed.get_future().wait();
    std::promise<void> retry_started;
    std::jthread retry{[&] {
        retry_started.set_value();
        static_cast<void>(settings.Retry());
    }};
    retry_started.get_future().wait();
    release_update.set_value();
    update.join();
    retry.join();
    const auto snapshot = settings.snapshot();
    CHECK(snapshot.revision == 2U);
    CHECK_FALSE(snapshot.settings_state.ui.dark_mode);
    CHECK(snapshot.settings_state.ui.annotation_brush_radius == 9);
}
TEST_CASE("dataset publishes direct progress, returns Busy, stops locally, and reconstructs after failure", "[controller][systems][dataset]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    TerminalSequence<DatasetSystem::event_type> terminals;
    std::atomic_size_t progress = 0U;
    DatasetSystem dataset{settings,
                          [&] {
                              const bool fail = constructions++ == 0U;
                              return std::make_unique<FakeDatasetRuntime>(gate, fail);
                          },
                          [&](DatasetSystem::event_type event) {
                              if (std::holds_alternative<DatasetProgress>(event))
                                  ++progress;
                              else
                                  terminals.Publish(std::move(event));
                          }};
    static_cast<void>(dataset.Compile({}));
    CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
    // CLEANUP-IGNORE: Compile and Inspect are separate dataset admission endpoints sharing one system-owned runtime.
    CHECK_THROWS_AS(dataset.Inspect({}, "rf-detr-base", 560U), contracts::BusyError);
    gate->Release();
    const auto failed_dataset = terminals.First().get();
    REQUIRE(std::holds_alternative<DatasetChanged>(failed_dataset));
    CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.generation == 1U);
    CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
    CHECK(progress == 0U);
    CHECK_FALSE(dataset.snapshot().active);
    gate = std::make_shared<StopGate>();
    static_cast<void>(dataset.Compile({}));
    static_cast<void>(dataset.Stop());
    CHECK(std::holds_alternative<DatasetChanged>(terminals.Second().get()));
    CHECK(dataset.snapshot().terminal.outcome == contracts::ArtifactTerminalOutcome::Cancelled);
    gate->Release();
    static_cast<void>(dataset.Compile({}));
    CHECK(std::holds_alternative<DatasetChanged>(terminals.Third().get()));
    CHECK(dataset.snapshot().terminal.outcome == contracts::ArtifactTerminalOutcome::Succeeded);
    CHECK(constructions == 2U);
}
TEST_CASE("dataset rejects Compile and a second Inspect while Inspect owns the runtime", "[controller][systems][dataset][admission]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset-inspect-admission");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    auto observation = std::make_shared<DatasetRuntimeObservation>();
    std::atomic_int constructions = 0;
    std::promise<DatasetSystem::event_type> compile_done;
    DatasetSystem dataset{settings,
                          [&] {
                              ++constructions;
                              return std::make_unique<BlockingInspectRuntime>(observation);
                          },
                          [&](DatasetSystem::event_type event) {
                              if (!std::holds_alternative<DatasetProgress>(event)) compile_done.set_value(std::move(event));
                          }};
    const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity> paths{root / "train.bin", {}, {}};
    auto inspecting = std::async(std::launch::async, [&] { return dataset.Inspect(paths, "rf-detr-base", 560U); });
    observation->inspect_started.get_future().wait();
    CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
    CHECK_THROWS_AS(dataset.Inspect(paths, "rf-detr-base", 560U), contracts::BusyError);
    CHECK(constructions == 1);
    CHECK(observation->compile_calls == 0);
    CHECK(observation->maximum_active_calls == 1);
    observation->inspect_gate->Release();
    CHECK(inspecting.get().available());
    static_cast<void>(dataset.Compile({}));
    CHECK(std::holds_alternative<DatasetChanged>(compile_done.get_future().get()));
    CHECK(observation->compile_calls == 1);
    CHECK(observation->maximum_active_calls == 1);
}
TEST_CASE("checked operation identity advancement refuses exhaustion", "[controller][systems][identity]") {
    CHECK_FALSE(contracts::next_compute_generation(std::numeric_limits<std::uint64_t>::max()));
    CHECK(contracts::next_compute_generation(0U) == 1U);
}
TEST_CASE("model selection shutdown cancels and joins one active acquisition without publishing a selection", "[controller][systems][model]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-model");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    std::ofstream(root / "weights.pt").put('\0');
    auto gate = std::make_shared<StopGate>();
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
TEST_CASE("model and compute systems use direct facts, progress, Busy, Stop, and lazy runtime reconstruction", "[controller][systems][compute]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-compute");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Validate);
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    TerminalSequence<ValidationSystem::event_type> terminals;
    std::atomic_size_t progress = 0U;
    ValidationSystem validation{settings, dataset, model,
                                [&] {
                                    const bool fail = constructions++ == 0U;
                                    return std::make_unique<FakeNonvisualComputeRuntime>(ComputeScenario{.gate = gate, .fail = fail});
                                },
                                [&](ValidationSystem::event_type event) {
                                    if (std::holds_alternative<ValidationProgress>(event))
                                        ++progress;
                                    else
                                        terminals.Publish(std::move(event));
                                }};
    static_cast<void>(validation.Start({}));
    // CLEANUP-IGNORE: Validation Busy evidence is independent from file-dialog and dataset admission evidence.
    CHECK_THROWS_AS(validation.Start({}), contracts::BusyError);
    gate->Release();
    const auto failed_compute = terminals.First().get();
    REQUIRE(std::holds_alternative<ValidationChanged>(failed_compute));
    CHECK(std::get<ValidationChanged>(failed_compute).snapshot.operation.generation_frontier == 1U);
    CHECK(std::get<ValidationChanged>(failed_compute).snapshot.operation.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
    CHECK(progress == 0U);
    CHECK_FALSE(validation.snapshot().operation.active);
    gate = std::make_shared<StopGate>();
    static_cast<void>(validation.Start({}));
    static_cast<void>(validation.Stop());
    CHECK(std::holds_alternative<ValidationChanged>(terminals.Second().get()));
    CHECK(validation.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    gate->Release();
    static_cast<void>(validation.Start({}));
    CHECK(std::holds_alternative<ValidationChanged>(terminals.Third().get()));
    CHECK(validation.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(constructions == 2U);
    const auto measured = validation.snapshot();
    REQUIRE(measured.metrics);
    CHECK(measured.metrics->bbox.ap == 0.75);
    CHECK(measured.detail_rows == 5U);
    CHECK_FALSE(measured.metrics->mask);
    CHECK(validation.Details({measured.operation.generation_frontier, 0U, 4U}).rows.size() == 4U);
    const auto last = validation.Details({measured.operation.generation_frontier, 4U, 4U});
    REQUIRE(last.rows.size() == 1U);
    CHECK(last.rows.front().category == 4U);
    CHECK(validation.Details({measured.operation.generation_frontier, 5U, 1U}).rows.empty());
    CHECK_THROWS_AS(validation.Details({measured.operation.generation_frontier, 6U, 1U}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(validation.Details({measured.operation.generation_frontier, 0U, 5U}), contracts::InvalidIntentError);
    CHECK_THROWS_AS(validation.Details({measured.operation.generation_frontier - 1U, 0U, 1U}), contracts::InvalidIntentError);
    REQUIRE(last.rows.front().category_name);
    CHECK(last.rows.front().category_name->value == std::string(mmltk::backend::data::catalog::kClassNameCapacity, 'z'));
    CHECK_THROWS_AS(validation.Details({measured.operation.generation_frontier, 0U, 0U}), contracts::InvalidIntentError);
    contracts::SettingsUpdateRequest edit;
    edit.updates = {
        {.path = "workflows.validate.request.compiled_path",
         .value =
             mmltk::frameworks::serialization::wire::FlatValue::text((root / "later.bin").string(), mmltk::frameworks::reflection::kMaximumPathBytes).value()},
        {.path = "workflows.validate.request.weights_path",
         .value =
             mmltk::frameworks::serialization::wire::FlatValue::text((root / "later.pt").string(), mmltk::frameworks::reflection::kMaximumPathBytes).value()}};
    static_cast<void>(settings.Update(std::move(edit)));
    const auto retained = validation.Details({measured.operation.generation_frontier, 0U, 4U});
    REQUIRE(retained.rows.front().category_name);
    CHECK(retained.rows.front().category_name->value == "last in model");
    CHECK(retained.rows.back().category_name->value == "first in model");
    CHECK_FALSE(retained.rows[1].available);  // No-GT category still has its evaluated name.
}
struct PredictReaderComposition final {
    SettingsSystem* settings;
    PredictSystem* predict;
};
// Exercises real source observation/borrowing/paired metadata and Presentation
// routing; this writer supplies publication receipts, not Firefox GPU acceptance.
class PredictReaderWriter final : public PresentationNativeWriter {
   public:
    explicit PredictReaderWriter(std::function<void()> pumped = {}) : pumped_(std::move(pumped)) {}
    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        auto pixels = reader.borrow();
        if (!visual_product_matches_frame(submitted.observation.frame, pixels) || !reader.image_metadata(submitted.observation.frame))
            throw std::runtime_error("Predict materialized reader lost its exact image");
        pending_ = submitted;
    }
    PresentationNativeOutcome Pump(std::uint64_t generation) override {
        const mmltk::testsupport::ScopedTestCleanup notify{[&] {
            if (pumped_) pumped_();
        }};
        if (!pending_) return {};
        const auto submitted = *std::exchange(pending_, std::nullopt);
        if (submitted.selection_generation != generation) return {.progress = PresentationNativeProgress::Superseded, .submitted = submitted};
        ++sequence_;
        return {.progress = PresentationNativeProgress::Published,
                .submitted = submitted,
                .publication = {.capability = {.surface_high = 1U,
                                               .surface_low = 1U,
                                               .extent = submitted.observation.frame.extent,
                                               .generation = 1U,
                                               .condition = PresentationCapabilityCondition::Ready},
                                .timeline_ready = sequence_,
                                .presentation_revision = sequence_,
                                .transfer_sequence = sequence_}};
    }
    int poll_fd() const noexcept override { return -1; }
    int completion_fd() const noexcept override { return -1; }
    bool wants_write() const noexcept override { return false; }
    void SetExpectedBrowserProcessGroup(pid_t) override {}
    Retirement BrowserPeerLost() noexcept override { return Retirement::Released; }

   private:
    std::function<void()> pumped_;
    std::optional<PresentationSubmittedSource> pending_;
    std::uint64_t sequence_ = 0U;
};
TEST_CASE("Predict materialized routing keeps one producer across input changes and ordinary restart", "[controller][systems][predict][presentation]") {
    const bool invalid_first = GENERATE(false, true);
    const auto kind = GENERATE(contracts::SourceKind::CompiledDataset, contracts::SourceKind::SingleImage, contracts::SourceKind::VideoFile);
    const auto root = mmltk::testsupport::make_temp_root("predict-materialized-routing");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    const auto field = kind == contracts::SourceKind::CompiledDataset ? "workflows.predict.source.compiled_path"
                       : kind == contracts::SourceKind::SingleImage   ? "workflows.predict.source.single_image_path"
                                                                      : "workflows.predict.source.video_file_path";
    const auto select = [&](std::string path) {
        contracts::SettingsUpdateRequest update;
        update.updates = {
            {.path = "workflows.predict.source.kind", .value = static_cast<std::int64_t>(kind)},
            {.path = field, .value = mmltk::frameworks::serialization::wire::FlatValue::text(path, mmltk::frameworks::reflection::kMaximumPathBytes).value()}};
        static_cast<void>(settings.Update(std::move(update)));
    };
    select((root / "first.input").string());
    auto gate = std::make_shared<StopGate>();
    gate->Release();
    auto index = std::make_shared<std::atomic_int64_t>(0);
    std::atomic_size_t constructions = 0U;
    std::atomic<PresentationSystem*> route = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t terminal_generation = 0U;
    PresentationSnapshot displayed;
    std::string routing_failure;
    PredictSystem prediction{settings,
                             dataset,
                             model,
                             {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                             [&] {
                                 return std::make_unique<FakePredictRuntime>(
                                     PredictionScenario{.compute = {.gate = gate, .fail = invalid_first && constructions++ == 0U}, .source_index = index});
                             },
                             [&](PredictSystem::event_type event) {
                                 if (auto* presentation = route.load()) presentation->SourceChanged({PresentationSourceKind::Predict, 1U});
                                 std::visit(
                                     [&](const auto& value) {
                                         if (!value.snapshot.operation.active) {
                                             std::scoped_lock lock(mutex);
                                             terminal_generation = std::max(terminal_generation, value.snapshot.operation.generation_frontier);
                                             changed.notify_all();
                                         }
                                     },
                                     event);
                             }};
    const auto readers = browser::materialize_visual_source_readers(PredictReaderComposition{&settings, &prediction});
    REQUIRE(readers.size() == 1U);
    CHECK((readers[0].source == PresentationSourceIdentity{PresentationSourceKind::Predict, 1U}));
    CHECK_FALSE(prediction.ObserveSource().valid());
    CHECK(prediction.ObserveSource() == readers[0].observe());
    PresentationSystem presentation{{.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                                    [] { return std::make_unique<PredictReaderWriter>(); },
                                    readers,
                                    [&](PresentationSystem::event_type event) {
                                        std::scoped_lock lock(mutex);
                                        if (const auto* completed = std::get_if<PresentationCompleted>(&event)) displayed = completed->snapshot;
                                        if (const auto* failure = std::get_if<PresentationFailed>(&event)) routing_failure = failure->detail;
                                        changed.notify_all();
                                    }};
    route = &presentation;
    const mmltk::testsupport::ScopedTestCleanup stop{[&] {
        prediction.Shutdown();
        route = nullptr;
        presentation.BrowserPeerLost();
        static_cast<void>(presentation.Shutdown());
    }};
    static_cast<void>(presentation.Select(readers[0].source));
    const auto run = [&](bool expect_image) {
        const auto previous = prediction.ObserveSource().frame.revision;
        const auto admitted = prediction.Start({});
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return !routing_failure.empty() ||
                   (terminal_generation >= admitted.operation.generation_frontier && (!expect_image || displayed.completed.revision > previous));
        }));
        REQUIRE(routing_failure.empty());
        lock.unlock();
        return prediction.snapshot();
    };
    if (invalid_first) {
        CHECK(run(false).operation.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
        select((root / "valid-after-error.input").string());
    }
    const auto first = run(true);
    CHECK(first.frame.source == readers[0].source);
    REQUIRE(first.content_identity != 0U);
    CHECK(readers[0].observe().frame == first.frame);
    REQUIRE(prediction.ImageSnapshot(first.frame));
    CHECK(prediction.ImageSnapshot(first.frame)->content_identity == first.content_identity);
    select((root / "second.input").string());
    const auto second = run(true);
    CHECK(second.frame.source == first.frame.source);
    CHECK(second.content_identity != first.content_identity);
    const auto same = run(true);
    CHECK(same.content_identity == second.content_identity);
    CHECK(same.frame.revision > second.frame.revision);
    if (kind != contracts::SourceKind::SingleImage) {
        ++*index;
        const auto next = run(true);
        CHECK(next.image_id == same.image_id);
        CHECK((next.content_identity == same.content_identity) == (kind == contracts::SourceKind::VideoFile));
    }
}
TEST_CASE("Predict compact publication and source observation do not reread retained labels", "[controller][systems][predict][presentation]") {
    ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-compact-progress")};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto begin = std::make_shared<StopGate>();
    begin->Release();
    auto after_image = std::make_shared<StopGate>();
    std::atomic<PresentationSystem*> route = nullptr;
    std::atomic_bool scalar_sent = false;
    std::atomic_bool scalar_observed = false;
    std::atomic_size_t metadata_reads = 0U;
    std::promise<void> shown;
    std::promise<void> scalar_pumped;
    using Publisher = browser::ApplicationEventPublisher<&PredictReaderComposition::predict, PredictReaderComposition>;
    Publisher::Sink sink = [](browser::SystemEvent) {};
    Publisher publisher{sink, [] {},
                        [&](auto source) {
                            if (auto* selected = route.load()) selected->SourceChanged(source);
                        }};
    PredictSystem prediction{
        settings,
        dataset,
        model,
        {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [&] {
            return std::make_unique<FakePredictRuntime>(
                PredictionScenario{.compute = {.gate = begin}, .labels = contracts::kAnnotationObjectCapacity, .after_product = after_image});
        },
        [&](PredictSystem::event_type event) {
            if (const auto* progress = std::get_if<PredictProgress>(&event); progress && progress->snapshot.operation.progress.sequence == 2U)
                scalar_sent = true;
            publisher(event);
        }};
    auto readers = browser::materialize_visual_source_readers(PredictReaderComposition{&settings, &prediction});
    auto observe = std::move(readers[0].observe);
    readers[0].observe = [&, observe = std::move(observe)] {
        auto result = observe();
        if (scalar_sent) scalar_observed = true;
        return result;
    };
    auto metadata = std::move(readers[0].image_metadata);
    readers[0].image_metadata = [&, metadata = std::move(metadata)](const VisualFrame& frame) {
        ++metadata_reads;
        return metadata(frame);
    };
    PresentationSystem presentation{{.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                                    [&] {
                                        return std::make_unique<PredictReaderWriter>([&] {
                                            if (scalar_observed) mmltk::testsupport::release_test_promise(scalar_pumped);
                                        });
                                    },
                                    readers,
                                    [&](PresentationSystem::event_type event) {
                                        if (std::holds_alternative<PresentationCompleted>(event)) mmltk::testsupport::release_test_promise(shown);
                                    }};
    route = &presentation;
    const mmltk::testsupport::ScopedTestCleanup stop{[&] {
        after_image->Release();
        prediction.Shutdown();
        route = nullptr;
        presentation.BrowserPeerLost();
        static_cast<void>(presentation.Shutdown());
    }};
    static_cast<void>(presentation.Select(readers[0].source));
    static_cast<void>(prediction.Start({}));
    mmltk::testsupport::await_test_promise(shown, "full Predict label image");
    REQUIRE(prediction.snapshot().labels.size() == contracts::kAnnotationObjectCapacity);
    const auto before = prediction.ObserveSource();
    const auto reads = metadata_reads.load();
    REQUIRE(reads == 1U);
    after_image->Release();
    mmltk::testsupport::await_test_promise(scalar_pumped, "Predict progress presentation cycle");
    CHECK(prediction.ObserveSource() == before);
    CHECK(metadata_reads == reads);
    CHECK(prediction.snapshot().revision > before.snapshot_revision);
}
TEST_CASE("Predict seals unsafe execution and close custody across repeated admission", "[controller][systems][predict][custody]") {
    const bool on_close = GENERATE(false, true);
    const bool preview_terminal = GENERATE(false, true);
    ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-unsafe-admission")};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto custody = std::make_shared<int>(7);
    std::weak_ptr<int> retained = custody;
    std::atomic_size_t constructions = 0U;
    std::promise<void> failed;
    {
        PredictSystem prediction{settings,
                                 dataset,
                                 model,
                                 {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                                 [&] {
                                     ++constructions;
                                     return std::make_unique<UnsafePredictRuntime>(on_close, custody, preview_terminal);
                                 },
                                 [&](PredictSystem::event_type event) {
                                     if (const auto* failure = std::get_if<PredictFailed>(&event); failure && !failure->snapshot.operation.active)
                                         mmltk::testsupport::release_test_promise(failed);
                                 }};
        static_cast<void>(prediction.Start({}));
        mmltk::testsupport::await_test_promise(failed, "unsafe Predict settlement");
        REQUIRE_FALSE(prediction.snapshot().operation.active);
        for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
        CHECK(constructions == 1U);
        CHECK(*custody == (on_close && !preview_terminal ? 8 : 7));
        custody.reset();
    }
    CHECK_FALSE(retained.expired());
}
TEST_CASE("receiver retirement outlives concurrent preview pool destruction", "[controller][gpu][custody]") {
    namespace gpu = mmltk::frameworks::gpu;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity + 2U);
    PredictionReceiverFault fault;
    fault.enabled = fault.terminal = true;
    const ScopedPredictionReceiverFault registration{fault};
    const auto operations = PredictionReceiverFault::Operations();
    auto pool = std::make_unique<detail::PredictionPreviewPool>(execution, context, operations, authority);
    std::array<std::uint8_t, 48U> pixels;
    pixels.fill(127U);
    auto input = PredictionSource::Decoded({4U, 4U}, pixels);
    auto& decoded = input.custody();
    std::weak_ptr<void> retained = decoded;
    auto raw = pool->Capture(nullptr, input.extent(), 0U, {}, {}, input.classes(), 1, input.rgb8(), decoded);
    REQUIRE(raw);
    auto draw = std::async(std::launch::async, [raw, context] {
        try {
            gpu::SystemImageRuntime runtime({.device = 0,
                                             .context_mode = gpu::DeviceContextMode::Isolated,
                                             .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
                                             .adopted_context = context});
            auto candidate = runtime.AcquireOutput();
            raw->Draw(runtime, candidate);
        } catch (...) { return std::current_exception(); }
        return std::exception_ptr{};
    });
    const mmltk::testsupport::ScopedTestCleanup release{[&] {
        fault.upload.Release();
        if (draw.valid()) draw.wait();
    }};
    REQUIRE(fault.upload.WaitEntered(std::chrono::seconds{2}));
    CHECK_FALSE(pool->HasUnsafeSourceCustody());
    raw.reset();
    decoded.reset();
    pool.reset();  // the receiver transaction, not this replaceable shell, owns the fact
    CHECK(authority->admission_open());
    // The source frame and the in-flight composition reserve independent
    // custody. Terminal composition retains the real frame, including its lease.
    CHECK(authority->fact().reservations == 2U);
    fault.upload.Release();
    const auto failure = mmltk::testsupport::await_test_future(draw, "receiver completion after pool destruction");
    CHECK(gpu::is_image_execution_failure(failure));
    CHECK_FALSE(authority->admission_open());
    CHECK(authority->fact().occupancy == 1U);
    CHECK(authority->fact().reservations == 1U);
    CHECK_FALSE(retained.expired());
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS(detail::PredictionPreviewPool(execution, context, operations, authority));
    CHECK(authority->fact().occupancy == 1U);
}
TEST_CASE("late receiver custody seals Predict admission while optional visual failure preserves semantic success", "[controller][systems][predict][custody]") {
    const bool terminal = GENERATE(false, true);
    ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-late-receiver")};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    gate->Release();
    auto fault = std::make_shared<PredictionReceiverFault>();
    fault->terminal = terminal;
    const ScopedPredictionReceiverFault registration{*fault};
    std::atomic_size_t constructions = 0U;
    std::promise<void> first_frame, first_done, second_done, visual_failure, recovered;
    {
        PredictSystem prediction{settings,
                                 dataset,
                                 model,
                                 {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                                 [&] {
                                     ++constructions;
                                     return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .receiver_fault = fault});
                                 },
                                 [&](PredictSystem::event_type event) {
                                     if (const auto* changed = std::get_if<PredictChanged>(&event)) {
                                         const auto& state = changed->snapshot;
                                         if (state.frame.valid() && state.operation.generation_frontier == 1U)
                                             mmltk::testsupport::release_test_promise(first_frame);
                                         if (!state.operation.active) {
                                             if (state.operation.generation_frontier == 1U) mmltk::testsupport::release_test_promise(first_done);
                                             if (state.operation.generation_frontier == 2U) mmltk::testsupport::release_test_promise(second_done);
                                         }
                                         if (state.operation.generation_frontier == 3U && state.frame.revision > 1U)
                                             mmltk::testsupport::release_test_promise(recovered);
                                     }
                                     if (std::holds_alternative<PredictFailed>(event)) mmltk::testsupport::release_test_promise(visual_failure);
                                 }};
        const mmltk::testsupport::ScopedTestCleanup stop{[&] {
            fault->upload.Release();
            prediction.Shutdown();
        }};
        static_cast<void>(prediction.Start({}));
        mmltk::testsupport::await_test_promise(first_frame, "initial retained Predict image");
        mmltk::testsupport::await_test_promise(first_done, "initial semantic completion");
        const auto retained = prediction.snapshot().frame;
        fault->enabled = true;
        static_cast<void>(prediction.Start({}));
        REQUIRE(fault->upload.WaitEntered(std::chrono::seconds{2}));
        mmltk::testsupport::await_test_promise(second_done, "semantic completion before receiver outcome");
        CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
        CHECK(prediction.snapshot().frame == retained);
        REQUIRE(fault->retirement);
        CHECK(fault->retirement->admission_open());
        fault->upload.Release();
        mmltk::testsupport::await_test_promise(visual_failure, "late receiver failure");
        CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
        CHECK(prediction.snapshot().frame == retained);
        CHECK(fault->retirement->admission_open() == !terminal);
        if (terminal) {
            CHECK(fault->retirement->fact().occupancy == 1U);
            for (unsigned attempt = 0U; attempt < 4U; ++attempt) CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
            CHECK_FALSE(fault->decoded.expired());
        } else {
            fault->enabled = false;
            static_cast<void>(prediction.Start({}));
            mmltk::testsupport::await_test_promise(recovered, "safely settled visual recovery");
            CHECK(prediction.snapshot().frame.revision > retained.revision);
            CHECK(fault->retirement->fact().occupancy == 0U);
        }
        CHECK(constructions == 1U);
    }
    CHECK(fault->decoded.expired() == !terminal);
    if (terminal) {
        const auto facts = fault->retirement->fact();
        CHECK(facts.occupancy == 1U);
        CHECK(facts.occupancy + facts.reservations <= detail::PredictionPreviewPool::kSlotCapacity + 4U);
    }
}
TEST_CASE("prediction preview refusal preserves successful inference completion", "[controller][systems][compute]") {
    const auto root = mmltk::testsupport::make_temp_root("prediction-preview-refusal");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    gate->Release();
    std::promise<PredictSnapshot> completed;
    std::promise<PredictFailed> preview_failed;
    PredictSystem prediction{settings,
                             dataset,
                             model,
                             {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                             [gate] { return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .refuse_preview = true}); },
                             [&](PredictSystem::event_type event) {
                                 if (auto* failure = std::get_if<PredictFailed>(&event)) preview_failed.set_value(std::move(*failure));
                                 if (auto* changed = std::get_if<PredictChanged>(&event); changed && !changed->snapshot.operation.active)
                                     completed.set_value(std::move(changed->snapshot));
                             }};
    static_cast<void>(prediction.Start({}));
    const auto failure = mmltk::testsupport::await_test_promise(preview_failed, "prediction preview refusal");
    CHECK(failure.snapshot.operation.terminal.outcome != contracts::ComputeOperationOutcome::Failed);
    const auto terminal = mmltk::testsupport::await_test_promise(completed, "prediction completion after preview refusal");
    CHECK(terminal.operation.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(terminal.operation.terminal.completed == 2U);
    CHECK(terminal.operation.terminal.output == "result");
}
TEST_CASE("export and predict wrappers share Busy Stop and failure isolation", "[controller][systems][compute]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-export-predict");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Export);
    auto [settings, dataset, model] = fixture.systems();
    auto export_gate = std::make_shared<StopGate>();
    std::promise<ComputeSystemEvent> export_terminal;
    ExportSystem export_system{settings, dataset, model,
                               [export_gate] { return std::make_unique<FakeNonvisualComputeRuntime>(ComputeScenario{.gate = export_gate}); },
                               [&](ComputeSystemEvent event) {
                                   if (!std::holds_alternative<ComputeProgressEvent>(event)) export_terminal.set_value(std::move(event));
                               }};
    static_cast<void>(export_system.Start({}));
    CHECK_THROWS_AS(export_system.Start({}), contracts::BusyError);
    static_cast<void>(export_system.Stop());
    CHECK(std::holds_alternative<ComputeChanged>(export_terminal.get_future().get()));
    CHECK(export_system.snapshot().terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto predict_gate = std::make_shared<StopGate>();
    auto predictions = std::make_shared<std::atomic_size_t>(0U);
    std::atomic_size_t constructions = 0U;
    std::promise<PredictSystem::event_type> predict_failed;
    std::promise<PredictSystem::event_type> predict_succeeded;
    std::promise<PredictSystem::event_type> predict_cancelled;
    std::promise<void> prediction_frame;
    std::atomic_bool frame_seen = false;
    std::atomic_size_t predict_terminals = 0U;
    PredictSystem predict{
        settings,
        dataset,
        model,
        {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [&] {
            const bool fail = constructions++ == 0U;
            return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = predict_gate, .fail = fail}, .predictions = predictions});
        },
        [&](PredictSystem::event_type event) {
            if (const auto* changed = std::get_if<PredictChanged>(&event); changed && changed->snapshot.frame.valid() && !frame_seen.exchange(true))
                prediction_frame.set_value();
            const auto terminal = std::visit([](const auto& value) { return value.snapshot.operation.terminal.outcome; }, event);
            if (std::holds_alternative<PredictFailed>(event))
                predict_failed.set_value(std::move(event));
            else if (terminal == contracts::ComputeOperationOutcome::Succeeded && predict_terminals.fetch_add(1U) == 0U)
                predict_succeeded.set_value(std::move(event));
            else if (terminal == contracts::ComputeOperationOutcome::Cancelled)
                predict_cancelled.set_value(std::move(event));
        }};
    CHECK_FALSE(predict.BorrowFrame().valid());
    const auto first_admitted = predict.Start({});
    CHECK(first_admitted.revision > 0U);
    CHECK_FALSE(predict.BorrowFrame().valid());
    CHECK_THROWS_AS(predict.Start({}), contracts::BusyError);
    predict_gate->Release();
    CHECK(std::holds_alternative<PredictFailed>(predict_failed.get_future().get()));
    const auto failed_observation = predict.snapshot().revision;
    CHECK(failed_observation > first_admitted.revision);
    CHECK_FALSE(predict.BorrowFrame().valid());
    static_cast<void>(predict.Start({}));
    CHECK(std::holds_alternative<PredictChanged>(predict_succeeded.get_future().get()));
    mmltk::testsupport::await_test_promise(prediction_frame, "Predict completed frame publication");
    const auto completed_observation = predict.snapshot().revision;
    CHECK(completed_observation > failed_observation);
    CHECK(predict.snapshot().frame.valid());
    REQUIRE(predict.BorrowFrame().valid());
    {
        namespace gpu = mmltk::frameworks::gpu;
        gpu::test_support::VulkanWorkspaceFixture allocation(0, 4U, 4U);
        gpu::DeviceContext display(0, gpu::cuda_image_copy_backend());
        auto workspace = gpu::ImageWorkspace::Create(display, allocation.layout());
        REQUIRE(workspace->QueueAllocation(allocation.Export()));
        workspace->Admit(workspace->identity(), allocation.layout().device_incarnation);
        const auto observed = predict.ObserveWorkspace();
        const gpu::ImageWorkspaceContent expected{observed.product_owner, observed.product_revision};
        REQUIRE(expected.valid());
        auto raw = predict.BorrowFrame();
        REQUIRE(raw.valid());
        CHECK(raw.plane(0U).plane().allocation.owner == expected.owner);
        CHECK(raw.plane(0U).revision() == expected.revision);
        const auto source_calls = predictions->load();
        const auto request = [&] {
            auto ready = std::make_shared<std::promise<void>>();
            auto result = ready->get_future();
            predict.RequestWorkspace(
                {.product_owner = expected.owner, .product_revision = expected.revision, .destination = workspace, .ready = [ready] { ready->set_value(); }});
            return result;
        };
        auto ready = request();
        // The explicit raw receiver prevents preparation from taking exclusive
        // allocation access. No settling API is called to force readiness.
        CHECK_FALSE(workspace->Contains(expected));
        CHECK_FALSE(predict.BorrowWorkspace().valid());
        CHECK(ready.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
        raw = {};
        mmltk::testsupport::await_test_future(ready, "Predict native workspace completion");
        REQUIRE(workspace->Contains(expected));
        {
            auto completed = predict.BorrowWorkspace();
            REQUIRE(completed.valid());
            CHECK(completed.identity() == workspace->identity());
            CHECK(completed.revision() == expected.revision);
            display.Bind();
            std::array<std::uint8_t, 4U * 4U * 4U> pixels{};
            const auto plane = completed.plane();
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = plane.data;
            copy.srcPitch = plane.descriptor.pitch_bytes;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = pixels.data();
            copy.dstPitch = 4U * 4U;
            copy.WidthInBytes = 4U * 4U;
            copy.Height = 4U;
            REQUIRE(cuMemcpy2D(&copy) == CUDA_SUCCESS);
            CHECK(std::ranges::all_of(pixels, [](auto value) { return value == 255U; }));
        }
        ready = request();
        mmltk::testsupport::await_test_future(ready, "Predict retained workspace completion");
        CHECK(predict.BorrowWorkspace().revision() == expected.revision);
        CHECK(predict.ObserveWorkspace().product_owner == expected.owner);
        CHECK(predict.snapshot().revision == completed_observation);
        CHECK(predictions->load() == source_calls);
        CHECK(constructions == 2U);
        auto detached = std::make_shared<std::promise<void>>();
        auto released = detached->get_future();
        predict.RequestWorkspace({.detach_only = true, .destination = workspace, .ready = [detached] { detached->set_value(); }});
        mmltk::testsupport::await_test_future(released, "Predict workspace detach");
        workspace.reset();
    }
    predict_gate->Reset();
    const auto admitted = predict.Start({});
    CHECK(admitted.revision > completed_observation);
    CHECK(admitted.frame.valid());
    CHECK(predict.BorrowFrame().valid());
    const auto cancelled_request = predict.Stop({});
    CHECK(cancelled_request.revision > admitted.revision);
    CHECK(std::holds_alternative<PredictChanged>(predict_cancelled.get_future().get()));
    CHECK(predict.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    CHECK(predict.snapshot().revision >= cancelled_request.revision);
    CHECK(predict.snapshot().frame.valid());
    CHECK(predict.BorrowFrame().valid());
    CHECK(constructions == 2U);
}
TEST_CASE("training owns provider offers and remote control with Busy and lazy failure isolation", "[controller][systems][training]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training");
    ApplicationDataFixture fixture{root};
    // CLEANUP-IGNORE: Provider training intentionally starts without the accepted local-model prerequisite used by
    // local training.
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    TerminalSequence<TrainingSystem::event_type> terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions), [&](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingProgress>(event)) return;
                                terminals.Publish(std::move(event));
                            }};
    const auto admitted_query = training.Query({});
    CHECK_THROWS_AS(training.Query({}), contracts::BusyError);
    CHECK_THROWS_AS(training.Select({.offer_id = 17}), contracts::BusyError);
    gate->Release();
    const auto provider_failure = terminals.First().get();
    REQUIRE(std::holds_alternative<TrainingChanged>(provider_failure));
    CHECK(std::get<TrainingChanged>(provider_failure).snapshot.offers.revision > admitted_query.offers.revision);
    CHECK(std::get<TrainingChanged>(provider_failure).snapshot.offers.outcome == contracts::ProviderQueryOutcome::Failed);
    CHECK_FALSE(std::get<TrainingChanged>(provider_failure).snapshot.offers.detail.empty());
    const auto admitted_successful_query = training.Query({});
    CHECK(admitted_successful_query.offers.outcome == contracts::ProviderQueryOutcome::Idle);
    CHECK(admitted_successful_query.offers.detail.empty());
    CHECK_FALSE(admitted_successful_query.offers.cancellation_requested);
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Second().get()));
    REQUIRE(training.snapshot().offers.offers.size() == 1U);
    CHECK(training.snapshot().offers.revision >= admitted_successful_query.offers.revision);
    const auto before_selection_revision = training.snapshot().offers.revision;
    const auto selected = training.Select({.offer_id = 17});
    CHECK(selected.offers.selected->offer_id == 17);
    CHECK(selected.offers.revision > before_selection_revision);
    static_cast<void>(training.StartRemote({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Third().get()));
    CHECK(training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK(constructions == 2U);
}
TEST_CASE("local training resets progress and supports failure Stop and reconstruction", "[controller][systems][training][local]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-local-training");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    // CLEANUP-IGNORE: Local progress accounting and provider offer sequencing use different event invariants.
    std::atomic_size_t constructions = 0U;
    std::atomic_size_t sequence_one_progress = 0U;
    std::promise<void> first_successful_progress;
    auto first_successful_progress_observed = first_successful_progress.get_future();
    TerminalSequence<TrainingSystem::event_type> terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions), [&](TrainingSystem::event_type event) {
                                if (const auto* progress = std::get_if<TrainingProgress>(&event)) {
                                    if (progress->local.progress.sequence == 1U) {
                                        CHECK(progress->local.generation_frontier != 0U);
                                        if (sequence_one_progress.fetch_add(1U) == 0U) first_successful_progress.set_value();
                                    }
                                    return;
                                }
                                if (const auto* changed = std::get_if<TrainingChanged>(&event); changed && changed->snapshot.local.active) return;
                                terminals.Publish(std::move(event));
                            }};  // CLEANUP-IGNORE: Local training and validation start evidence targets independently typed terminal state.
    static_cast<void>(training.Start({}));
    CHECK_THROWS_AS(training.Start({}), contracts::BusyError);
    gate->Release();
    const auto local_failure = terminals.First().get();
    REQUIRE(std::holds_alternative<TrainingChanged>(local_failure));
    CHECK(std::get<TrainingChanged>(local_failure).snapshot.local.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
    CHECK(std::get<TrainingChanged>(local_failure).snapshot.local.generation_frontier == 1U);
    gate = std::make_shared<StopGate>();
    static_cast<void>(training.Start({}));
    first_successful_progress_observed.get();
    const auto before_rejected_clear = training.snapshot();
    CHECK_THROWS_AS(training.Clear({}), contracts::BusyError);
    CHECK(training.snapshot() == before_rejected_clear);
    // CLEANUP-IGNORE: Training cancellation and restart assert its local terminal, independently of validation metrics.
    static_cast<void>(training.Stop({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Second().get()));
    CHECK(training.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    gate->Release();
    static_cast<void>(training.Start({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Third().get()));
    CHECK(training.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(sequence_one_progress == 2U);
    CHECK(constructions == 2U);
}
TEST_CASE("remote training rejects duplicate starts and reconciles an inconclusive create", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-reconcile");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    gate->Release();
    std::promise<void> query_done;
    std::promise<void> mutation_done;
    std::promise<void> reconciliation_done;
    std::atomic_size_t changed = 0U;
    TrainingSystem training{settings, dataset, model, [gate] { return std::make_unique<FakeTrainingRuntime>(gate, false, true); },
                            [&](TrainingSystem::event_type event) {
                                if (!std::holds_alternative<TrainingChanged>(event)) return;
                                switch (changed++) {
                                    case 0U: query_done.set_value(); break;
                                    case 1U: mutation_done.set_value(); break;
                                    default: reconciliation_done.set_value(); break;
                                }
                            }};
    static_cast<void>(training.Query({}));
    query_done.get_future().wait();
    static_cast<void>(training.Select({.offer_id = 17}));
    static_cast<void>(training.StartRemote({}));
    mutation_done.get_future().wait();
    CHECK(training.snapshot().remote.reconciliation_pending);
    static_cast<void>(training.RetryReconciliation());
    reconciliation_done.get_future().wait();
    CHECK_FALSE(training.snapshot().remote.reconciliation_pending);
    CHECK(training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK_THROWS_AS(training.StartRemote({}), contracts::InvalidIntentError);
}
TEST_CASE("provider Clear cancels the active query and clears selection state", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-clear");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto query_gate = std::make_shared<StopGate>();
    std::promise<void> query_done;
    TrainingSystem training{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                            [&](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingChanged>(event)) query_done.set_value();
                            }};
    static_cast<void>(training.Query({}));
    const auto stopping = training.Clear({});
    CHECK(stopping.activity == TrainingActivity::ProviderQuery);
    CHECK(stopping.offers.cancellation_requested);
    query_done.get_future().wait();
    CHECK(training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Cancelled);
    CHECK_FALSE(training.snapshot().offers.cancellation_requested);
    CHECK(training.snapshot().offers.offers.empty());
    CHECK_FALSE(training.snapshot().offers.selected);
}
TEST_CASE("provider Clear is admitted once until the query worker settles", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-clear-once");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto probe = std::make_shared<QueryCancellationProbe>();
    auto started = probe->started.get_future();
    auto cancellation = probe->cancellation_observed.get_future();
    std::promise<void> settled;
    TrainingSystem training{settings, dataset, model, [probe] { return std::make_unique<BlockingCancellationTrainingRuntime>(probe); },
                            [&settled](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingChanged>(event)) settled.set_value();
                            }};
    static_cast<void>(training.Query({}));
    started.wait();
    const auto stopping = training.Clear({});
    REQUIRE(cancellation.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(stopping.offers.cancellation_requested);
    CHECK_THROWS_AS(training.Clear({}), contracts::BusyError);
    CHECK(training.snapshot() == stopping);
    probe->release.set_value();
    settled.get_future().wait();
    CHECK_FALSE(training.snapshot().offers.cancellation_requested);
    CHECK(training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Cancelled);
}
TEST_CASE("local Stop does not cancel provider query or remote effect", "[controller][systems][training][provider]") {
    // CLEANUP-IGNORE: Stop-isolation and Clear-cancellation are distinct provider ownership scenarios.
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-stop-ownership");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto query_gate = std::make_shared<StopGate>();
    std::promise<void> query_done;
    TrainingSystem query_training{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                                  [&](TrainingSystem::event_type event) {
                                      if (std::holds_alternative<TrainingChanged>(event)) query_done.set_value();
                                  }};
    static_cast<void>(query_training.Query({}));
    static_cast<void>(query_training.Stop({}));
    query_gate->Release();
    query_done.get_future().wait();
    CHECK(query_training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Succeeded);
    auto remote_gate = std::make_shared<StopGate>();
    std::promise<void> offers_ready;
    std::promise<void> remote_done;
    std::atomic_size_t changes = 0U;
    TrainingSystem remote_training{settings, dataset, model, [remote_gate] { return std::make_unique<BlockingRemoteRuntime>(remote_gate); },
                                   [&](TrainingSystem::event_type event) {
                                       if (!std::holds_alternative<TrainingChanged>(event)) return;
                                       if (changes++ == 0U)
                                           offers_ready.set_value();
                                       else
                                           remote_done.set_value();
                                   }};
    static_cast<void>(remote_training.Query({}));
    offers_ready.get_future().wait();
    static_cast<void>(remote_training.Select({.offer_id = 17}));
    const auto remote_admitted = remote_training.StartRemote({});
    CHECK(remote_admitted.remote.outcome == contracts::RemoteOperationOutcome::Idle);
    CHECK(remote_admitted.remote.detail.empty());
    static_cast<void>(remote_training.Stop({}));
    remote_gate->Release();
    remote_done.get_future().wait();
    CHECK(remote_training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK(remote_training.snapshot().remote.revision > remote_admitted.remote.revision);
}
[[nodiscard]] auto release_training_publication_on_exit(TrainingSystem& system, StopGate& runtime, std::promise<void>& publication) {
    return mmltk::testsupport::ScopedTestCleanup{[&system, &runtime, &publication] {
        mmltk::testsupport::release_test_promise(publication);
        runtime.Release();
        static_cast<void>(system.Stop({}));
    }};
}
TEST_CASE("training completion releases admission before observer publication returns", "[controller][systems][training][settlement]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-settlement");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto local_gate = std::make_shared<StopGate>();
    local_gate->Release();
    std::promise<void> local_publishing;
    std::promise<void> release_local_publication;
    const auto release_local = release_local_publication.get_future().share();
    TrainingSystem local{settings, dataset, model, [local_gate] { return std::make_unique<FakeTrainingRuntime>(local_gate, false); },
                         [&](TrainingSystem::event_type event) {
                             const auto* changed = std::get_if<TrainingChanged>(&event);
                             if (!changed || changed->snapshot.local.active) return;
                             local_publishing.set_value();
                             release_local.wait();
                         }};
    auto release_local_on_exit = release_training_publication_on_exit(local, *local_gate, release_local_publication);
    static_cast<void>(local.Start({}));
    auto publishing = local_publishing.get_future();
    REQUIRE(publishing.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    publishing.get();
    CHECK(local.Stop({}).local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(local.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    release_local_publication.set_value();
    auto query_gate = std::make_shared<StopGate>();
    query_gate->Release();
    std::promise<TrainingSnapshot> query_publishing;
    std::promise<void> release_query_publication;
    const auto release_query = release_query_publication.get_future().share();
    TrainingSystem query{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                         [&](TrainingSystem::event_type event) {
                             const auto* changed = std::get_if<TrainingChanged>(&event);
                             if (changed == nullptr) return;
                             query_publishing.set_value(changed->snapshot);
                             release_query.wait();
                         }};
    auto release_query_on_exit = release_training_publication_on_exit(query, *query_gate, release_query_publication);
    static_cast<void>(query.Query({}));
    auto query_publication = query_publishing.get_future();
    REQUIRE(query_publication.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    const auto published = query_publication.get();
    static_cast<void>(query.Stop({}));
    const auto cleared = query.Clear({});
    release_query_publication.set_value();
    CHECK(published.offers.outcome == contracts::ProviderQueryOutcome::Succeeded);
    REQUIRE(published.offers.offers.size() == 1U);
    CHECK(cleared.offers.outcome == contracts::ProviderQueryOutcome::Idle);
    CHECK(cleared.offers.offers.empty());
}
TEST_CASE("training admission and matching cancellation do not invert system and worker locks", "[controller][systems][training][admission]") {
    const bool provider_query = GENERATE(false, true);
    CAPTURE(provider_query);
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-admission");
    ApplicationDataFixture fixture{root};
    // CLEANUP-IGNORE: Admission-race setup selects local model facts; provider cancellation tests above deliberately
    // do not.
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto runtime_gate = std::make_shared<StopGate>();
    std::promise<void> done;
    TrainingSystem system{settings, dataset, model, [runtime_gate] { return std::make_unique<FakeTrainingRuntime>(runtime_gate, false); },
                          [&](TrainingSystem::event_type event) {
                              if (!std::holds_alternative<TrainingProgress>(event)) done.set_value();
                          }};
    mmltk::testsupport::TestGate admission{provider_query ? "concurrent provider Query and Clear admission" : "concurrent training Start and Stop admission"};
    std::future<TrainingSnapshot> starting;
    std::future<void> cancelling;
    mmltk::testsupport::ScopedTestCleanup release_runtime{[&] {
        admission.Release();
        runtime_gate->Release();
    }};
    starting = std::async(std::launch::async, [&] {
        admission.receipt().ArriveAndWait();
        return provider_query ? system.Query({}) : system.Start({});
    });
    cancelling = std::async(std::launch::async, [&] {
        admission.receipt().ArriveAndWait();
        if (provider_query) {
            try {
                static_cast<void>(system.Clear({}));
            } catch (const contracts::BusyError&) {}
        } else {
            static_cast<void>(system.Stop({}));
        }
    });
    REQUIRE(admission.WaitEntered(std::chrono::seconds{2}, 2U));
    admission.Release();
    const auto start_status = starting.wait_for(std::chrono::seconds{2});
    const auto cancel_status = cancelling.wait_for(std::chrono::seconds{2});
    if (start_status != std::future_status::ready || cancel_status != std::future_status::ready) runtime_gate->Release();
    REQUIRE(start_status == std::future_status::ready);
    REQUIRE(cancel_status == std::future_status::ready);
    CHECK_NOTHROW(static_cast<void>(starting.get()));
    CHECK_NOTHROW(cancelling.get());
    // Clear alone may reject busy provider work at admission. Local Stop must
    // never acquire that exception policy; each generated case settles its own API.
    if (provider_query)
        static_cast<void>(system.Clear({}));
    else
        static_cast<void>(system.Stop({}));
    auto terminal = done.get_future();
    REQUIRE(terminal.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    terminal.get();
}
}  // namespace
}  // namespace mmltk::controller
TEST_CASE("Local compute admission waits for required worker placement", "[controller][compute][placement]") {
    using namespace mmltk::common::system;
    using namespace mmltk::controller;
    const auto topology = NumaTopology::Capture();
    const auto cpu = topology.permitted_cpus.front();
    const auto node = std::ranges::find(topology.cpus, cpu, &CpuTopology::cpu)->node;
    const auto before = capture_execution_policy_snapshot();
    direct::LocalRun run;
    std::optional<ExecutionPolicySnapshot> observed;
    run.Start({
        .policy = ExecutionPolicyRequest{{cpu}, {}, 0, node, -10, false},
        .work = [&](std::stop_token) -> direct::LocalRun::Notification {
            observed = capture_execution_policy_snapshot();
            return {};
        },
    });
    run.StopAndJoin();
    REQUIRE(observed);
    CHECK(observed->affinity == std::vector<int>{cpu});
    CHECK(observed->numa_node == node);
    CHECK(observed->nice_value <= -10);
    CHECK(observed->scheduler_policy == SCHED_OTHER);
    CHECK(capture_execution_policy_snapshot().affinity == before.affinity);
}
TEST_CASE("Compute policy denial precedes admission and CUDA construction", "[controller][compute][placement]") {
    using namespace mmltk::common::system;
    using namespace mmltk::controller;
    for (const auto call : {SYS_setpriority, SYS_set_mempolicy}) {
        CHECK(mmltk::common::system::test_support::with_denied_syscall(call, [] {
                  const auto topology = NumaTopology::Capture();
                  const auto cpu = topology.permitted_cpus.front();
                  const auto node = std::ranges::find(topology.cpus, cpu, &CpuTopology::cpu)->node;
                  direct::LocalRun run;
                  bool admitted = false;
                  try {
                      run.Start({
                          .policy = ExecutionPolicyRequest{{cpu}, {}, 0, node, -10, false},
                          .prepare = [&] { admitted = true; },
                          .work = [](std::stop_token) -> direct::LocalRun::Notification { return {}; },
                      });
                  } catch (const std::system_error& error) {
                      if (admitted || run.active() || error.code().value() != EPERM) return false;
                      const DirectComputeConfiguration configuration{
                          .execution = mmltk::frameworks::gpu::DeviceExecution{.device = 999999, .placement = {.numa_node = node, .cpus = {cpu}}}};
                      for (const auto& construct : std::array<std::function<void()>, 3>{[&] { CudaValidationRuntime runtime(configuration); },
                                                                                        [&] { CudaExportRuntime runtime(configuration); },
                                                                                        [&] { CudaPredictRuntime runtime(configuration); }}) {
                          try {
                              construct();
                              return false;
                          } catch (const std::system_error& denied) {
                              if (denied.code().value() != EPERM) return false;
                          }
                      }
                      return true;
                  }
                  return false;
              }) == 0);
    }
}
TEST_CASE("prediction raw custody is bounded under retained readers and preserves pixels", "[controller][gpu]") {
    using namespace mmltk::controller;
    namespace gpu = mmltk::frameworks::gpu;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"first", "last"});
    const std::array<float, 12U> pixels{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {}, classes);
    const auto* source = input.pixels();
    auto& custody = input.custody();
    std::optional<detail::PredictionPreviewPool> pool;
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    pool.emplace(execution, context);
    std::array<std::shared_ptr<const detail::PredictionPreviewFrame>, 3U> readers;
    for (auto& reader : readers) {
        reader = pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
        REQUIRE(reader);
    }
    CHECK_FALSE(pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody));
    gpu::SystemImageRuntime runtime({.device = 0,
                                     .context_mode = gpu::DeviceContextMode::Isolated,
                                     .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
                                     .adopted_context = context});
    auto candidate = runtime.AcquireOutput();
    readers[0]->Draw(runtime, candidate);
    const auto complete = runtime.CommitOutput(std::move(candidate));
    REQUIRE(complete.valid());
    gpu::SystemImageRuntime::OutputCandidate invalid_candidate;
    CHECK_THROWS_AS(readers[0]->Draw(runtime, invalid_candidate), std::invalid_argument);
    CHECK(runtime.Completed().revision() == complete.revision());
    auto image = complete.Borrow();
    const auto plane = image.plane(0U).plane();
    std::array<std::uint8_t, 16U> rgba{};
    REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) ==
            cudaSuccess);
    CHECK(rgba == std::array<std::uint8_t, 16U>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
    // A replacement renderer rejects a pending old-context frame before touching its candidate.
    gpu::DeviceContext replacement_context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    gpu::SystemImageRuntime replacement({.device = 0,
                                         .context_mode = gpu::DeviceContextMode::Isolated,
                                         .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
                                         .adopted_context = replacement_context});
    CHECK(readers[0]->CompatibleWith(runtime));
    CHECK_FALSE(readers[0]->CompatibleWith(replacement));
    gpu::SystemImageRuntime::OutputCandidate untouched;
    CHECK_THROWS_AS(readers[0]->Draw(replacement, untouched), std::runtime_error);
    CHECK_FALSE(replacement.Completed().valid());
    CHECK(runtime.Completed().revision() == complete.revision());
    detail::PredictionPreviewPool replacement_pool(execution, replacement_context);
    auto next = replacement_pool.Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
    REQUIRE(next);
    CHECK(next->CompatibleWith(replacement));
    auto next_candidate = replacement.AcquireOutput();
    next->Draw(replacement, next_candidate);
    CHECK(replacement.CommitOutput(std::move(next_candidate)).valid());
    readers[1].reset();
    auto latest = pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody);
    REQUIRE(latest);
    auto invalid = execution;
    invalid.device = std::numeric_limits<int>::max();
    CHECK_THROWS_AS(detail::PredictionPreviewPool(invalid, context), std::invalid_argument);
    std::vector<mmltk::backend::models::rfdetr::Prediction> excessive(contracts::kAnnotationObjectCapacity + 1U);
    readers[2].reset();
    CHECK_THROWS_AS(pool->Capture(source, {2U, 2U}, 0U, excessive, {}, classes, 2, nullptr, custody), std::invalid_argument);
    gpu::DeviceContext other(0, gpu::cuda_image_copy_backend());
    other.Bind();
    CUcontext before = nullptr;
    REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
    latest.reset();
    readers = {};
    pool.reset();
    CUcontext after = nullptr;
    REQUIRE(cuCtxGetCurrent(&after) == CUDA_SUCCESS);
    CHECK(after == before);
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
}
TEST_CASE("prediction transfer faults settle or retain exact source custody", "[controller][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace controller = mmltk::controller;
    namespace runtime = mmltk::backend::ml::runtime;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
    const std::array<float, 12U> pixels{};
    auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {{.class_reference = 0, .score = .7F}}, classes);
    const auto* source = input.pixels();
    auto& custody = input.custody();
    const std::weak_ptr<void> lifetime = custody;
    const auto& annotations = input.annotations();
    const auto& predictions = input.detections();
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    PredictionTransferFault fault;
    const auto operations = PredictionTransferFault::Operations();
    auto pool = std::make_unique<controller::detail::PredictionPreviewPool>(execution, context, operations);
    for (int stage : {1, 2, 3, 4}) {
        fault.Reset({.fail_copy = stage < 4 ? stage : 0, .fail_record = stage == 4});
        CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody), std::runtime_error);
        CHECK(fault.settlements == 1);
        CHECK(custody.use_count() == 1);
        fault.Reset();
        auto recovered = pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody);
        REQUIRE(recovered);
        CHECK(fault.settlements == 0);
    }
    fault.Reset({.fail_copy = 2, .fail_settle = true});
    int stopped = 0;
    CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody, &CountPredictionSourceStop, &stopped),
                    runtime::CudaOperationError);
    CHECK(stopped == 1);
    CHECK(pool->HasUnsafeSourceCustody());
    const auto attempted = fault.copies;
    CHECK_THROWS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody));
    CHECK(fault.copies == attempted);
    custody.reset();
    CHECK_FALSE(lifetime.expired());
    pool.reset();
    CHECK_FALSE(lifetime.expired());
    fault.Reset();
}
TEST_CASE("preview slot reuse orders cross-stream writes and preserves fault custody", "[controller][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace runtime = mmltk::backend::ml::runtime;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t first_raw = nullptr, second_raw = nullptr;
    REQUIRE(cudaStreamCreateWithFlags(&first_raw, cudaStreamNonBlocking) == cudaSuccess);
    std::unique_ptr<std::remove_pointer_t<cudaStream_t>, decltype(&cudaStreamDestroy)> first(first_raw, &cudaStreamDestroy);
    REQUIRE(cudaStreamCreateWithFlags(&second_raw, cudaStreamNonBlocking) == cudaSuccess);
    std::unique_ptr<std::remove_pointer_t<cudaStream_t>, decltype(&cudaStreamDestroy)> second(second_raw, &cudaStreamDestroy);
    const auto classes = std::make_shared<const mmltk::backend::data::catalog::ClassCatalog>(std::vector<std::string>{"object"});
    const std::array<float, 12> pixels{};
    auto input = PredictionSource::Device(execution, {2U, 2U}, pixels, {}, classes);
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    PredictionTransferFault fault;
    for (int stage : {0, 1, 2, 3, 4}) {
        fault.Reset();
        mmltk::controller::detail::PredictionPreviewPool pool(execution, context, PredictionTransferFault::Operations(), {}, 1U);
        auto capture = [&](cudaStream_t stream) {
            return pool.Capture(input.pixels(), {2U, 2U}, reinterpret_cast<std::uintptr_t>(stream), {}, input.annotations(), classes, 1, nullptr,
                                input.custody());
        };
        auto previous = capture(first.get());
        REQUIRE(previous);
        previous.reset();  // The peer copy need not have completed or been drawn.
        fault.Reset({.fail_copy = stage == 2 || stage == 4 ? 1 : 0, .fail_record = stage == 3, .fail_settle = stage == 4, .fail_wait = stage == 1});
        if (stage == 0) {
            REQUIRE(capture(second.get()));
            CHECK(fault.settlements == 0);
        } else if (stage == 4) {
            CHECK_THROWS_AS(capture(second.get()), runtime::CudaOperationError);
            CHECK(pool.HasUnsafeSourceCustody());
        } else {
            CHECK_THROWS_AS(capture(second.get()), std::runtime_error);
            CHECK(fault.settlements == 1);
            CHECK_FALSE(pool.HasUnsafeCustody());
        }
        CHECK(fault.waits == 1);
        CHECK(fault.waited_stream == second.get());
        fault.Reset();
        if (stage != 4) REQUIRE(capture(first.get()));
    }
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
}
TEST_CASE("ordinary preview allocation refusal leaves its decoded source intact", "[controller][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace controller = mmltk::controller;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    const auto operations = RefusePinnedRegistration();
    controller::detail::PredictionPreviewPool pool(execution, context, operations);
    const std::array<std::uint8_t, 12U> pixels{255, 0, 0};
    auto input = PredictionSource::Decoded({2U, 2U}, pixels);
    auto& source = input.custody();
    const auto& classes = input.classes();
    auto raw = pool.Capture(nullptr, input.extent(), 0, {}, {}, classes, 1, input.rgb8(), source);
    REQUIRE(raw);
    CHECK(source.use_count() == 2);
    gpu::SystemImageRuntime runtime({.device = 0,
                                     .context_mode = gpu::DeviceContextMode::Isolated,
                                     .output_layout = gpu::ImageProductLayout::CleanAndSemantic,
                                     .adopted_context = context});
    auto candidate = runtime.AcquireOutput();
    CHECK_THROWS(raw->Draw(runtime, candidate));
    CHECK(input.rgb8()[0] == 255U);
    candidate = {};
    controller::detail::PredictionPreviewPool healthy(execution, context);
    auto recovered = healthy.Capture(nullptr, {2, 2}, 0, {}, {}, classes, 1, input.rgb8(), source);
    REQUIRE(recovered);
    auto output = runtime.AcquireOutput();
    recovered->Draw(runtime, output);
    const auto completed = runtime.CommitOutput(std::move(output));
    auto view = completed.Borrow();
    const auto plane = view.plane(0U).plane();
    std::array<std::uint8_t, 16> rgba{};
    REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) ==
            cudaSuccess);
    CHECK(rgba[0] == 255U);
    CHECK(rgba[1] == 0U);
    CHECK(rgba[2] == 0U);
    CHECK(rgba[3] == 255U);
}
namespace mmltk::controller {
TEST_CASE("preview context failure retains initialized state and source before returning", "[controller][gpu][context]") {
    namespace gpu = mmltk::frameworks::gpu;
    const bool query_failure = GENERATE(false, true);
    const bool terminal = GENERATE(false, true);
    enum class Stage { Capture, Draw, Destruction };
    const auto stage = GENERATE(Stage::Capture, Stage::Draw, Stage::Destruction);
    PredictionContextFault driver{query_failure ? PredictionContextFault::Failure::Query
                                  : terminal    ? PredictionContextFault::Failure::RestoreAlways
                                                : PredictionContextFault::Failure::RestoreOnce};
    const auto api = driver.Api();
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity);
    detail::PredictionPreviewPool::TransferOperations operations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister};
    operations.context_api = api;
    auto pool = std::make_unique<detail::PredictionPreviewPool>(execution, context, operations, authority);
    const std::array<std::uint8_t, 12U> pixels{};
    auto input = PredictionSource::Decoded({2U, 2U}, pixels);
    auto& decoded = input.custody();
    const std::weak_ptr<void> retained = decoded;
    const auto& classes = input.classes();
    int stopped = 0;
    const auto capture = [&] { return pool->Capture(nullptr, {2U, 2U}, 0U, {}, {}, classes, 1, input.rgb8(), decoded, &CountPredictionSourceStop, &stopped); };
    const mmltk::testsupport::ScopedTestCleanup disarm{[&] { driver.armed = false; }};
    if (stage == Stage::Destruction) {
        auto raw = capture();
        REQUIRE(raw);
    }
    const bool unsafe = query_failure || terminal;
    if (stage == Stage::Draw) {
        auto raw = capture();
        REQUIRE(raw);
        gpu::SystemImageRuntime runtime({.device = 0, .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
        auto candidate = runtime.AcquireOutput();
        const mmltk::testsupport::ScopedTestCleanup disarm_draw{[&] { driver.armed = false; }};
        driver.armed = true;
        if (unsafe) {
            CHECK_THROWS_AS(raw->Draw(runtime, candidate), gpu::ImageStreamExecutionFailure);
            CHECK_THROWS(runtime.BeginWork());
            CHECK_FALSE(runtime.Retire().safe_to_destroy);
        } else {
            CHECK_THROWS_AS(raw->Draw(runtime, candidate), gpu::CudaContextFailure);
            CHECK_NOTHROW(runtime.BeginWork());
        }
        driver.armed = false;
        raw.reset();
        pool.reset();
    } else if (stage == Stage::Destruction) {
        driver.armed = true;
        pool.reset();
    } else {
        driver.armed = true;
        if (unsafe)
            CHECK_THROWS_AS(capture(), mmltk::backend::ml::runtime::CudaOperationError);
        else
            CHECK_THROWS_AS(capture(), gpu::CudaContextFailure);
        CHECK(stopped == (unsafe ? 1 : 0));
        if (unsafe) {
            const auto calls = driver.calls;
            CHECK_THROWS(capture());
            CHECK(driver.calls == calls);
        }
        pool.reset();
    }
    CHECK(authority->admission_open() == !unsafe);
    CHECK(authority->fact().occupancy == (unsafe ? 1U : 0U));
    decoded.reset();
    // A destructor restore failure occurs after settled source custody releases.
    if (stage == Stage::Capture || query_failure) CHECK(retained.expired() == !unsafe);
    driver.armed = false;
}
TEST_CASE("Predict replacement pressure coalesces without overwriting its selected image", "[controller][systems][predict][gpu]") {
    ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-replacement-pressure")};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<StopGate>();
    gate->Release();
    auto fault = std::make_shared<PredictionReceiverFault>();
    const ScopedPredictionReceiverFault registration{*fault};
    auto source_index = std::make_shared<std::atomic_int64_t>(0);
    std::array<std::promise<void>, 6U> done;
    std::array<std::promise<void>, 3U> images;
    std::promise<void> failed;
    std::atomic_size_t published = 0U;
    std::atomic_uint64_t last_revision = 0U;
    PredictSystem prediction{
        settings,
        dataset,
        model,
        {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [&] { return std::make_unique<FakePredictRuntime>(PredictionScenario{.compute = {.gate = gate}, .source_index = source_index, .labels = 1U}); },
        [&](PredictSystem::event_type event) {
            if (const auto* changed = std::get_if<PredictChanged>(&event)) {
                const auto& snapshot = changed->snapshot;
                if (!snapshot.operation.active && snapshot.operation.generation_frontier <= done.size())
                    mmltk::testsupport::release_test_promise(done[snapshot.operation.generation_frontier - 1U]);
                if (snapshot.frame.valid() && snapshot.frame.revision > last_revision.load()) {
                    last_revision = snapshot.frame.revision;
                    const auto index = published.fetch_add(1U);
                    if (index < images.size()) mmltk::testsupport::release_test_promise(images[index]);
                }
            }
            if (std::holds_alternative<PredictFailed>(event)) mmltk::testsupport::release_test_promise(failed);
        }};
    const mmltk::testsupport::ScopedTestCleanup stop{[&] { prediction.Shutdown(); }};
    const auto run = [&](std::size_t index) {
        source_index->store(static_cast<std::int64_t>(index));
        static_cast<void>(prediction.Start({}));
        mmltk::testsupport::await_test_promise(done[index], "Predict semantic completion under display pressure");
    };
    run(0U);
    mmltk::testsupport::await_test_promise(images[0U], "first Predict slot");
    auto old_reader = prediction.BorrowFrame();
    REQUIRE(old_reader.valid());
    run(1U);
    mmltk::testsupport::await_test_promise(images[1U], "selected Predict slot");
    const auto selected = prediction.snapshot();
    const auto pixels = [&] {
        auto borrowed = prediction.BorrowFrame();
        REQUIRE(borrowed.valid());
        const auto plane = borrowed.plane(0U).plane();
        borrowed.plane(0U).context().Bind();
        std::array<std::uint8_t, 64U> result{};
        REQUIRE(cudaMemcpy2D(result.data(), 16U, reinterpret_cast<void*>(plane.data), plane.descriptor.pitch_bytes, 16U, 4U, cudaMemcpyDeviceToHost) ==
                cudaSuccess);
        return result;
    };
    const auto selected_pixels = pixels();
    const auto check_metadata = [&] {
        const auto metadata = prediction.ImageSnapshot(selected.frame);
        REQUIRE(metadata);
        CHECK(metadata->frame == selected.frame);
        CHECK(metadata->content_identity == selected.content_identity);
        CHECK(metadata->image_id == selected.image_id);
        REQUIRE(metadata->labels.size() == selected.labels.size());
        for (std::size_t index = 0U; index < selected.labels.size(); ++index) {
            CHECK(metadata->labels[index].name == selected.labels[index].name);
            CHECK(metadata->labels[index].box == selected.labels[index].box);
            CHECK(metadata->labels[index].confidence == selected.labels[index].confidence);
            CHECK(metadata->labels[index].class_reference == selected.labels[index].class_reference);
            CHECK(metadata->labels[index].class_domain == selected.labels[index].class_domain);
            CHECK(metadata->labels[index].color == selected.labels[index].color);
        }
    };
    fault->partial_draw = true;
    run(2U);
    run(3U);  // bounded latest-product replacement while the other output is held
    CHECK(published == 2U);
    CHECK(prediction.snapshot().frame == selected.frame);
    check_metadata();
    CHECK(pixels() == selected_pixels);
    old_reader = {};
    mmltk::testsupport::await_test_promise(failed, "partial replacement draw failure after reader release");
    CHECK(prediction.snapshot().frame == selected.frame);
    check_metadata();
    CHECK(pixels() == selected_pixels);
    fault->partial_draw = false;
    old_reader = prediction.BorrowFrame();
    run(4U);
    mmltk::testsupport::await_test_promise(images[2U], "successful replacement recovery");
    CHECK(prediction.snapshot().content_identity > selected.content_identity);
    CHECK(prediction.snapshot().frame.revision > selected.frame.revision);
    CHECK(prediction.BorrowFrame().valid());
    gate->Reset();
    static_cast<void>(prediction.Start({}));
    auto stopping = std::async(std::launch::async, [&] { return prediction.Stop({}); });
    static_cast<void>(mmltk::testsupport::await_test_future(stopping, "Stop while old output remains borrowed"));
    mmltk::testsupport::await_test_promise(done[5U], "cancelled Predict execution with both display roles occupied");
    CHECK_FALSE(prediction.snapshot().operation.active);
    CHECK(prediction.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
}
TEST_CASE("preview context construction publishes only after exact caller restoration", "[controller][gpu][context]") {
    namespace gpu = mmltk::frameworks::gpu;
    using Failure = PredictionContextFault::Failure;
    const auto failure = GENERATE(Failure::None, Failure::Query, Failure::RestoreOnce, Failure::RestoreAlways);
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    int devices = 0;
    REQUIRE(cudaGetDeviceCount(&devices) == cudaSuccess);
    gpu::DeviceContext caller(devices > 1 ? 1 : 0, gpu::cuda_image_copy_backend());
    caller.Bind();
    PredictionContextFault driver{failure};
    driver.armed = true;
    driver.observe_candidate = true;
    CUcontext previous{};
    REQUIRE(cuCtxGetCurrent(&previous) == CUDA_SUCCESS);
    const auto api = driver.Api();
    auto authority = std::make_shared<gpu::TerminalCudaRetirementOwner>(detail::PredictionPreviewPool::kSlotCapacity + 3U);
    // Existing current, in-flight and pending frames can reserve all five other slots.
    std::array<gpu::TerminalCudaRetirementLease, detail::PredictionPreviewPool::kSlotCapacity + 2U> frames;
    for (auto& lease : frames) lease = gpu::ReserveTerminalCudaLease(*authority);
    std::optional<gpu::DeviceContext> published;
    const auto create = [&] { published = detail::CreatePredictionPreviewContext(execution, authority, api); };
    if (failure == Failure::None)
        CHECK_NOTHROW(create());
    else
        CHECK_THROWS_AS(create(), gpu::CudaContextFailure);
    const bool terminal = failure == Failure::Query || failure == Failure::RestoreAlways;
    CHECK(published.has_value() == (failure == Failure::None));
    CHECK(authority->admission_open() == !terminal);
    CHECK(authority->fact().occupancy == (terminal ? 1U : 0U));
    CHECK(authority->fact().reservations == frames.size());
    CUcontext restored{};
    REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
    CHECK(restored == previous);
    if (failure == Failure::RestoreAlways) {
        REQUIRE(driver.candidate != previous);
        unsigned version{};
        CHECK(cuCtxGetApiVersion(driver.candidate, &version) == CUDA_SUCCESS);
    }
}
}  // namespace mmltk::controller
TEST_CASE("CUDA export and validation preserve cancelled outcomes and prior artifacts", "[controller][compute][gpu]") {
    namespace controller = mmltk::controller;
    namespace r = mmltk::backend::models::rfdetr;
    namespace io = mmltk::common::io;
    const mmltk::testsupport::ScopedTempDir root("compute-stopped-artifact");
    const auto output = root.path() / "model.output";
    const auto companion = std::filesystem::path(output.string() + ".classes.json");
    const auto layout = r::native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog({"cat"}));
    {
        std::ofstream file(output);
        file << "completed artifact";
    }
    const auto previous = io::sha256_file(output);
    {
        std::ofstream file(companion);
        file << r::encode_class_descriptor({1, io::sha256_hex(previous), layout});
    }
    const auto previous_companion = io::sha256_file(companion);
    const controller::DirectComputeConfiguration configuration{
        .execution = mmltk::frameworks::gpu::test_support::selected_test_device(0, mmltk::common::system::NumaTopology::Capture())};
    controller::CudaExportRuntime exporter(configuration);
    controller::CudaValidationRuntime validator(configuration);
    std::stop_source stop;
    stop.request_stop();
    r::ExportOnnxRequest onnx;
    onnx.weights_path = root.path() / "not-opened.pt";
    onnx.output_path = output;
    CHECK(exporter.Run(onnx, stop.get_token(), {}).outcome == controller::contracts::ComputeOperationOutcome::Cancelled);
    r::BuildEngineRequest engine;
    engine.onnx_path = root.path() / "not-opened.onnx";
    engine.output_path = output;
    CHECK(exporter.Run(engine, stop.get_token(), {}).outcome == controller::contracts::ComputeOperationOutcome::Cancelled);
    r::ValidateRequest validation;
    validation.onnx_path = engine.onnx_path;
    validation.save_engine_path = output;
    validation.compiled_path = root.path() / "not-opened.bin";
    validation.eval_order = "tensorrt";
    CHECK(validator.Run(validation, stop.get_token(), {}, {}).terminal.outcome == controller::contracts::ComputeOperationOutcome::Cancelled);
    CHECK(io::sha256_file(output) == previous);
    CHECK(io::sha256_file(companion) == previous_companion);
    for (const auto& entry : std::filesystem::directory_iterator(root.path())) CHECK_FALSE(entry.is_directory());
}
TEST_CASE("artifact model inspection observes cancellation before each opaque input loader", "[controller][systems][model]") {
    namespace contracts = mmltk::controller::contracts;
    const mmltk::testsupport::ScopedTempDir root("model-inspection-stop");
    const auto artifact = root.path() / "selected.model";
    {
        std::ofstream output(artifact);
        output << "loader must not consume these bytes";
    }
    for (const auto input :
         {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx, contracts::ModelArtifactInputKind::TensorRt}) {
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
TEST_CASE("compilation captures its perceptual selection independently of augmentation", "[controller][systems][dataset][perceptual]") {
    namespace contracts = mmltk::controller::contracts;
    namespace services = mmltk::controller::services;
    contracts::GuiSettingsState settings;
    settings.workflows.train.compile_perceptual_downscale = true;
    settings.workflows.train.request.gpu_augmentation.enabled = false;
    settings.workflows.train.request.gpu_augmentation.perceptual_downscale = false;
    const auto captured = services::materialize_artifact_compile(settings);
    REQUIRE(captured);
    CHECK(captured->perceptual_downscale);
    settings.workflows.train.compile_perceptual_downscale = false;
    settings.workflows.train.request.gpu_augmentation.perceptual_downscale = true;
    const auto next = services::materialize_artifact_compile(settings);
    REQUIRE(next);
    CHECK_FALSE(next->perceptual_downscale);
    CHECK(captured->perceptual_downscale);
}
