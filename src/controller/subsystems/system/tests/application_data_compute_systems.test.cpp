#include "src/acceptance/tests/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cuda.h>

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
    CHECK_THROWS_AS(Revision::Admit(maximum), contracts::FailedError);
    CHECK_THROWS_AS(Revision::Admit(maximum - 1U), contracts::FailedError);
    CHECK_THROWS_AS(Revision::Admit(maximum - 2U), contracts::FailedError);
    CHECK_THROWS_AS(Revision::Admit(maximum - 3U), contracts::FailedError);
    const auto admitted = Revision::Admit(maximum - 5U);
    REQUIRE(admitted == maximum - 4U);
    const auto progressed = Revision::Progress(admitted, false);
    REQUIRE(progressed == maximum - 3U);
    CHECK_FALSE(Revision::Progress(*progressed, false));
    const auto cancelled = Revision::Cancel(*progressed);
    REQUIRE(cancelled == maximum - 2U);
    CHECK_FALSE(Revision::Progress(*cancelled, true));
    const auto settled = Revision::Complete(*cancelled);
    REQUIRE(settled == maximum - 1U);
    CHECK_FALSE(Revision::Complete(*settled));
    // A failure after successful settlement still receives a distinct identity.
    CHECK(Revision::Fail(*settled) == maximum);
    CHECK_FALSE(Revision::Fail(maximum));
    CHECK(Revision::Fail(*cancelled) == maximum - 1U);
    CHECK(Revision::Complete(Revision::Admit(maximum - 4U)) == maximum - 2U);
    const auto earlier_cancel = Revision::Cancel(admitted);
    REQUIRE(earlier_cancel == maximum - 3U);
    CHECK(Revision::Progress(*earlier_cancel, true) == maximum - 2U);
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
                                          std::string_view, std::uint32_t) override {
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
        REQUIRE(dataset_.Inspect({root_ / "train.bin", root_ / "val.bin", {}}, "rf-detr-base", 560U).available());
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
                                          std::string_view, std::uint32_t) override {
        Enter();
        observation_->inspect_started.set_value();
        static_cast<void>(observation_->inspect_gate->Wait({}));
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

class FakeComputeRuntime final : public ValidationRuntime, public ExportRuntime, public PredictRuntime {
   public:
    FakeComputeRuntime(std::shared_ptr<StopGate> gate, const bool fail, std::shared_ptr<std::atomic_size_t> predictions = {})
        : gate_(std::move(gate)), fail_(fail), predictions_(std::move(predictions)) {}

    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ValidateRequest, const std::stop_token stop,
                                   const ComputeProgressSink& progress) override {
        return RunImpl(stop, progress);
    }
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, const std::stop_token stop,
                                   const ComputeProgressSink& progress) override {
        return RunImpl(stop, progress);
    }
    PredictRuntime::Product Run(mmltk::backend::models::rfdetr::PredictRequest, const std::stop_token stop,
                                const ComputeProgressSink& progress) override {
        if (predictions_) ++*predictions_;
        auto terminal = RunImpl(stop, progress);
        if (terminal.outcome != contracts::ComputeOperationOutcome::Succeeded)
            return {.terminal = std::move(terminal), .extent = {}, .rgba = {}, .boxes = {}};
        return {.terminal = std::move(terminal), .extent = {4U, 4U}, .rgba = std::vector<std::uint8_t>(4U * 4U * 4U, 0xffU), .boxes = {}};
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
                              if (std::holds_alternative<PredictProgress>(event)) return;
                              if (predict_terminals++ == 0U)
                                  predict_failed.set_value(std::move(event));
                              else if (predict_terminals == 2U)
                                  predict_succeeded.set_value(std::move(event));
                              else
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
            predict.RequestWorkspace({.product_owner = expected.owner, .product_revision = expected.revision,
                                      .destination = workspace, .ready = [ready] { ready->set_value(); }});
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
        predict.RequestWorkspace({.detach_only = true, .destination = workspace,
                                  .ready = [detached] { detached->set_value(); }});
        mmltk::testsupport::await_test_future(released, "Predict workspace detach");
        workspace.reset();
    }
    predict_gate->Reset();
    const auto admitted = predict.Start({});
    CHECK(admitted.revision > completed_observation);
    CHECK_FALSE(admitted.frame.valid());
    CHECK_FALSE(predict.BorrowFrame().valid());
    const auto cancelled_request = predict.Stop({});
    CHECK(cancelled_request.revision > admitted.revision);
    CHECK(std::holds_alternative<PredictChanged>(predict_cancelled.get_future().get()));
    CHECK(predict.snapshot().operation.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    CHECK(predict.snapshot().revision >= cancelled_request.revision);
    CHECK_FALSE(predict.snapshot().frame.valid());
    CHECK_FALSE(predict.BorrowFrame().valid());
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
