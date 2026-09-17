#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/settings_system.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {

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
};
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
TEST_CASE("dataset publishes direct progress, returns Busy, stops locally, and reconstructs after failure", "[controller][systems][dataset]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-dataset");
    SettingsSystem settings;
    REQUIRE(settings.Load(install_settings(root)).applied());
    auto gate = std::make_shared<mmltk::testsupport::StopGate>();
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
    gate = std::make_shared<mmltk::testsupport::StopGate>();
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
} // namespace
} // namespace mmltk::controller
