#include <algorithm>
#include "src/controller/presentation/tests/support/visual_runtime_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/annotation/tests/support/annotation_system_fixture.h"
#include "src/controller/subsystems/explore/tests/support/explore_system_fixture.h"
#include "src/controller/subsystems/upscale/tests/support/upscale_system_fixture.h"
#include "src/controller/subsystems/live/tests/support/live_system_fixture.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/subsystems/annotation/tests/support/annotation_test_utils.hpp"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/frameworks/gpu/image_workspace.h"
#include "src/controller/contracts/diagnostic_context.h"
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cuda.h>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <variant>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
void present_and_wait(PresentationSystem& presentation, TestPresentationWriterState& writer, EventGate& events, const VisualSourceReader& source,
                      const std::uint64_t revision) {
    static_cast<void>(presentation.Select(source.source));
    writer.SignalReadiness();
    REQUIRE(events.Wait([&] { return presentation.snapshot().completed.revision == revision; }));
}
TEST_CASE("Explore visible loading and independent producers progress through shared mouse input", "[explore][priority]") {
    StreamingExploreFixture scenario{1U};
    StreamingExploreFixture independent{1U};
    const ExploreViewport viewport{.extent = {16U, 8U}, .row_count = 2U, .columns = 4U};
    scenario.OpenAndWait(viewport);
    independent.OpenAndWait(viewport);
    auto& explore = scenario.system();
    auto& probe = scenario.probe();
    explore.SetInputPeer(1U);
    const auto placeholders = explore.snapshot();
    CHECK(placeholders.order.visible_indices == std::vector<std::uint32_t>{0U, 1U, 2U, 3U, 4U, 5U});
    CHECK(std::ranges::none_of(placeholders.gallery.slots, [](bool ready) { return ready; }));
    scenario.probe().AllowAllocation();
    REQUIRE(probe.Wait([&] { return probe.assignments.size() == 1U; }));
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{12.5F, 0.5F}});
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{15.875F, 3.875F}});
    probe.Release(0U);
    REQUIRE(probe.Wait([&] { return probe.assignments.size() == 1U && probe.assignments.front().compiled_index == 1U; }));
    REQUIRE(scenario.Wait([&]() -> bool { return explore.snapshot().gallery.slots[0U]; }));
    CHECK(explore.snapshot().order.visible_indices == placeholders.order.visible_indices);
    CHECK_FALSE(explore.snapshot().gallery.slots[3U]);
    independent.probe().AllowAllocation();
    REQUIRE(independent.probe().Wait([&] { return independent.probe().assignments.size() == 1U; }));
    independent.probe().Release(0U);
    REQUIRE(independent.Wait([&]() -> bool { return independent.system().snapshot().gallery.slots[0U]; }));
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{8.0F, 4.0F}});
    probe.Release(1U);
    REQUIRE(probe.Wait([&] { return probe.assignments.size() == 1U && probe.assignments.front().compiled_index == 2U; }));
    REQUIRE(scenario.Wait([&]() -> bool { return explore.snapshot().gallery.slots[1U]; }));
    CHECK(explore.BorrowFrame().valid());
}
TEST_CASE("Explore retains content and accepts input through pending image completion and source changes", "[explore][priority]") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto work = std::make_shared<ExploreWorkProbe>();
    auto gate = std::make_shared<ExplorePostRenderGate>(1U);
    LoadedSettings settings;
    ExploreScenario scenario{
        settings, backend,
        [work, gate] { return std::make_unique<TestExploreAlgorithm>(std::make_shared<std::atomic<std::size_t>>(0U), nullptr, nullptr, nullptr, work, gate); }};
    auto& explore = scenario.system();
    mmltk::testsupport::ScopedTestCleanup release_gate{[&] { mmltk::testsupport::release_test_promise(gate->release); }};
    scenario.OpenAndWait({.extent = {96U, 48U}, .row_count = 1U, .columns = 2U});
    explore.SetInputPeer(1U);
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{1.5F, 1.5F}});
    const auto prior = explore.snapshot();
    auto retained = explore.BorrowFrame();
    auto overlay = prior.overlay;
    overlay.show_boxes = !overlay.show_boxes;
    static_cast<void>(explore.UpdateOverlay(overlay));
    mmltk::testsupport::await_test_promise(gate->entered, "pending Explore overlay");
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{60.5F, 1.5F}});
    mmltk::testsupport::release_test_promise(gate->release);
    REQUIRE(scenario.Wait([&] { return explore.snapshot().overlay == overlay && explore.snapshot().frame != prior.frame; }));
    CHECK(retained.valid());
    CHECK(explore.snapshot().order.visible_indices == prior.order.visible_indices);
    auto filter = explore.snapshot().filter;
    filter.minimum_instances = 1U;
    filter.order = ExploreOrder::Shuffled;
    static_cast<void>(explore.UpdateFilter({.filter = filter, .overlay = overlay}));
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().filter == filter; }));
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{1.5F, 1.5F}});
    const auto seed = explore.snapshot().order.shuffle_seed;
    static_cast<void>(explore.Reroll());
    REQUIRE(scenario.Wait([&] { return !explore.snapshot().busy && explore.snapshot().order.shuffle_seed != seed; }));
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{1.5F, 1.5F}});
    scenario.OpenAndWait(explore.snapshot().viewport, "/replacement");
    explore.Input({.source = PresentationSourceKind::Explore, .peer_epoch = 1U, .point = WorkspacePoint{1.5F, 1.5F}});
    static_cast<void>(explore.Stop());
}
TEST_CASE("Explore viewport and Annotation pointer work preserve their intended ordering") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto observed_nproc = std::make_shared<std::atomic<std::size_t>>(0U);
    DiagnosticCapture diagnostics;
    EventGate explore_events;
    LoadedSettings settings;
    ExploreSystem explore{settings.system(),
                          kDevice,
                          4U,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [observed_nproc] { return std::make_unique<TestExploreAlgorithm>(observed_nproc); }, 3U),
                          [&explore_events](ExploreSystem::event_type) { explore_events.Advance(); },
                          diagnostics.sink()};
    CHECK_FALSE(explore.BorrowFrame().valid());
    CHECK_THROWS_AS(explore.UpdateViewport({.viewport = {.extent = {48U, 48U}}}), contracts::UnavailableError);
    const auto explore_admitted = explore.Open({.viewport = {.extent = {64U, 64U}}, .compiled_source = "/test"});
    CHECK(explore_admitted.busy);
    CHECK(explore_admitted.revision != 0U);
    REQUIRE(explore_events.Wait([&] { return explore.snapshot().ready; }));
    CHECK_FALSE(explore.snapshot().busy);
    CHECK(explore.snapshot().revision > explore_admitted.revision);
    explore.UpdateViewport({.viewport = {.extent = {48U, 48U}}});
    explore.UpdateViewport({.viewport = {.extent = {32U, 32U}}});
    REQUIRE(explore_events.Wait([&] {
        const auto state = explore.snapshot();
        return state.viewport.extent.width == 32U && state.frame.extent.width == 32U;
    }));
    CHECK(explore.snapshot().nproc == 4U);
    REQUIRE(explore.BorrowFrame().valid());
    CHECK(explore.BorrowFrame().plane(0U).revision() == explore.snapshot().frame.revision);
    CHECK(observed_nproc->load(std::memory_order_acquire) == 4U);
    CHECK(diagnostics.count.load(std::memory_order_acquire) != 0U);
    CHECK(diagnostics.last_system.load(std::memory_order_acquire) == contracts::DiagnosticOwner::Explore);
    EventGate annotation_events;
    std::atomic_uint64_t failures{0U};
    auto probe = std::make_shared<AnnotationRenderProbe>();
    AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, probe), borrow_exactly_from(explore),
                                [&](AnnotationSystem::event_type event) {
                                    if (std::holds_alternative<AnnotationFailed>(event)) failures.fetch_add(1U);
                                    annotation_events.Advance();
                                },
                                mmltk::testsupport::annotation_render_evidence()};
    CHECK_FALSE(annotation.BorrowFrame().valid());
    const auto admitted = annotation.Open({.source = explore.snapshot().frame});
    CHECK(admitted.busy);
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().ready && annotation.snapshot().frame.valid(); }));
    annotation.SetInputPeer(7U);
    auto retained = hold_annotation_frame(annotation, annotation_events, contracts::AnnotationTool::Box);
    const auto initial = annotation.snapshot();
    const auto mouse = [&](WorkspaceMouseKind kind, float x) { return mmltk::testsupport::annotation_mouse(annotation, 7U, kind, {x, 3.0F + x}); };
    auto invalid = mouse(WorkspaceMouseKind::Motion, std::numeric_limits<float>::infinity());
    CHECK_THROWS_AS(annotation.Input(invalid), contracts::InvalidIntentError);
    annotation.Input(mouse(WorkspaceMouseKind::Press, 2.0F));
    annotation.Input(mouse(WorkspaceMouseKind::Motion, 7.0F));
    annotation.Input(mouse(WorkspaceMouseKind::Release, 9.0F));
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().ui.document_revision > initial.ui.document_revision; }));
    CHECK(annotation.snapshot().ui.scene.objects.back().box == contracts::AnnotationBox{{2, 5}, {9, 12}});
    // Save and undo settle without waiting for the held output, and preserve the
    // ordered box commit in the real reducer/history rather than a mock journal.
    mmltk::testsupport::ScopedTempDir saved_document{"mmltk-annotation-independent-save"};
    const auto destination = saved_document.path() / "annotation.cbor";
    const auto saved = annotation.Save({.destination = destination.string()});
    mmltk::testsupport::await_annotation_command(annotation, annotation_events, saved.revision);
    CHECK(annotation.snapshot().ui.save_status == contracts::AnnotationSaveStatus::Saved);
    std::filesystem::remove(destination);
    const auto objects = annotation.snapshot().ui.scene.objects.size();
    const auto undo = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
    mmltk::testsupport::await_annotation_command(annotation, annotation_events, undo.revision);
    CHECK(annotation.snapshot().ui.scene.objects.size() + 1U == objects);
    CHECK(annotation.snapshot().frame == initial.frame);
    retained = {};
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().frame.revision > initial.frame.revision; }));
    CHECK(annotation.snapshot().frame.clean_revision == initial.frame.clean_revision);
    mmltk::testsupport::await_annotation_render(annotation, annotation_events);
    const auto refused = annotation.Save({.destination = "/missing-annotation-directory/file.cbor"});
    mmltk::testsupport::await_annotation_command(annotation, annotation_events, refused.revision);
    CHECK(failures.load() == 1U);
    CHECK(annotation.snapshot().ui.save_status == contracts::AnnotationSaveStatus::Failed);
    explore.Shutdown();
    CHECK(annotation.BorrowFrame().valid());
    const auto retired_mouse = mouse(WorkspaceMouseKind::Press, 2.0F);
    annotation.Shutdown();
    CHECK_THROWS_AS(annotation.Input(retired_mouse), contracts::UnavailableError);
}
TEST_CASE("Upscale and Presentation preserve complete non-square four-times high-water storage") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {80U, 40U}};
    auto& source = opened_explore.system();
    auto extents = std::make_shared<UpscaleExtentProbe>();
    EventGate upscale_events;
    UpscaleSystem upscale{kDevice,
                          RuntimeFactory(
                              0, backend, mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                              [extents] { return std::make_unique<ExtentUpscaleAlgorithm>(extents); }, 4U),
                          borrow_exactly_from(source), [&upscale_events](UpscaleSystem::event_type) { upscale_events.Advance(); }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.snapshot().frame})));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready; }));
    REQUIRE((upscale.snapshot().frame.extent == VisualExtent{320U, 160U}));
    REQUIRE(upscale.BorrowFrame().valid());
    const auto large_output = upscale.BorrowFrame().plane(0U).plane();
    CHECK(large_output.descriptor.width == 320U);
    CHECK(large_output.descriptor.height == 160U);
    REQUIRE(extents->sources.size() == 1U);
    REQUIRE(extents->targets.size() == 1U);
    CHECK(extents->sources[0U].descriptor.width == 80U);
    CHECK(extents->sources[0U].descriptor.height == 40U);
    CHECK(extents->targets[0U].descriptor.width == 320U);
    CHECK(extents->targets[0U].descriptor.height == 160U);
    const auto input_address = extents->sources[0U].data;
    const auto output_address = extents->targets[0U].data;
    const std::array presentation_sources{read_from(upscale), read_from(source)};
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    EventGate presentation_events;
    PresentationSystem presentation{kDevice, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
                                    presentation_sources, [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    present_and_wait(presentation, *writer_state, presentation_events, presentation_sources[0U], upscale.snapshot().frame.revision);
    CHECK((presentation.snapshot().completed.extent == VisualExtent{320U, 160U}));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{320U, 160U}));
    const auto allocations_at_high_water = backend->planes_allocated.load(std::memory_order_acquire);
    const auto copies_at_high_water = backend->same_copies.load(std::memory_order_acquire);
    CHECK(backend->staged_downloads.load(std::memory_order_acquire) == 0U);
    CHECK(backend->staged_uploads.load(std::memory_order_acquire) == 0U);
    opened_explore.Reopen({32U, 16U});
    static_cast<void>(upscale.Start(test_upscale_request({.source = source.snapshot().frame, .kernel = UpscaleKernel::ShiftLut})));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready && upscale.snapshot().frame.extent == VisualExtent{128U, 64U}; }));
    REQUIRE(extents->sources.size() == 2U);
    REQUIRE(extents->targets.size() == 2U);
    CHECK(extents->sources[1U].data == input_address);
    CHECK(extents->targets[1U].data != output_address);
    CHECK(backend->planes_allocated.load(std::memory_order_acquire) == allocations_at_high_water + 4U);
    present_and_wait(presentation, *writer_state, presentation_events, presentation_sources[0U], upscale.snapshot().frame.revision);
    CHECK((presentation.snapshot().completed.extent == VisualExtent{128U, 64U}));
    CHECK((presentation.snapshot().capability.extent == VisualExtent{320U, 160U}));
    CHECK(backend->planes_allocated.load(std::memory_order_acquire) == allocations_at_high_water + 4U);
    CHECK(backend->same_copies.load(std::memory_order_acquire) == copies_at_high_water + 3U);
    CHECK(backend->staged_downloads.load(std::memory_order_acquire) == 0U);
    CHECK(backend->staged_uploads.load(std::memory_order_acquire) == 0U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    upscale.Shutdown();
    CHECK(upscale.stopped());
    CHECK(backend->contexts_destroyed.load(std::memory_order_acquire) >= 2U);
}
TEST_CASE("Presentation forwards every private visual product to the simulated browser arena") {
    ProductPresentationSources products;
    const auto sources = products.sources();
    EventGate events;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    PresentationSystem presentation{kDevice,
                                    [backend = products.backend(), writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
                                    sources, [&events](PresentationSystem::event_type) { events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    std::uint64_t previous_presentation_revision = 0U;
    for (auto source = sources.rbegin(); source != sources.rend(); ++source) {
        static_cast<void>(presentation.Select(source->source));
        writer_state->SignalReadiness();
        REQUIRE(events.Wait([&] { return presentation.snapshot().completed.source == source->source; }));
        const auto published_revision = writer_state->presentation_revision.load(std::memory_order_acquire);
        CHECK(published_revision > previous_presentation_revision);
        CHECK(presentation.snapshot().presentation_revision == published_revision);
        previous_presentation_revision = published_revision;
    }
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == sources.size());
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == sources.size());
    CHECK_THROWS_AS(presentation.Select({PresentationSourceKind::Explore, 2U}), contracts::InvalidIntentError);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
}
TEST_CASE("Presentation switches sources while Live advances in background") {
    auto backend = std::make_shared<FakeImageBackend>();
    OpenedExplore opened_explore{backend, {40U, 20U}};
    auto& explore = opened_explore.system();
    EventGate live_events;
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto token_changed = std::make_shared<std::atomic_bool>(false);
    LiveSystem live{kDevice, test_live_runtime_factory(backend, captures, token_changed), [&live_events](LiveSystem::event_type) { live_events.Advance(); }};
    CHECK_FALSE(live.BorrowFrame().valid());
    const auto admitted_live = live.Start({.extent = {80U, 45U}, .frames_per_second = 120U});
    CHECK(admitted_live.revision != 0U);
    CHECK_THROWS_AS(live.Start({.extent = {80U, 45U}, .frames_per_second = 120U}), contracts::BusyError);
    REQUIRE(live_events.Wait([&] { return live.snapshot().completed_frames >= 2U; }));
    REQUIRE(live.BorrowFrame().valid());
    CHECK(live.snapshot().revision > admitted_live.revision);
    const auto before_switch = live.snapshot().completed_frames;
    const std::array sources{read_from(explore), read_from(live)};
    EventGate presentation_events;
    auto writer_state = std::make_shared<TestPresentationWriterState>();
    writer_state->allow_publication.store(false, std::memory_order_release);
    auto first_submission = writer_state->first_submission.get_future();
    PresentationSystem presentation{kDevice, [backend, writer_state] { return std::make_unique<TestPresentationWriter>(0, backend, writer_state); },
                                    std::span{sources}, [&presentation_events](PresentationSystem::event_type) { presentation_events.Advance(); }};
    PresentationScenario scenario{presentation, writer_state};
    CHECK(writer_state->constructions.load(std::memory_order_acquire) == 1U);
    const auto first_selection = presentation.Select(sources[0].source);
    CHECK(first_selection.revision != 0U);
    REQUIRE(first_submission.wait_for(2s) == std::future_status::ready);
    first_submission.get();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().capability.valid(); }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Admitted);
    const auto second_selection = presentation.Select(sources[1].source);
    CHECK(second_selection.revision > first_selection.revision);
    writer_state->allow_publication.store(true, std::memory_order_release);
    writer_state->SignalReadiness();
    REQUIRE(presentation_events.Wait([&] { return presentation.snapshot().completed.source == sources[1].source; }));
    CHECK(presentation.snapshot().capability.condition == PresentationCapabilityCondition::Ready);
    REQUIRE(live_events.Wait([&] { return live.snapshot().completed_frames > before_switch; }));
    CHECK(presentation.snapshot().capability.extent.width == 80U);
    CHECK(presentation.snapshot().capability.extent.height == 45U);
    CHECK(presentation.snapshot().capability.generation != 0U);
    CHECK(writer_state->submissions.load(std::memory_order_acquire) == 2U);
    CHECK(writer_state->timeline.load(std::memory_order_acquire) == presentation.snapshot().timeline_ready);
    CHECK(presentation.Shutdown() == PresentationShutdownResult::BrowserTerminalRequired);
    CHECK_FALSE(presentation.stopped());
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 0U);
    presentation.CloseAdmission();
    presentation.BrowserPeerLost();
    presentation.BrowserPeerLost();
    CHECK(presentation.Shutdown() == PresentationShutdownResult::Stopped);
    CHECK(writer_state->browser_terminals.load(std::memory_order_acquire) == 1U);
    CHECK(writer_state->retirements.load(std::memory_order_acquire) == 1U);
    const auto stopping_live = live.Stop();
    CHECK(stopping_live.running);
    CHECK(stopping_live.cancellation_requested);
    const auto repeated_stop = live.Stop();
    CHECK(repeated_stop.revision >= stopping_live.revision);
    CHECK(repeated_stop.completed_frames >= stopping_live.completed_frames);
    if (repeated_stop.running) {
        CHECK(repeated_stop.revision == stopping_live.revision);
        CHECK(repeated_stop.cancellation_requested);
    } else {
        CHECK(repeated_stop.revision > stopping_live.revision);
        CHECK_FALSE(repeated_stop.cancellation_requested);
    }
    REQUIRE(live_events.Wait([&] { return !live.snapshot().running; }));
    REQUIRE(live.BorrowFrame().valid());
    CHECK_FALSE(live.snapshot().cancellation_requested);
    CHECK(live.snapshot().revision > stopping_live.revision);
    CHECK_FALSE(token_changed->load(std::memory_order_acquire));
}
template <class Producer>
ProducerWorkspaceRequest request_delayed_workspace(Producer& producer, const std::shared_ptr<FakeImageBackend>& backend) {
    namespace fixture = mmltk::frameworks::gpu::test_support;
    auto layout = fixture::ImageWorkspaceTestAccess::Layout(0);
    {
        const auto raw = producer.BorrowFrame();
        REQUIRE(raw.valid());
        const auto descriptor = raw.plane(0U).plane().descriptor;
        layout.width = descriptor.width;
        layout.height = descriptor.height;
        layout.pitch_bytes = ((descriptor.row_bytes() + 63U) / 64U) * 64U;
        layout.required_allocation_bytes = layout.pitch_bytes * layout.height;
    }
    ProducerWorkspaceRequest request{.workspace = fixture::ImageWorkspaceTestAccess::Create(backend, layout)};
    request.workspace->Admit(request.workspace->identity(), layout.device_incarnation);
    backend->defer_notifications = true;
    auto submitted = backend->ObserveNextNotificationStream();
    request.Request(producer);
    request.stream = mmltk::testsupport::await_test_future(submitted, "producer workspace finalization submitted");
    CHECK_FALSE(request.workspace->Contains(request.content));
    CHECK_FALSE(producer.BorrowWorkspace().valid());
    CHECK(request.ready.wait_for(0s) == std::future_status::timeout);
    return request;
}
template <class Producer>
void complete_producer_workspace(Producer& producer, const std::shared_ptr<FakeImageBackend>& backend, ProducerWorkspaceRequest& request) {
    auto settlement = backend->HoldStreamSettlements("producer workspace physical completion", request.stream);
    backend->CompleteNotifications(request.stream);
    REQUIRE(settlement->WaitEntered(2s));
    // The owner observed the callback, but has not crossed its physical
    // settlement boundary. Access remains unpublished.
    CHECK(request.workspace->revision() == 0U);
    CHECK(request.ready.wait_for(0s) == std::future_status::timeout);
    settlement->Release();
    request.CheckCompleted(producer);
    backend->defer_notifications = false;
    const auto finalizations = backend->workspace_finalizations.load();
    request.Request(producer);
    request.CheckCompleted(producer);
    CHECK(backend->workspace_finalizations == finalizations);
}
TEST_CASE("Explore Annotation Live and Upscale endpoints complete retained workspaces independently", "[presentation][workspace]") {
    namespace gpu = mmltk::frameworks::gpu;
    namespace fixture = gpu::test_support;
    fixture::ImageWorkspaceTestAccess::Reset();
    // A backend models one CUDA implementation; independent producers create
    // separate contexts within it, including for cross-context source copies.
    auto backend = std::make_shared<FakeImageBackend>();
    auto explore_work = std::make_shared<ExploreWorkProbe>();
    LoadedSettings settings;
    ExploreScenario opened(settings, 2U,
                           RuntimeFactory(0, backend, gpu::ImageProductLayout::CleanAndSemantic, ExploreScenario::TrackWork(explore_work), 3U,
                                          fixture::FakeWorkspaceFinalizer(backend)));
    opened.OpenAndWait({.extent = {32U, 32U}, .columns = 1U});
    auto& explore = opened.system();
    EventGate annotation_events, live_events, upscale_events;
    auto annotation_work = std::make_shared<AnnotationRenderProbe>();
    AnnotationSystem annotation{
        kDevice,
        RuntimeFactory(
            0, backend, gpu::ImageProductLayout::CleanAndSemantic, [annotation_work] { return std::make_unique<TestAnnotationAlgorithm>(annotation_work); }, 3U,
            fixture::FakeWorkspaceFinalizer(backend)),
        borrow_exactly_from(explore), [&](AnnotationSystem::event_type) { annotation_events.Advance(); }, mmltk::testsupport::annotation_render_evidence()};
    mmltk::testsupport::open_annotation(annotation, annotation_events, explore.snapshot().frame);
    auto captures = std::make_shared<std::atomic<std::uint64_t>>(0U);
    LiveSystem live{kDevice,
                    RuntimeFactory(
                        0, backend, gpu::ImageProductLayout::Clean, [captures] { return std::make_unique<TestLiveAlgorithm>(captures); }, 1U,
                        fixture::FakeWorkspaceFinalizer(backend)),
                    [&](LiveSystem::event_type) { live_events.Advance(); }};
    static_cast<void>(live.Start({.extent = {32U, 32U}, .frames_per_second = 120U}));
    REQUIRE(live_events.Wait([&] { return live.snapshot().completed_frames != 0U; }));
    static_cast<void>(live.Stop());
    REQUIRE(live_events.Wait([&] { return !live.snapshot().running; }));
    auto kernel = std::make_shared<std::atomic<UpscaleKernel>>(UpscaleKernel::Default);
    auto upscale_runs = std::make_shared<std::atomic_uint32_t>(0U);
    UpscaleSystem upscale{
        kDevice,
        RuntimeFactory(
            0, backend, gpu::ImageProductLayout::CleanAndSemantic, [=] { return std::make_unique<TestUpscaleAlgorithm>(kernel, nullptr, upscale_runs); }, 4U,
            fixture::FakeWorkspaceFinalizer(backend)),
        borrow_exactly_from(explore), [&](UpscaleSystem::event_type) { upscale_events.Advance(); }};
    static_cast<void>(upscale.Start(test_upscale_request({.source = explore.snapshot().frame})));
    REQUIRE(upscale_events.Wait([&] { return upscale.snapshot().ready; }));
    {
        const std::array contexts{explore.BorrowFrame().plane(0U).context(), annotation.BorrowFrame().plane(0U).context(),
                                  live.BorrowFrame().plane(0U).context(), upscale.BorrowFrame().plane(0U).context()};
        for (std::size_t left = 0U; left != contexts.size(); ++left)
            for (std::size_t right = left + 1U; right != contexts.size(); ++right) CHECK_FALSE(contexts[left] == contexts[right]);
    }
    const auto shutdown = [&] {
        backend->defer_notifications = false;
        backend->CompleteNotifications();
        upscale.Shutdown();
        annotation.Shutdown();
        live.Shutdown();
        explore.Shutdown();
    };
    auto cleanup = mmltk::testsupport::ScopedTestCleanup{shutdown};
    const auto renders = explore_work->renders.load();
    const auto opens = explore_work->opens.load();
    const auto prepares = explore_work->prepares.load();
    auto explore_display = request_delayed_workspace(explore, backend);
    const auto annotations = annotation_work->calls.load();
    auto annotation_display = request_delayed_workspace(annotation, backend);
    complete_producer_workspace(explore, backend, explore_display);
    CHECK(explore_work->renders == renders);
    CHECK(explore_work->opens == opens);
    CHECK(explore_work->prepares == prepares);
    CHECK(annotation_display.ready.wait_for(0s) == std::future_status::timeout);
    CHECK_FALSE(annotation.BorrowWorkspace().valid());
    const auto captured = captures->load();
    auto live_display = request_delayed_workspace(live, backend);
    complete_producer_workspace(live, backend, live_display);
    CHECK(captures->load() == captured);
    const auto upscaled = upscale_runs->load();
    auto upscale_display = request_delayed_workspace(upscale, backend);
    complete_producer_workspace(upscale, backend, upscale_display);
    CHECK(upscale_runs->load() == upscaled);
    complete_producer_workspace(annotation, backend, annotation_display);
    CHECK(annotation_work->calls == annotations);
    // Hold the completed semantic image while a new document render is queued.
    auto previous = annotation.BorrowWorkspace();
    REQUIRE(previous.valid());
    const auto previous_pixel = *reinterpret_cast<const std::uint8_t*>(previous.plane().data);
    const auto previous_content = annotation_display.content;
    annotation_work->semantic_value = 0xf0U;
    backend->defer_notifications = true;
    auto submitted = backend->ObserveNextNotificationStream();
    const auto changed = annotation.Edit({.edit = {.value = AnnotationCategoryEdit{contracts::AnnotationText::From("workspace replacement")}}});
    const auto replacement_stream = mmltk::testsupport::await_test_future(submitted, "replacement annotation render submitted");
    CHECK(annotation.ObserveWorkspace().product_revision == previous_content.revision);
    CHECK(annotation.BorrowWorkspace().revision() == previous_content.revision);
    CHECK(previous.revision() == previous_content.revision);
    CHECK(*reinterpret_cast<const std::uint8_t*>(previous.plane().data) == previous_pixel);
    backend->defer_notifications = false;
    backend->CompleteNotifications(replacement_stream);
    mmltk::testsupport::await_annotation_command(annotation, annotation_events, changed.revision);
    REQUIRE(annotation_events.Wait([&] { return annotation.snapshot().frame.revision != previous_content.revision; }));
    previous = {};
    auto replacement_display = request_delayed_workspace(annotation, backend);
    CHECK(replacement_display.content != previous_content);
    complete_producer_workspace(annotation, backend, replacement_display);
    {
        const auto replacement = annotation.BorrowWorkspace();
        REQUIRE(replacement.valid());
        CHECK(*reinterpret_cast<const std::uint8_t*>(replacement.plane().data) != previous_pixel);
    }
    explore_display.workspace.reset();
    annotation_display.workspace.reset();
    live_display.workspace.reset();
    upscale_display.workspace.reset();
    replacement_display.workspace.reset();
    shutdown();
    CHECK(backend->planes_allocated == backend->planes_freed);
    CHECK(backend->contexts_created == backend->contexts_destroyed);
    CHECK(backend->events_created == backend->events_destroyed);
}
}  // namespace
}  // namespace mmltk::controller
