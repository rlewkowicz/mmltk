#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/settings_system.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
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
   public:
    mutable std::filesystem::path physical_output;
    mutable std::filesystem::path publication_output;
    mutable bool observer_enabled = false;
    mutable bool observer_context = false;
    std::function<void(mmltk::common::concurrency::CancellationObservation)> work;

   private:
    void compile_directory(const std::filesystem::path&, const std::filesystem::path&, std::uint32_t, bool, mmltk::backend::imaging::resample::ImageResizeMode,
                           mmltk::common::concurrency::CancellationObservation, services::ArtifactProgressObserver) const override {}
    void compile_benchmark(const std::filesystem::path& output, const std::filesystem::path& publication, std::uint32_t, bool,
                           mmltk::backend::imaging::resample::ImageResizeMode, mmltk::common::concurrency::CancellationObservation cancellation,
                           services::ArtifactProgressObserver,
                           services::ArtifactBenchmarkTraceObserver trace) const override {
        physical_output = output;
        publication_output = publication;
        observer_enabled = trace.report != nullptr;
        observer_context = trace.context != nullptr;
        trace("benchmark.direct", R"({"records":1})");
        if (work) work(cancellation);
    }
};
TEST_CASE("artifact dataset runtime owns its diagnostic target and borrows only during compile", "[controller][systems][dataset][diagnostics]") {
    const mmltk::testsupport::ScopedTempDir root{"ordinary-dataset-diagnostics"};
    UnusedWeightOperations weights;
    DiagnosticCompiler compiler;
    const auto log = root.path() / "trace.jsonl";
    services::DiagnosticsClient client{mmltk::common::io::ScopedFd{::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)},
                                       services::DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(client.enabled());
    services::RuntimeDiagnosticTarget target;
    {
        services::RuntimeDiagnostics creator{client.producer()};
        target = creator.target();
    }
    bool enabled = true;
    SECTION("target copy survives creator and caller copy destruction") {}
    SECTION("absent target installs no observer") {
        target = {};
        enabled = false;
    }
    SECTION("closed producer installs no observer") {
        client.close();
        enabled = false;
    }
    SECTION("destroyed producer installs no observer") {
        services::DiagnosticsClient temporary{mmltk::common::io::ScopedFd{::open((root.path() / "closed.jsonl").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)},
                                              services::DiagnosticsExecutionPolicy::CallerDriven};
        services::RuntimeDiagnostics creator{temporary.producer()};
        target = creator.target();
        enabled = false;
    }
    const services::ArtifactCompileRequest request{.kind = services::ArtifactCompileKind::Benchmark,
                                                   .source = {},
                                                   .output = root.path() / "output",
                                                   .preset = "rf-detr-base",
                                                   .resolution = 560U,
                                                   .overwrite = true};
    {
        ArtifactDatasetRuntime runtime{services::ArtifactStore{root.path() / "cache", weights, compiler}, target};
        target = {};
        const auto result = runtime.Compile(request, {}, {});
        CHECK_FALSE(result.cancelled);
        CHECK_FALSE(result.inspection.available());
        CHECK(compiler.observer_enabled == enabled);
        CHECK(compiler.observer_context == enabled);
        CHECK(compiler.publication_output == request.output);
        CHECK(compiler.physical_output != request.output);
        CHECK(compiler.physical_output.parent_path() == request.output.parent_path());
        CHECK_FALSE(std::filesystem::exists(compiler.physical_output));
    }
    CHECK(client.counters().accepted == (enabled ? 1U : 0U));
    client.close(services::DiagnosticsCloseMode::Flush);
    std::ifstream input{log};
    const std::string records{std::istreambuf_iterator<char>{input}, {}};
    CHECK(records.contains("benchmark.direct") == enabled);
}
TEST_CASE("owned dataset diagnostics survive runtime failure reconstruction and stop join", "[controller][systems][dataset][diagnostics]") {
    const mmltk::testsupport::ScopedTempDir root{"dataset-diagnostic-reconstruction"};
    SettingsSystem settings;
    auto location = install_settings(root.path());
    auto stored = contracts::default_gui_settings_state();
    stored.workflows.train.compile_benchmark_dataset_override = true;
    stored.workflows.train.compiled_dataset_dir = root.path() / "compiled";
    REQUIRE(services::SettingsStore::save(location.value(), stored, 1U).succeeded());
    REQUIRE(settings.Load(location).applied());
    services::DiagnosticsClient client{mmltk::common::io::ScopedFd{::open((root.path() / "trace.jsonl").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600)},
                                       services::DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(client.enabled());
    services::RuntimeDiagnosticTarget target;
    {
        services::RuntimeDiagnostics creator{client.producer()};
        target = creator.target();
    }
    UnusedWeightOperations weights;
    DiagnosticCompiler compiler;
    std::size_t constructions = 0U;
    TerminalSequence<DatasetSystem::event_type> terminals;
    mmltk::testsupport::TestGate gate{"dataset compiler"};
    compiler.work = [](auto) { throw std::runtime_error("diagnostic compiler failure"); };
    DatasetSystem dataset{settings, [&, target] {
                              ++constructions;
                              return std::make_unique<ArtifactDatasetRuntime>(services::ArtifactStore{root.path() / "cache", weights, compiler}, target);
                          }, [&](DatasetSystem::event_type event) {
                              if (std::holds_alternative<DatasetChanged>(event)) terminals.Publish(std::move(event));
                          }};
    mmltk::testsupport::ScopedTestCleanup cleanup{[&] {
        gate.Release();
        dataset.Shutdown();
    }};
    target = {};
    static_cast<void>(dataset.Compile({}));
    auto first = terminals.First();
    const auto failed = mmltk::testsupport::await_test_future(first, "failed compile");
    CHECK(std::get<DatasetChanged>(failed).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
    CHECK(compiler.observer_enabled);
    compiler.work = [receipt = gate.receipt()](auto) { receipt.ArriveAndWait(); };
    static_cast<void>(dataset.Compile({}));
    REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
    static_cast<void>(dataset.Stop());
    gate.Release();
    auto second = terminals.Second();
    const auto cancelled = mmltk::testsupport::await_test_future(second, "cancelled compile");
    CHECK(std::get<DatasetChanged>(cancelled).snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Cancelled);
    dataset.Shutdown();
    CHECK(constructions == 2U);
    CHECK(compiler.observer_enabled);
    CHECK_FALSE(std::filesystem::exists(compiler.physical_output));
    CHECK(client.counters().accepted == 2U);
    client.close(services::DiagnosticsCloseMode::Flush);
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
    using ResizeMode = mmltk::backend::imaging::resample::ImageResizeMode;
    CHECK(settings.workflows.train.compile_resize_mode == ResizeMode::Stretch);
    settings.workflows.train.compile_resize_mode = ResizeMode::Letterbox;
    settings.workflows.train.compile_perceptual_downscale = true;
    settings.workflows.train.request.gpu_augmentation.enabled = false;
    settings.workflows.train.request.gpu_augmentation.perceptual_downscale = false;
    const auto captured = services::materialize_artifact_compile(settings);
    REQUIRE(captured);
    CHECK(captured->perceptual_downscale);
    CHECK(captured->resize_mode == ResizeMode::Letterbox);
    settings.workflows.train.compile_perceptual_downscale = false;
    settings.workflows.train.compile_resize_mode = ResizeMode::Stretch;
    settings.workflows.train.request.gpu_augmentation.perceptual_downscale = true;
    const auto next = services::materialize_artifact_compile(settings);
    REQUIRE(next);
    CHECK_FALSE(next->perceptual_downscale);
    CHECK(next->resize_mode == ResizeMode::Stretch);
    CHECK(captured->perceptual_downscale);
    CHECK(captured->resize_mode == ResizeMode::Letterbox);
}
}  // namespace
}  // namespace mmltk::controller
