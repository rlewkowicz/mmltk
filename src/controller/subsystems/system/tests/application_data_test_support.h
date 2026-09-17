#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <variant>
#include "src/test_support/async_test_utils.hpp"
#include "src/controller/services/tests/support/settings_test_fixture.h"
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
    FakeDatasetRuntime(std::shared_ptr<mmltk::testsupport::StopGate> gate, const bool fail) : gate_(std::move(gate)), fail_(fail) {}
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
    std::shared_ptr<mmltk::testsupport::StopGate> gate_;
    bool fail_ = false;
};
class BlockingModelRuntime final : public ModelRuntime {
   public:
    explicit BlockingModelRuntime(std::shared_ptr<mmltk::testsupport::StopGate> gate) : gate_(std::move(gate)) {}
    ModelArtifactAdmission Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path& custom, int, const std::stop_token stop,
                                   const std::function<void(const contracts::ModelProgress&)>& progress) override {
        progress({.stage = contracts::ModelProgressStage::Verifying, .activity = "verifying fixture model"});
        if (!gate_->Wait(stop)) throw std::runtime_error("fixture model selection cancelled");
        return {.artifact = custom.string()};
    }

   private:
    std::shared_ptr<mmltk::testsupport::StopGate> gate_;
};
class ApplicationDataFixture final {
   public:
    explicit ApplicationDataFixture(std::filesystem::path root)
        : root_(std::move(root)),
          loaded_(root_),
          dataset_gate_(std::make_shared<mmltk::testsupport::StopGate>()),
          dataset_(loaded_.settings, [gate = dataset_gate_] { return std::make_unique<FakeDatasetRuntime>(gate, false); }),
          model_(
              loaded_.settings,
              [] {
                  auto gate = std::make_shared<mmltk::testsupport::StopGate>();
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
    std::shared_ptr<mmltk::testsupport::StopGate> dataset_gate_;
    DatasetSystem dataset_;
    std::mutex model_mutex_;
    std::condition_variable model_changed_;
    std::vector<contracts::ModelUiState> model_terminals_;
    ModelSystem model_;
};
struct DatasetRuntimeObservation final {
    std::shared_ptr<mmltk::testsupport::StopGate> inspect_gate = std::make_shared<mmltk::testsupport::StopGate>();
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
}  // namespace mmltk::controller::test_support
