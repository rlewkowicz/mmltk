#include "src/controller/subsystems/train/training_system.h"
#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/controller/subsystems/system/tests/prediction_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/common/io/file_digest.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_event_publisher.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/file_dialog_client.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/validate/validation_system.h"
#include "src/controller/subsystems/validate/validation_runtime.h"
#include "src/controller/subsystems/export/export_system.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/system/compute_runtime.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/common/system/numa_topology.h"
#include "src/backend/data/catalog/class_catalog.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {

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
} // namespace
} // namespace mmltk::controller
