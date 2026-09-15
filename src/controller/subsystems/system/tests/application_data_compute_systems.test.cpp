#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_event_publisher.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include <cuda_runtime_api.h>
#include "src/acceptance/tests/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

#include "filesystem_test_utils.hpp"
#include "mmltk/frameworks/reflection/member_relation.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/persistence_storage.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/compute_systems.h"
#include "src/controller/subsystems/system/detail/predict_revision.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/train/training_system.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"

namespace mmltk::controller {
namespace {

TEST_CASE("Predict revision capacity preserves cancellation and terminal observations", "[controller][systems][predict]") {
    using Revision = detail::PredictRevision;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (std::uint64_t reserved = 0U; reserved <= 5U; ++reserved)
        CHECK_THROWS_AS(Revision::Admit(maximum - reserved), contracts::FailedError);
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

class StopGate final {
   public:
    void Release() {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    void Reset() {
        std::scoped_lock lock(mutex_);
        released_ = false;
    }

    [[nodiscard]] bool Wait(const std::stop_token stop) {
        std::unique_lock lock(mutex_);
        return condition_.wait(lock, stop, [this] { return released_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool released_ = false;
};

class TrainingTerminals final {
   public:
    using Event = TrainingSystem::event_type;

    void Publish(Event event) {
        switch (next_.fetch_add(1U)) {
            case 0U:
                first_.set_value(std::move(event));
                break;
            case 1U:
                second_.set_value(std::move(event));
                break;
            default:
                third_.set_value(std::move(event));
                break;
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

[[nodiscard]] services::SettingsLocation install_settings(const std::filesystem::path& root) {
    auto settings = contracts::default_gui_settings_state();
    settings.workflows.train.dataset_source_dir = root / "source";
    settings.workflows.train.compiled_dataset_dir = root / "compiled";
    settings.workflows.train.request.train_compiled_path = root / "train.bin";
    settings.workflows.train.request.val_compiled_path = root / "val.bin";
    settings.workflows.train.request.test_compiled_path.clear();
    settings.workflows.train.request.weights_path = root / "weights.pt";
    settings.workflows.train.request.output_dir = root / "training";
    settings.workflows.train.model_source = contracts::ModelSelectionSource::Custom;
    settings.workflows.train.model_input = contracts::ModelArtifactInputKind::Weights;
    settings.workflows.train.remote_container_image = "mmltk-test";
    settings.workflows.train.remote_launch_template = "mmltk-test";
    settings.workflows.validate.request.compiled_path = root / "val.bin";
    settings.workflows.validate.request.weights_path = root / "weights.pt";
    settings.workflows.validate.model_source = contracts::ModelSelectionSource::Custom;
    settings.workflows.validate.model_input = contracts::ModelArtifactInputKind::Weights;
    settings.workflows.predict.source.compiled_path = (root / "train.bin").string();
    settings.workflows.predict.request.output_path = root / "prediction.json";
    settings.workflows.predict.request.weights_path = root / "weights.pt";
    settings.workflows.predict.model_source = contracts::ModelSelectionSource::Custom;
    settings.workflows.predict.model_input = contracts::ModelArtifactInputKind::Weights;
    settings.workflows.export_state.weights_path = root / "weights.pt";
    settings.workflows.export_state.onnx_input_path = root / "model-input.onnx";
    settings.workflows.export_state.onnx_output_path = root / "model-output.onnx";
    settings.workflows.export_state.model_source = contracts::ModelSelectionSource::Custom;
    settings.workflows.export_state.model_input = contracts::ModelArtifactInputKind::Onnx;
    REQUIRE(contracts::gui_settings_valid(settings));
    const services::SettingsLocation location{(root / "settings.json").string()};
    REQUIRE(
        services::save_persistence_settings(location, services::make_persistence_settings_snapshot(std::move(settings)), 1U).succeeded());
    return location;
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

[[nodiscard]] contracts::ArtifactSplitFact split(const std::filesystem::path& path) {
    return {.path = path.string(),
            .image_count = 1U,
            .width = 16U,
            .height = 16U,
            .channels = 3U,
            .max_instances_per_image = 1U,
            .class_names = {{.value = "object"}}};
}

[[nodiscard]] contracts::ModelSelection export_model_selection(const contracts::GuiSettingsState& settings,
                                                               const contracts::ModelArtifactInputKind input, std::string artifact) {
    return {
        .key = {.workflow = contracts::FeatureId::Export,
                .source = contracts::ModelSelectionSource::Custom,
                .input = input,
                .preset = settings.workflows.export_state.preset_name,
                .resolution = static_cast<std::uint32_t>(settings.workflows.export_state.model_resolution)},
        .artifact = std::move(artifact),
    };
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
    const contracts::ArtifactInspection inspected{.compatible = true,
        .splits = {split("/first/compiled.mmltk"), split("/second/compiled.mmltk")}, .detail = {}};
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
            case contracts::FeatureId::Train:
                contracts::apply_model_artifacts(settings.workflows.train, artifacts);
                break;
            case contracts::FeatureId::Validate:
                contracts::apply_model_artifacts(settings.workflows.validate, artifacts);
                break;
            case contracts::FeatureId::Predict:
                contracts::apply_model_artifacts(settings.workflows.predict, artifacts);
                break;
            case contracts::FeatureId::Export:
                settings.workflows.export_state.build_tensorrt = build_tensorrt;
                contracts::apply_model_artifacts(settings.workflows.export_state, artifacts);
                break;
            case contracts::FeatureId::Annotate:
            case contracts::FeatureId::Live:
            case contracts::FeatureId::Explore:
                FAIL("test case requires a ModelSystem workflow");
        }

        const auto* compatibility = workflow == contracts::FeatureId::Export
                                        ? contracts::find_model_selection_compatibility(workflow, selected_input, build_tensorrt)
                                        : contracts::find_model_selection_compatibility(workflow, selected_input);
        const bool expected = selected_input != contracts::ModelArtifactInputKind::None && compatibility != nullptr &&
                              contracts::model_selection_source_allowed(*compatibility, source);

        CAPTURE(workflow, source, selected_input, build_tensorrt);
        const auto result = subsystems::system::ComputeIntentMaterializer::ModelInputFor(settings, workflow);
        CHECK(result.has_value() == expected);
        if (!result) return;
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
                case contracts::ModelArtifactInputKind::Weights:
                    CHECK(result->custom_artifact == artifacts.weights_path);
                    break;
                case contracts::ModelArtifactInputKind::Onnx:
                    CHECK(result->custom_artifact == artifacts.onnx_path);
                    break;
                case contracts::ModelArtifactInputKind::TensorRt:
                    CHECK(result->custom_artifact == artifacts.tensorrt_path);
                    break;
                case contracts::ModelArtifactInputKind::None:
                    FAIL("materialized model input cannot be None");
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
    const auto malformed = subsystems::system::ComputeIntentMaterializer::ModelInputFor(
        unsupported, static_cast<contracts::FeatureId>(std::numeric_limits<std::uint8_t>::max()));
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

class FakeDatasetRuntime final : public DatasetRuntime {
   public:
    FakeDatasetRuntime(std::shared_ptr<StopGate> gate, const bool fail) : gate_(std::move(gate)), fail_(fail) {}

    services::ArtifactCompileResult Compile(const services::ArtifactCompileRequest& request, const std::stop_token stop,
                                            const std::function<void(const contracts::ArtifactProgress&)>& progress) override {
        progress(fail_ ? contracts::ArtifactProgress{.phase = static_cast<contracts::ArtifactCompilePhase>(255U),
                                                     .activity = std::string(contracts::kArtifactProgressTextCapacity + 1U, 'x'),
                                                     .completed = 2U,
                                                     .total = 1U}
                       : contracts::ArtifactProgress{
                             .phase = contracts::ArtifactCompilePhase::Pixels, .activity = "compiling", .completed = 1U, .total = 2U});
        if (!gate_->Wait(stop)) return {.output = request.output, .cancelled = true};
        if (fail_)
            return {.output = request.output,
                    .inspection = {.compatible = true,
                                   .splits = {split(request.output / "train.bin"), split(request.output / "val.bin"),
                                              split(request.output / "test.bin"), split(request.output / "overflow.bin")},
                                   .detail = {}}};
        return {.output = request.output,
                .inspection = {.compatible = true, .splits = {split(request.output / "train.bin")}, .detail = {}}};
    }

    contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths,
                                          std::string_view, std::uint32_t, std::stop_token) override {
        contracts::ArtifactInspection result{.compatible = true, .splits = {}, .detail = {}};
        for (const auto& path : paths)
            if (!path.empty()) result.splits.push_back(split(path));
        return result;
    }

   private:
    std::shared_ptr<StopGate> gate_;
    bool fail_ = false;
};

class BlockingModelRuntime final : public ModelRuntime {
   public:
    explicit BlockingModelRuntime(std::shared_ptr<StopGate> gate) : gate_(std::move(gate)) {}
    std::string Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path& custom, const std::stop_token stop,
                        const std::function<void(const contracts::ModelProgress&)>& progress) override {
        progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "verifying fixture model"});
        if (!gate_->Wait(stop)) throw std::runtime_error("fixture model selection cancelled");
        return custom.string();
    }

   private:
    std::shared_ptr<StopGate> gate_;
};

class ApplicationDataFixture final {
   public:
    explicit ApplicationDataFixture(std::filesystem::path root)
        : root_(std::move(root)),
          loaded_(root_),
          dataset_gate_(std::make_shared<StopGate>()),
          dataset_(loaded_.settings, [gate = dataset_gate_] { return std::make_unique<FakeDatasetRuntime>(gate, false); }),
          model_(
              loaded_.settings, [] { return std::make_unique<ArtifactModelRuntime>(); },
              [this](ModelSystem::event_type event) {
                  if (std::holds_alternative<ModelChanged>(event)) {
                      {
                          std::scoped_lock lock(model_mutex_);
                          model_terminals_.push_back(std::get<ModelChanged>(std::move(event)).snapshot);
                      }
                      model_changed_.notify_all();
                  }
              }) {
        dataset_gate_->Release();
    }

    void PrepareModel(const contracts::FeatureId workflow = contracts::FeatureId::Train) {
        std::ofstream(root_ / "train.bin").put('\0');
        std::ofstream(root_ / "val.bin").put('\0');
        std::ofstream(root_ / "weights.pt").put('\0');
        std::ofstream(root_ / "model-input.onnx").put('\0');
        const auto settings = loaded_.settings.materialization_facts();
        REQUIRE(settings.loaded);
        const auto expected = subsystems::system::ComputeIntentMaterializer::ModelInputFor(settings.settings, workflow);
        REQUIRE(expected.has_value());
        REQUIRE(expected->key.source == contracts::ModelSelectionSource::Custom);
        std::size_t terminal_index = 0U;
        {
            std::scoped_lock lock(model_mutex_);
            terminal_index = model_terminals_.size();
        }
        const auto admitted = model_.Select({.workflow = workflow});
        REQUIRE(admitted.active);
        std::unique_lock lock(model_mutex_);
        model_changed_.wait(lock, [&] { return model_terminals_.size() > terminal_index; });
        const auto accepted = model_terminals_[terminal_index];
        REQUIRE(accepted.terminal.outcome == contracts::ModelSelectionOutcome::Accepted);
        CHECK(accepted.selection.key == expected->key);
        CHECK(accepted.selection.artifact == expected->custom_artifact);
    }

    [[nodiscard]] auto systems() noexcept { return std::tie(loaded_.settings, dataset_, model_); }

   private:
    class LoadedSettings final {
       public:
        explicit LoadedSettings(const std::filesystem::path& root) { REQUIRE(settings.Load(install_settings(root)).applied()); }
        SettingsSystem settings;
    };

    std::filesystem::path root_;
    LoadedSettings loaded_;
    std::shared_ptr<StopGate> dataset_gate_;
    DatasetSystem dataset_;
    std::mutex model_mutex_;
    std::condition_variable model_changed_;
    std::vector<contracts::ModelUiState> model_terminals_;
    ModelSystem model_;
};

struct DatasetRuntimeObservation final {
    std::shared_ptr<StopGate> inspect_gate = std::make_shared<StopGate>();
    std::promise<void> inspect_started;
    std::atomic_int active_calls = 0;
    std::atomic_int maximum_active_calls = 0;
    std::atomic_int compile_calls = 0;
};

class BlockingInspectRuntime final : public DatasetRuntime {
   public:
    explicit BlockingInspectRuntime(std::shared_ptr<DatasetRuntimeObservation> observation) : observation_(std::move(observation)) {}

    services::ArtifactCompileResult Compile(const services::ArtifactCompileRequest& request, std::stop_token,
                                            const std::function<void(const contracts::ArtifactProgress&)>&) override {
        ++observation_->compile_calls;
        Enter();
        const auto result = services::ArtifactCompileResult{
            .output = request.output, .inspection = {.compatible = true, .splits = {split(request.output / "train.bin")}, .detail = {}}};
        Leave();
        return result;
    }

    contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths,
                                          std::string_view, std::uint32_t, const std::stop_token stop) override {
        Enter();
        observation_->inspect_started.set_value();
        static_cast<void>(observation_->inspect_gate->Wait(stop));
        contracts::ArtifactInspection result{.compatible = true, .splits = {}, .detail = {}};
        for (const auto& path : paths)
            if (!path.empty()) result.splits.push_back(split(path));
        Leave();
        return result;
    }

   private:
    void Enter() noexcept {
        const int active = observation_->active_calls.fetch_add(1) + 1;
        int maximum = observation_->maximum_active_calls.load();
        while (maximum < active && !observation_->maximum_active_calls.compare_exchange_weak(maximum, active)) {}
    }
    void Leave() noexcept { --observation_->active_calls; }
    std::shared_ptr<DatasetRuntimeObservation> observation_;
};

struct PredictionReceiverFault final {
    mmltk::testsupport::TestGate upload{"prediction receiver upload"};
    bool terminal = false;
    bool enabled = false;
    PredictRuntime::PreviewRetirement retirement;
    std::weak_ptr<std::array<std::uint8_t, 48U>> decoded;
};
std::atomic<PredictionReceiverFault*> prediction_receiver_fault = nullptr;
cudaError_t prediction_receiver_upload(void* destination, const void* source, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream) {
    const auto copied = cudaMemcpyAsync(destination, source, bytes, kind, stream);
    if (copied != cudaSuccess) return copied;
    auto* fault = prediction_receiver_fault.load();
    if (!fault || !fault->enabled) return cudaSuccess;
    // The actual receiver upload and pinned allocation exist before injection.
    // Settle test work before reporting an unobservable receiver outcome.
    const auto settled = cudaStreamSynchronize(stream);
    fault->upload.receipt().ArriveAndWait();
    if (fault->terminal) throw mmltk::frameworks::gpu::ImageStreamExecutionFailure(
        std::make_exception_ptr(std::runtime_error("injected receiver completion failure")));
    return settled == cudaSuccess ? cudaErrorMemoryAllocation : settled;
}

class FakeComputeRuntime final : public ValidationRuntime, public ExportRuntime, public PredictRuntime {
   public:
    FakeComputeRuntime(std::shared_ptr<StopGate> gate, const bool fail, std::shared_ptr<std::atomic_size_t> predictions = {}, bool refuse_preview = false, std::shared_ptr<std::atomic_int64_t> source_index = {},
                       std::size_t labels = 0U, std::shared_ptr<StopGate> after_product = {}, std::shared_ptr<PredictionReceiverFault> receiver_fault = {})
        : gate_(std::move(gate)), fail_(fail), predictions_(std::move(predictions)), refuse_preview_(refuse_preview), source_index_(std::move(source_index)), labels_(labels), after_product_(std::move(after_product)), receiver_fault_(std::move(receiver_fault)) {}

    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, const std::stop_token stop,
                                   const ComputeProgressSink& progress) override {
        return RunImpl(stop, progress);
    }
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, const std::stop_token stop,
                                   const ComputeProgressSink& progress) override {
        return RunImpl(stop, progress);
    }
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, const std::stop_token stop,
                                const ComputeProgressSink& progress, const ProductSink& products, const PlaybackGate&, VisualExtent, const ContextProvider& current_context, const PreviewRetirement& retirement) override {
        if (predictions_) ++*predictions_;
        auto terminal = RunImpl(stop, progress);
        if (terminal.outcome != contracts::ComputeOperationOutcome::Succeeded) return terminal;
        if (refuse_preview_) {
            products(std::unexpected{std::string{"Prediction preview exceeds the visual object capacity"}});
            return terminal;
        }
        const auto execution = mmltk::frameworks::gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
        const auto context = current_context();
        if (!context) return terminal;
        if (!preview_) preview_ = std::make_unique<detail::PredictionPreviewPool>(execution, *context,
            detail::PredictionPreviewPool::TransferOperations{&cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister, &prediction_receiver_upload}, retirement);
        if (receiver_fault_ && receiver_fault_->enabled) {
            receiver_fault_->retirement = retirement;
            auto decoded = std::make_shared<std::array<std::uint8_t, 48U>>();
            decoded->fill(255U);
            receiver_fault_->decoded = decoded;
            auto raw = preview_->Capture(nullptr, {4U, 4U}, 0U, {}, {},
                std::make_shared<const std::vector<std::string>>(), 1, decoded->data(), decoded);
            products(Product{.extent = {4U, 4U}, .raw = std::move(raw), .image_id = 41});
            return terminal;
        }
        if (cudaSetDevice(0) != cudaSuccess) throw std::runtime_error("test CUDA device unavailable");
        float* pixels = nullptr;
        if (cudaMalloc(reinterpret_cast<void**>(&pixels), (48U + 5U * labels_) * sizeof(float)) != cudaSuccess) throw std::bad_alloc();
        const std::shared_ptr<void> custody(pixels, [](void* address) { static_cast<void>(cudaFree(address)); });
        std::vector<float> white(48U + 5U * labels_, 0.0F);
        std::fill_n(white.begin(), 48U, 1.0F);
        std::vector<mmltk::backend::models::rfdetr::Prediction> detections(labels_,
            {.category_id = 1, .score = .75F, .bbox_xyxy = {0.0F, 0.0F, 3.0F, 3.0F}});
        for (std::size_t i = 0U; i < labels_; ++i) {
            white[48U + 4U * i + 2U] = 3.0F;
            white[48U + 4U * i + 3U] = 3.0F;
        }
        if (cudaMemcpy(pixels, white.data(), white.size() * sizeof(float), cudaMemcpyHostToDevice) != cudaSuccess) throw std::runtime_error("test upload failed");
        mmltk::backend::ml::runtime::AnalysisAnnotationStorage annotations{
            .boxes_xyxy = {.address = reinterpret_cast<std::uintptr_t>(pixels + 48U), .capacity_bytes = labels_ * 4U * sizeof(float)},
            .category_ids = {.address = reinterpret_cast<std::uintptr_t>(pixels + 48U + 4U * labels_), .capacity_bytes = labels_ * sizeof(std::int32_t)}};
        auto raw = preview_->Capture(pixels, {4U, 4U}, 0U, detections, annotations,
            std::make_shared<const std::vector<std::string>>(std::vector<std::string>{std::string(256U, 'p')}), 1, nullptr, custody);
        products(Product{.extent = {4U, 4U}, .raw = std::move(raw), .image_id = 41,
            .source_index = source_index_ ? source_index_->load() : 0});
        if (after_product_ && after_product_->Wait(stop)) progress({2U, 2U, 2U, "Processed"});
        return terminal;
    }

   private:
    contracts::ComputeTerminal RunImpl(const std::stop_token stop, const ComputeProgressSink& progress) {
        progress(fail_ ? contracts::ComputeProgress{.sequence = 0U,
                                                    .completed = 2U,
                                                    .total = 1U,
                                                    .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
                       : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 2U, .status = "running"});
        if (!gate_->Wait(stop)) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
        if (fail_)
            return {.outcome = static_cast<contracts::ComputeOperationOutcome>(255U),
                    .output = std::string(contracts::kComputePathCapacity + 1U, 'x'),
                    .detail = std::string(contracts::kComputeErrorCapacity + 1U, 'x')};
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded, 0U, 2U, "result");
    }

    std::shared_ptr<StopGate> gate_;
    bool fail_ = false;
    std::shared_ptr<std::atomic_size_t> predictions_;
    bool refuse_preview_ = false;
    std::shared_ptr<std::atomic_int64_t> source_index_;
    std::size_t labels_ = 0U;
    std::shared_ptr<StopGate> after_product_;
    std::shared_ptr<PredictionReceiverFault> receiver_fault_;
    std::unique_ptr<detail::PredictionPreviewPool> preview_;
};

class UnsafePredictRuntime final : public PredictRuntime {
   public:
    UnsafePredictRuntime(bool on_close, std::shared_ptr<int> custody) : on_close_(on_close), custody_(std::move(custody)) {}
    void Close() noexcept override { unsafe_ = true; }
    [[nodiscard]] bool HasUnsafeCustody() const noexcept override { return unsafe_; }
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token,
        const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent, const ContextProvider&, const PreviewRetirement&) override {
        unsafe_ = !on_close_;
        throw std::runtime_error("test prediction failure");
    }
   private:
    bool on_close_;
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
                                     const std::function<void(const contracts::ComputeProgress&)>& progress) override {
        progress(fail_ ? contracts::ComputeProgress{.sequence = 0U, .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
                       : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 1U, .status = "trained"});
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

    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&,
                                           contracts::ProviderOfferIdentity, int, std::string_view, std::stop_token) override {
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

[[nodiscard]] TrainingSystem::RuntimeFactory reconstructing_training_runtime(std::shared_ptr<StopGate>& gate,
                                                                             std::atomic_size_t& constructions) {
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
                                     const std::function<void(const contracts::ComputeProgress&)>&) override {
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
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&,
                                           contracts::ProviderOfferIdentity, int, std::string_view, std::stop_token) override {
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
    void compile_directory(const std::filesystem::path&, const std::filesystem::path&, std::uint32_t,
                           mmltk::common::concurrency::CancellationObservation, services::ArtifactProgressObserver) const override {}
    void compile_benchmark(const std::filesystem::path&, std::uint32_t, mmltk::common::concurrency::CancellationObservation,
                           services::ArtifactProgressObserver, services::ArtifactBenchmarkTraceObserver trace) const override {
        trace("benchmark.direct", R"({"records":1})");
    }
};  // CLEANUP-IGNORE: Diagnostic compiler and blocking training runtime are separate typed dependency fakes.

class BlockingRemoteRuntime final : public TrainingRuntime {
   public:
    explicit BlockingRemoteRuntime(std::shared_ptr<StopGate> remote_gate) : remote_gate_(std::move(remote_gate)) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                     const std::function<void(const contracts::ComputeProgress&)>&) override {
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, std::stop_token) override {
        return {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {provider_offer()}};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&,
                                           contracts::ProviderOfferIdentity, int, std::string_view, const std::stop_token stop) override {
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
                                    case 0U:
                                        first_completion.set_value(std::move(event));
                                        throw std::runtime_error("observer failure");
                                    case 1U:
                                        second_completion.set_value(std::move(event));
                                        break;
                                    default:
                                        third_completion.set_value(std::move(event));
                                        break;
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
    const auto diagnostics = services::ArtifactDiagnosticObserver{
        .benchmark = {.context = &traces, .report = [](const void* context, std::string_view, std::string_view) noexcept {
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
    std::jthread first{
        update, contracts::SettingsValueUpdate{.path = "ui.dark_mode", .value = mmltk::frameworks::serialization::wire::FlatValue{true}}};
    std::jthread second{update,
                        contracts::SettingsValueUpdate{.path = "ui.annotation_brush_radius",
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
    using TrainRecipeRelation =
        mmltk::frameworks::reflection::catalog_provider_relation<mmltk::backend::models::rfdetr::TrainRecipeCatalog>;
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
    CHECK_THROWS_AS(settings.persist_explore_class_catalog(candidate, rejected_identity, candidate.preferences.policy),
                    contracts::FailedError);

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
        request.updates.emplace_back(contracts::SettingsValueUpdate{
            .path = "ui.annotation_brush_radius", .value = mmltk::frameworks::serialization::wire::FlatValue{std::int64_t{9}}});
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

TEST_CASE("dataset publishes direct progress, returns Busy, stops locally, and reconstructs after failure",
          "[controller][systems][dataset]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    std::promise<DatasetSystem::event_type> first_terminal;
    std::promise<DatasetSystem::event_type> second_terminal;
    std::promise<DatasetSystem::event_type> third_terminal;
    std::atomic_size_t terminals = 0U;
    std::atomic_size_t progress = 0U;
    DatasetSystem dataset{settings,
                          [&] {
                              const bool fail = constructions++ == 0U;
                              return std::make_unique<FakeDatasetRuntime>(gate, fail);
                          },
                          [&](DatasetSystem::event_type event) {
                              if (std::holds_alternative<DatasetProgress>(event))
                                  ++progress;
                              else {
                                  switch (terminals++) {
                                      case 0U:
                                          first_terminal.set_value(std::move(event));
                                          break;
                                      case 1U:
                                          second_terminal.set_value(std::move(event));
                                          break;
                                      default:
                                          third_terminal.set_value(std::move(event));
                                          break;
                                  }
                              }
                          }};
    static_cast<void>(dataset.Compile({}));
    CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
    // CLEANUP-IGNORE: Compile and Inspect are separate dataset admission endpoints sharing one system-owned runtime.
    CHECK_THROWS_AS(dataset.Inspect({}, "rf-detr-base", 560U), contracts::BusyError);
    gate->Release();
    const auto failed_dataset = first_terminal.get_future().get();
    REQUIRE(std::holds_alternative<DatasetChanged>(failed_dataset));
    CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.generation == 1U);
    CHECK(std::get<DatasetChanged>(failed_dataset).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
    CHECK(progress == 0U);
    CHECK_FALSE(dataset.snapshot().active);

    gate = std::make_shared<StopGate>();
    static_cast<void>(dataset.Compile({}));
    static_cast<void>(dataset.Stop());
    CHECK(std::holds_alternative<DatasetChanged>(second_terminal.get_future().get()));
    CHECK(dataset.snapshot().terminal.outcome == contracts::ArtifactTerminalOutcome::Cancelled);
    gate->Release();
    static_cast<void>(dataset.Compile({}));
    CHECK(std::holds_alternative<DatasetChanged>(third_terminal.get_future().get()));
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

TEST_CASE("model selection shutdown cancels and joins one active acquisition without publishing a selection",
          "[controller][systems][model]") {
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

TEST_CASE("model and compute systems use direct facts, progress, Busy, Stop, and lazy runtime reconstruction",
          "[controller][systems][compute]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-compute");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Validate);
    auto [settings, dataset, model] = fixture.systems();

    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0U;
    std::promise<ComputeSystemEvent> first_terminal;
    std::promise<ComputeSystemEvent> second_terminal;
    std::promise<ComputeSystemEvent> third_terminal;
    std::atomic_size_t terminals = 0U;
    std::atomic_size_t progress = 0U;
    ValidationSystem validation{settings, dataset, model,
                                [&] {
                                    const bool fail = constructions++ == 0U;
                                    return std::make_unique<FakeComputeRuntime>(gate, fail);
                                },
                                // CLEANUP-IGNORE: Compute and dataset event sequences prove distinct typed system contracts.
                                [&](ComputeSystemEvent event) {
                                    if (std::holds_alternative<ComputeProgressEvent>(event))
                                        ++progress;  // CLEANUP-IGNORE: This oracle consumes the distinct typed compute
                                                     // event stream; dataset events are validated independently above.
                                    else {
                                        switch (terminals++) {
                                            case 0U:
                                                first_terminal.set_value(std::move(event));
                                                break;
                                            case 1U:
                                                second_terminal.set_value(std::move(event));
                                                break;
                                            default:
                                                third_terminal.set_value(std::move(event));
                                                break;
                                        }
                                    }
                                }};
    static_cast<void>(validation.Start({}));
    // CLEANUP-IGNORE: Validation Busy evidence is independent from file-dialog and dataset admission evidence.
    CHECK_THROWS_AS(validation.Start({}), contracts::BusyError);
    gate->Release();
    const auto failed_compute = first_terminal.get_future().get();
    REQUIRE(std::holds_alternative<ComputeChanged>(failed_compute));
    CHECK(std::get<ComputeChanged>(failed_compute).snapshot.generation_frontier == 1U);
    CHECK(std::get<ComputeChanged>(failed_compute).snapshot.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
    CHECK(progress == 0U);
    CHECK_FALSE(validation.snapshot().active);

    gate = std::make_shared<StopGate>();
    static_cast<void>(validation.Start({}));
    static_cast<void>(validation.Stop());
    CHECK(std::holds_alternative<ComputeChanged>(second_terminal.get_future().get()));
    CHECK(validation.snapshot().terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    gate->Release();
    static_cast<void>(validation.Start({}));
    CHECK(std::holds_alternative<ComputeChanged>(third_terminal.get_future().get()));
    CHECK(validation.snapshot().terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(constructions == 2U);
}

TEST_CASE("validation admits asynchronous selected-path inspection and cancels before compute", "[controller][systems][compute][admission]") {
    const auto root = mmltk::testsupport::make_temp_root("validation-inspection-admission");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel(contracts::FeatureId::Validate);
    auto [settings, unused_dataset, model] = fixture.systems();
    auto observation = std::make_shared<DatasetRuntimeObservation>();
    DatasetSystem dataset{settings, [observation] { return std::make_unique<BlockingInspectRuntime>(observation); }};
    auto gate = std::make_shared<StopGate>();
    std::atomic_size_t constructions = 0;
    std::promise<ComputeSystemEvent> settled;
    ValidationSystem validation{settings, dataset, model, [&] {
        ++constructions;
        return std::make_unique<FakeComputeRuntime>(gate, false);
    }, [&](ComputeSystemEvent event) {
        if (std::holds_alternative<ComputeChanged>(event)) settled.set_value(std::move(event));
    }};
    const auto admitted = validation.Start({});
    CHECK(admitted.active);
    observation->inspect_started.get_future().wait();
    CHECK(constructions == 0U);
    CHECK_THROWS_AS(dataset.Compile({}), contracts::BusyError);
    static_cast<void>(validation.Stop());
    const auto terminal = settled.get_future().get();
    CHECK(std::get<ComputeChanged>(terminal).snapshot.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    CHECK(constructions == 0U);
    CHECK(observation->compile_calls == 0);
}

struct PredictReaderComposition final { SettingsSystem* settings; PredictSystem* predict; };

// Exercises real source observation/borrowing/paired metadata and Presentation
// routing; this writer supplies publication receipts, not Firefox GPU acceptance.
class PredictReaderWriter final : public PresentationNativeWriter {
   public:
    explicit PredictReaderWriter(std::function<void()> pumped = {}) : pumped_(std::move(pumped)) {}
    void Submit(PresentationSubmittedSource submitted, const VisualSourceReader& reader) override {
        auto pixels = reader.borrow();
        if (!visual_product_matches_frame(submitted.observation.frame, pixels) ||
            !reader.image_metadata(submitted.observation.frame))
            throw std::runtime_error("Predict materialized reader lost its exact image");
        pending_ = submitted;
    }
    PresentationNativeOutcome Pump(std::uint64_t generation) override {
        const mmltk::testsupport::ScopedTestCleanup notify{[&] { if (pumped_) pumped_(); }};
        if (!pending_) return {};
        const auto submitted = *std::exchange(pending_, std::nullopt);
        if (submitted.selection_generation != generation)
            return {.progress = PresentationNativeProgress::Superseded, .submitted = submitted};
        ++sequence_;
        return {.progress = PresentationNativeProgress::Published, .submitted = submitted,
            .publication = {.capability = {.surface_high = 1U, .surface_low = 1U,
                .extent = submitted.observation.frame.extent, .generation = 1U,
                .condition = PresentationCapabilityCondition::Ready},
                .timeline_ready = sequence_, .presentation_revision = sequence_, .transfer_sequence = sequence_}};
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
    const auto field = kind == contracts::SourceKind::CompiledDataset ? "workflows.predict.source.compiled_path" :
        kind == contracts::SourceKind::SingleImage ? "workflows.predict.source.single_image_path" : "workflows.predict.source.video_file_path";
    const auto select = [&](std::string path) {
        contracts::SettingsUpdateRequest update;
        update.updates = {{.path = "workflows.predict.source.kind", .value = static_cast<std::int64_t>(kind)},
                          {.path = field, .value = std::move(path)}};
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
    PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [&] { return std::make_unique<FakeComputeRuntime>(gate, invalid_first && constructions++ == 0U, nullptr, false, index); },
        [&](PredictSystem::event_type event) {
            if (auto* presentation = route.load()) presentation->SourceChanged({PresentationSourceKind::Predict, 1U});
            std::visit([&](const auto& value) {
                if (!value.snapshot.operation.active) {
                    std::scoped_lock lock(mutex);
                    terminal_generation = std::max(terminal_generation, value.snapshot.operation.generation_frontier);
                    changed.notify_all();
                }
            }, event);
        }};
    const auto readers = browser::materialize_visual_source_readers(PredictReaderComposition{&settings, &prediction});
    REQUIRE(readers.size() == 1U);
    CHECK((readers[0].source == PresentationSourceIdentity{PresentationSourceKind::Predict, 1U}));
    CHECK_FALSE(prediction.ObserveSource().valid());
    CHECK(prediction.ObserveSource() == readers[0].observe());
    PresentationSystem presentation{{.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [] { return std::make_unique<PredictReaderWriter>(); }, readers,
        [&](PresentationSystem::event_type event) {
            std::scoped_lock lock(mutex);
            if (const auto* completed = std::get_if<PresentationCompleted>(&event)) displayed = completed->snapshot;
            if (const auto* failure = std::get_if<PresentationFailed>(&event)) routing_failure = failure->detail;
            changed.notify_all();
        }};
    route = &presentation;
    const mmltk::testsupport::ScopedTestCleanup stop{[&] { prediction.Shutdown(); route = nullptr; }};
    static_cast<void>(presentation.Select(readers[0].source));
    const auto run = [&](bool expect_image) {
        const auto previous = prediction.ObserveSource().frame.revision;
        const auto admitted = prediction.Start({});
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return !routing_failure.empty() || (terminal_generation >= admitted.operation.generation_frontier &&
                (!expect_image || displayed.completed.revision > previous));
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
    Publisher publisher{sink, [] {}, [&](auto source) { if (auto* selected = route.load()) selected->SourceChanged(source); }};
    PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [&] { return std::make_unique<FakeComputeRuntime>(begin, false, nullptr, false, nullptr,
            contracts::kAnnotationObjectCapacity, after_image); },
        [&](PredictSystem::event_type event) {
            if (const auto* progress = std::get_if<PredictProgress>(&event);
                progress && progress->snapshot.operation.progress.sequence == 2U) scalar_sent = true;
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
        [&] { return std::make_unique<PredictReaderWriter>([&] {
            if (scalar_observed) mmltk::testsupport::release_test_promise(scalar_pumped);
        }); }, readers, [&](PresentationSystem::event_type event) {
            if (std::holds_alternative<PresentationCompleted>(event)) mmltk::testsupport::release_test_promise(shown);
        }};
    route = &presentation;
    const mmltk::testsupport::ScopedTestCleanup stop{[&] { after_image->Release(); prediction.Shutdown(); route = nullptr; }};
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
    ApplicationDataFixture fixture{mmltk::testsupport::make_temp_root("predict-unsafe-admission")};
    fixture.PrepareModel(contracts::FeatureId::Predict);
    auto [settings, dataset, model] = fixture.systems();
    auto custody = std::make_shared<int>(7);
    std::weak_ptr<int> retained = custody;
    std::atomic_size_t constructions = 0U;
    std::promise<void> failed;
    {
        PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
            [&] { ++constructions; return std::make_unique<UnsafePredictRuntime>(on_close, custody); },
            [&](PredictSystem::event_type event) {
                if (const auto* failure = std::get_if<PredictFailed>(&event);
                    failure && !failure->snapshot.operation.active) mmltk::testsupport::release_test_promise(failed);
            }};
        static_cast<void>(prediction.Start({}));
        mmltk::testsupport::await_test_promise(failed, "unsafe Predict settlement");
        REQUIRE_FALSE(prediction.snapshot().operation.active);
        for (unsigned attempt = 0U; attempt < 4U; ++attempt)
            CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
        CHECK(constructions == 1U);
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
    prediction_receiver_fault = &fault;
    const mmltk::testsupport::ScopedTestCleanup clear{[&] { fault.upload.Release(); prediction_receiver_fault = nullptr; }};
    const detail::PredictionPreviewPool::TransferOperations operations{
        &cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize, &cuMemHostRegister, &prediction_receiver_upload};
    auto pool = std::make_unique<detail::PredictionPreviewPool>(execution, context, operations, authority);
    auto decoded = std::make_shared<std::array<std::uint8_t, 48U>>();
    decoded->fill(127U);
    std::weak_ptr<std::array<std::uint8_t, 48U>> retained = decoded;
    auto raw = pool->Capture(nullptr, {4U, 4U}, 0U, {}, {}, std::make_shared<const std::vector<std::string>>(), 1, decoded->data(), decoded);
    REQUIRE(raw);
    auto draw = std::async(std::launch::async, [raw, context] {
        try {
            gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated,
                .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
            auto candidate = runtime.AcquireOutput();
            raw->Draw(runtime, candidate);
        } catch (...) { return std::current_exception(); }
        return std::exception_ptr{};
    });
    const mmltk::testsupport::ScopedTestCleanup release{[&] { fault.upload.Release(); }};
    REQUIRE(fault.upload.WaitEntered(std::chrono::seconds{2}));
    CHECK_FALSE(pool->HasUnsafeSourceCustody());
    raw.reset();
    decoded.reset();
    pool.reset(); // the receiver transaction, not this replaceable shell, owns the fact
    CHECK(authority->admission_open());
    CHECK(authority->fact().reservations == 1U);
    fault.upload.Release();
    const auto failure = mmltk::testsupport::await_test_future(draw, "receiver completion after pool destruction");
    CHECK(gpu::is_image_execution_failure(failure));
    CHECK_FALSE(authority->admission_open());
    CHECK(authority->fact().occupancy == 1U);
    CHECK(authority->fact().reservations == 0U);
    CHECK_FALSE(retained.expired());
    for (unsigned attempt = 0U; attempt < 4U; ++attempt)
        CHECK_THROWS(detail::PredictionPreviewPool(execution, context, operations, authority));
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
    prediction_receiver_fault = fault.get();
    const mmltk::testsupport::ScopedTestCleanup clear{[&] { fault->upload.Release(); prediction_receiver_fault = nullptr; }};
    std::atomic_size_t constructions = 0U;
    std::promise<void> first_frame, first_done, second_done, visual_failure, recovered;
    {
        PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
            [&] { ++constructions; return std::make_unique<FakeComputeRuntime>(gate, false, nullptr, false, nullptr, 0U, nullptr, fault); },
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
        const mmltk::testsupport::ScopedTestCleanup stop{[&] { fault->upload.Release(); prediction.Shutdown(); }};
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
            for (unsigned attempt = 0U; attempt < 4U; ++attempt)
                CHECK_THROWS_AS(prediction.Start({}), contracts::UnavailableError);
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
        CHECK(facts.occupancy + facts.reservations <= detail::PredictionPreviewPool::kSlotCapacity + 2U);
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
    PredictSystem prediction{settings, dataset, model, {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
        [gate] { return std::make_unique<FakeComputeRuntime>(gate, false, nullptr, true); },
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
    ExportSystem export_system{settings, dataset, model, [export_gate] { return std::make_unique<FakeComputeRuntime>(export_gate, false); },
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
    PredictSystem predict{settings,
                          dataset,
                          model,
                          {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
                          [&] {
                              const bool fail = constructions++ == 0U;
                              return std::make_unique<FakeComputeRuntime>(predict_gate, fail, predictions);
                          },
                          [&](PredictSystem::event_type event) {
                              if (const auto* changed = std::get_if<PredictChanged>(&event); changed && changed->snapshot.frame.valid() && !frame_seen.exchange(true))
                                  prediction_frame.set_value();
                              const auto terminal = std::visit([](const auto& value) { return value.snapshot.operation.terminal.outcome; }, event);
                              if (std::holds_alternative<PredictFailed>(event)) predict_failed.set_value(std::move(event));
                              else if (terminal == contracts::ComputeOperationOutcome::Succeeded && predict_terminals.fetch_add(1U) == 0U)
                                  predict_succeeded.set_value(std::move(event));
                              else if (terminal == contracts::ComputeOperationOutcome::Cancelled) predict_cancelled.set_value(std::move(event));
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
                {.product_owner = expected.owner, .product_revision = expected.revision, .destination = workspace, .ready = [ready] {
                     ready->set_value();
                 }});
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
    TrainingTerminals terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions),
                            [&](TrainingSystem::event_type event) {
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
    TrainingTerminals terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions),
                            [&](TrainingSystem::event_type event) {
                                if (const auto* progress = std::get_if<TrainingProgress>(&event)) {
                                    if (progress->local.progress.sequence == 1U) {
                                        CHECK(progress->local.generation_frontier != 0U);
                                        if (sequence_one_progress.fetch_add(1U) == 0U) first_successful_progress.set_value();
                                    }
                                    return;
                                }
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
                                    case 0U:
                                        query_done.set_value();
                                        break;
                                    case 1U:
                                        mutation_done.set_value();
                                        break;
                                    default:
                                        reconciliation_done.set_value();
                                        break;
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
    TrainingSystem query_training{settings, dataset, model,
                                  [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
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
                             if (!std::holds_alternative<TrainingChanged>(event)) return;
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

TEST_CASE("training admission and matching cancellation do not invert system and worker locks",
          "[controller][systems][training][admission]") {
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
    mmltk::testsupport::TestGate admission{provider_query ? "concurrent provider Query and Clear admission"
                                                          : "concurrent training Start and Stop admission"};
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
        CHECK(test_support::with_denied_syscall(call, [] {
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
                          .execution =
                              mmltk::frameworks::gpu::DeviceExecution{.device = 999999, .placement = {.numa_node = node, .cpus = {cpu}}}};
                      for (const auto& construct : std::array<std::function<void()>, 3>{
                               [&] { CudaValidationRuntime runtime(configuration); }, [&] { CudaExportRuntime runtime(configuration); },
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
    float* source = nullptr;
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&source), 12U * sizeof(float)) == cudaSuccess);
    const std::shared_ptr<void> custody(source, [](void* address) { static_cast<void>(cudaFree(address)); });
    const std::array<float, 12U> pixels{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    REQUIRE(cudaMemcpy(source, pixels.data(), sizeof(pixels), cudaMemcpyHostToDevice) == cudaSuccess);
    std::optional<detail::PredictionPreviewPool> pool;
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    pool.emplace(execution, context);
    const auto classes = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"first", "last"});
    std::array<std::shared_ptr<const detail::PredictionPreviewFrame>, 3U> readers;
    for (auto& reader : readers) { reader = pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody); REQUIRE(reader); }
    CHECK_FALSE(pool->Capture(source, {2U, 2U}, 0U, {}, {}, classes, 2, nullptr, custody));
    gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated,
        .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
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
    REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(rgba == std::array<std::uint8_t, 16U>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
    // A replacement renderer rejects a pending old-context frame before touching its candidate.
    gpu::DeviceContext replacement_context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated,
        execution.placement.numa_node, execution);
    gpu::SystemImageRuntime replacement({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated,
        .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = replacement_context});
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

namespace {
struct PredictionTransferFault {
    int copies = 0;
    int fail_copy = 0;
    bool fail_record = false;
    bool fail_settle = false;
    int settlements = 0;
};
thread_local PredictionTransferFault prediction_transfer_fault;
CUresult prediction_copy_test(CUdeviceptr destination, CUcontext destination_context, CUdeviceptr source, CUcontext source_context,
                              std::size_t bytes, CUstream stream) {
    if (++prediction_transfer_fault.copies == prediction_transfer_fault.fail_copy) return CUDA_ERROR_INVALID_VALUE;
    return cuMemcpyPeerAsync(destination, destination_context, source, source_context, bytes, stream);
}
cudaError_t prediction_record_test(cudaEvent_t event, cudaStream_t stream) {
    if (prediction_transfer_fault.fail_record) return cudaErrorInvalidResourceHandle;
    return cudaEventRecord(event, stream);
}
cudaError_t prediction_settle_test(cudaStream_t stream) {
    ++prediction_transfer_fault.settlements;
    if (prediction_transfer_fault.fail_settle) return cudaErrorUnknown;
    return cudaStreamSynchronize(stream);
}
}

TEST_CASE("prediction transfer faults settle or retain exact source custody", "[controller][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace controller = mmltk::controller;
    namespace runtime = mmltk::backend::ml::runtime;
    namespace rfdetr = mmltk::backend::models::rfdetr;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    float* source = nullptr;
    REQUIRE(cudaMalloc(reinterpret_cast<void**>(&source), 17U * sizeof(float)) == cudaSuccess);
    REQUIRE(cudaMemset(source, 0, 17U * sizeof(float)) == cudaSuccess);
    std::shared_ptr<void> custody(source, [](void* address) { static_cast<void>(cudaFree(address)); });
    const std::weak_ptr<void> lifetime = custody;
    const runtime::AnalysisAnnotationStorage annotations{
        .source_region = {.width = 2, .height = 2}, .value_capacity = 1, .value_count = 1,
        .boxes_xyxy = {.address = reinterpret_cast<std::uintptr_t>(source + 12), .capacity_bytes = 16},
        .category_ids = {.address = reinterpret_cast<std::uintptr_t>(source + 16), .capacity_bytes = 4}};
    const std::array<rfdetr::Prediction, 1> predictions{{{.category_id = 1, .score = .7F}}};
    const auto classes = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"object"});
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    const controller::detail::PredictionPreviewPool::TransferOperations operations{
        &prediction_copy_test, &prediction_record_test, &prediction_settle_test, &cuMemHostRegister};
    auto pool = std::make_unique<controller::detail::PredictionPreviewPool>(execution, context, operations);
    for (int stage : {1, 2, 3, 4}) {
        prediction_transfer_fault = {.fail_copy = stage < 4 ? stage : 0, .fail_record = stage == 4};
        CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody), std::runtime_error);
        CHECK(prediction_transfer_fault.settlements == 1);
        CHECK(custody.use_count() == 1);
        prediction_transfer_fault = {};
        auto recovered = pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody);
        REQUIRE(recovered);
        CHECK(prediction_transfer_fault.settlements == 0);
    }
    prediction_transfer_fault = {.fail_copy = 2, .fail_settle = true};
    int stopped = 0;
    CHECK_THROWS_AS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody,
        [](void* value) { ++*static_cast<int*>(value); }, &stopped), runtime::CudaOperationError);
    CHECK(stopped == 1);
    CHECK(pool->HasUnsafeSourceCustody());
    const auto attempted = prediction_transfer_fault.copies;
    CHECK_THROWS(pool->Capture(source, {2, 2}, 0, predictions, annotations, classes, 1, nullptr, custody));
    CHECK(prediction_transfer_fault.copies == attempted);
    custody.reset();
    CHECK_FALSE(lifetime.expired());
    pool.reset();
    CHECK_FALSE(lifetime.expired());
    prediction_transfer_fault = {};
}

TEST_CASE("ordinary preview allocation refusal leaves its decoded source intact", "[controller][gpu]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace controller = mmltk::controller;
    const auto execution = gpu::resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture());
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    gpu::DeviceContext context(0, gpu::cuda_image_copy_backend(), gpu::DeviceContextMode::Isolated, execution.placement.numa_node, execution);
    const controller::detail::PredictionPreviewPool::TransferOperations operations{
        &cuMemcpyPeerAsync, &cudaEventRecord, &cudaStreamSynchronize,
        +[](void*, std::size_t, unsigned) -> CUresult { return CUDA_ERROR_OUT_OF_MEMORY; }};
    controller::detail::PredictionPreviewPool pool(execution, context, operations);
    auto source = std::make_shared<std::array<std::uint8_t, 12>>(std::array<std::uint8_t, 12>{255, 0, 0});
    const auto classes = std::make_shared<const std::vector<std::string>>();
    auto raw = pool.Capture(nullptr, {2, 2}, 0, {}, {}, classes, 1, source->data(), source);
    REQUIRE(raw);
    CHECK(source.use_count() == 2);
    gpu::SystemImageRuntime runtime({.device = 0, .context_mode = gpu::DeviceContextMode::Isolated,
        .output_layout = gpu::ImageProductLayout::CleanAndSemantic, .adopted_context = context});
    auto candidate = runtime.AcquireOutput();
    CHECK_THROWS(raw->Draw(runtime, candidate));
    CHECK((*source)[0] == 255U);
    candidate = {};
    controller::detail::PredictionPreviewPool healthy(execution, context);
    auto recovered = healthy.Capture(nullptr, {2, 2}, 0, {}, {}, classes, 1, source->data(), source);
    REQUIRE(recovered);
    auto output = runtime.AcquireOutput();
    recovered->Draw(runtime, output);
    const auto completed = runtime.CommitOutput(std::move(output));
    auto view = completed.Borrow();
    const auto plane = view.plane(0U).plane();
    std::array<std::uint8_t, 16> rgba{};
    REQUIRE(cudaMemcpy2D(rgba.data(), 8U, reinterpret_cast<const void*>(plane.data), plane.descriptor.pitch_bytes, 8U, 2U, cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(rgba[0] == 255U);
    CHECK(rgba[1] == 0U);
    CHECK(rgba[2] == 0U);
    CHECK(rgba[3] == 255U);
}
