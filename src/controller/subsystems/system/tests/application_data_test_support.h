#pragma once
#include "prediction_test_support.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/services/settings_store.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/compute_intent_materializer.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mmltk::controller::test_support {
[[nodiscard]] inline services::SettingsLocation install_settings(const std::filesystem::path& root) {
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
    REQUIRE(services::SettingsStore::save(location.value(), settings, 1U).succeeded());
    return location;
}
[[nodiscard]] inline contracts::ArtifactSplitFact split(const std::filesystem::path& path) {
    return {.path = path.string(),
            .image_count = 1U,
            .width = 16U,
            .height = 16U,
            .channels = 3U,
            .max_instances_per_image = 1U,
            .class_names = {{.value = "object"}}};
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
                       : contracts::ArtifactProgress{.phase = contracts::ArtifactCompilePhase::Pixels, .activity = "compiling", .completed = 1U, .total = 2U});
        if (!gate_->Wait(stop)) return {.output = request.output, .cancelled = true};
        if (fail_)
            return {.output = request.output,
                    .inspection = {.compatible = true,
                                   .splits = {split(request.output / "train.bin"), split(request.output / "val.bin"), split(request.output / "test.bin"),
                                              split(request.output / "overflow.bin")},
                                   .detail = {}}};
        return {.output = request.output, .inspection = {.compatible = true, .splits = {split(request.output / "train.bin")}, .detail = {}}};
    }
    contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, std::string_view, std::uint32_t,
                                          std::stop_token) override {
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
    ModelArtifactAdmission Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path& custom, int, const std::stop_token stop,
                                   const std::function<void(const contracts::ModelProgress&)>& progress) override {
        progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "verifying fixture model"});
        if (!gate_->Wait(stop)) throw std::runtime_error("fixture model selection cancelled");
        return {.artifact = custom.string()};
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
              loaded_.settings,
              [] {
                  auto gate = std::make_shared<StopGate>();
                  gate->Release();
                  return std::make_unique<BlockingModelRuntime>(std::move(gate));
              },
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
        const auto result = services::ArtifactCompileResult{.output = request.output,
                                                            .inspection = {.compatible = true, .splits = {split(request.output / "train.bin")}, .detail = {}}};
        Leave();
        return result;
    }
    contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>& paths, std::string_view, std::uint32_t,
                                          const std::stop_token stop) override {
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
}  // namespace mmltk::controller::test_support
