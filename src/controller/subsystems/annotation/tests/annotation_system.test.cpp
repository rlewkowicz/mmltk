#include "src/controller/subsystems/annotation/tests/support/annotation_system_fixture.h"
#include "src/controller/subsystems/explore/tests/support/explore_system_fixture.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/subsystems/annotation/tests/support/annotation_test_utils.hpp"
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/controller/presentation/workspace_input.h"
#include "src/frameworks/gpu/image_workspace.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>
namespace mmltk::controller {
namespace {
using namespace visual_test_support;
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct AnnotationRenderHold final {
 std::shared_ptr<AnnotationRenderProbe> probe = std::make_shared<AnnotationRenderProbe>();
 std::shared_ptr<MutationCommitProbe> hold = std::make_shared<MutationCommitProbe>();
 std::future<void> entered = hold->committed.get_future();
};
[[nodiscard]] AnnotationSystem make_test_annotation(const std::shared_ptr<FakeImageBackend>& backend, ExploreSystem& source, EventGate& events) {
 return AnnotationSystem{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend), borrow_exactly_from(source), [&events](AnnotationSystem::event_type) { events.Advance(); },
  mmltk::testsupport::annotation_render_evidence()};
}
auto settle_annotation_on_exit(AnnotationSystem& annotation, std::promise<void>& release) {
 return mmltk::testsupport::ScopedTestCleanup{[&annotation, &release] {
  mmltk::testsupport::release_test_promise(release);
  annotation.Shutdown();
 }};
}
void complete_annotation_box_drag(AnnotationSystem& annotation, EventGate& events, std::uint64_t peer) {
 // CLEANUP-IGNORE: The box-drag oracle owns its edit receipt; browser-host gesture routing has a separate interaction boundary.
 const auto edit = annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}});
 mmltk::testsupport::await_annotation_command(annotation, events, edit.revision);
 const auto before = annotation.snapshot().ui;
 annotation.Input(mmltk::testsupport::annotation_mouse(annotation, peer, WorkspaceMouseKind::Press, {1.25F, 2.5F}));
 annotation.Input(mmltk::testsupport::annotation_mouse(annotation, peer, WorkspaceMouseKind::Motion, {10.75F, 12.125F}));
 auto release = mmltk::testsupport::annotation_mouse(annotation, peer, WorkspaceMouseKind::Release, {});
 release.point.reset();  // An outside release commits the last accepted motion.
 annotation.Input(release);
 REQUIRE(events.Wait([&] { return annotation.snapshot().ui.document_revision > before.document_revision; }));
 const auto after = annotation.snapshot().ui;
 REQUIRE(after.scene.objects.size() == before.scene.objects.size() + 1U);
 CHECK(after.scene.objects.back().box == contracts::AnnotationBox{{1.25F, 2.5F}, {10.75F, 12.125F}});
 CHECK(after.can_undo);
}
std::shared_ptr<VisualDocument> annotation_mask_document(std::string_view resource) {
 auto document = std::make_shared<VisualDocument>();
 document->scene.document = contracts::WorkspaceResource::From(resource, 1U);
 document->scene.categories = {{.value = "object"}};
 document->scene.objects = {{.name = contracts::AnnotationText::From("mask"), .shape = contracts::AnnotationShape::Mask, .box = {{1, 1}, {16, 16}}}};
 return document;
}
TEST_CASE("Annotation imports Validation ground truth and retains receiver pixels after source release") {
 auto backend = std::make_shared<FakeImageBackend>();
 std::optional<MutableVisualSource> source(std::in_place, backend, VisualExtent{16U, 16U}, 37U);
 auto document = annotation_mask_document("validation://sample");
 document->scene.categories[0].value = "ground truth";
 document->scene.objects[0].mask.present = true;
 document->mask_bounds = {{{0.25F, 0.25F, 0.5F, 0.5F}}};
 document->mask_contains = [](std::size_t object, float x, float y) { return object == 0U && x >= 0.25F && x < 0.5F && y >= 0.25F && y < 0.5F; };
 auto frame = source->frame();
 frame.source.kind = PresentationSourceKind::Validation;
 EventGate events;
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend),
  [&](const VisualFrame& requested) {
   if (!source || requested != frame) return VisualDocumentRead{};
   auto result = source->BorrowExact(source->frame());
   result.document = document;
   return result;
  },
  [&events](AnnotationSystem::event_type) { events.Advance(); }, mmltk::testsupport::annotation_render_evidence()};
 mmltk::testsupport::open_annotation(annotation, events, frame);
 const auto scene = annotation.snapshot().ui.scene;
 REQUIRE(scene.objects.size() == 1U);
 CHECK(scene.categories[0].value == "ground truth");
 REQUIRE(scene.objects[0].mask.runs.size() == 4U);
 CHECK(scene.objects[0].mask.runs.front() == contracts::AnnotationMaskRun{4U, 4U, 7U});
 source.reset();
 document.reset();
 auto owned = annotation.BorrowFrame();
 REQUIRE(owned.valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(owned.plane(0U).plane().data) == 37U);
 CHECK(annotation.snapshot().ui.scene == scene);
}
TEST_CASE("Annotation peer closure orders accepted input before replacement gestures") {
 auto backend = std::make_shared<FakeImageBackend>();
 OpenedExplore source{backend, {32U, 32U}};
 EventGate events;
 auto annotation = make_test_annotation(backend, source.system(), events);
 mmltk::testsupport::open_annotation(annotation, events, source.system().snapshot().frame);
 auto edited = annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}});
 mmltk::testsupport::await_annotation_command(annotation, events, edited.revision);
 annotation.SetInputPeer(1U);
 const auto before = annotation.snapshot().ui.scene.objects.size();
 const auto mouse = [&](std::uint64_t peer, WorkspaceMouseKind kind, WorkspacePoint point) { annotation.Input(mmltk::testsupport::annotation_mouse(annotation, peer, kind, point)); };
 mouse(1U, WorkspaceMouseKind::Press, {2, 3});
 mouse(1U, WorkspaceMouseKind::Motion, {4, 5});
 annotation.PeerClosed();
 annotation.SetInputPeer(2U);
 mouse(2U, WorkspaceMouseKind::Press, {6, 7});
 mouse(2U, WorkspaceMouseKind::Release, {8, 9});
 REQUIRE(events.Wait([&] { return annotation.snapshot().ui.scene.objects.size() == before + 1U; }));
 CHECK(annotation.snapshot().ui.scene.objects.back().box == contracts::AnnotationBox{{6, 7}, {8, 9}});
 CHECK(annotation.snapshot().ready);
}
// CLEANUP-IGNORE: Annotation receiver-copy rejection and Upscale high-water publication are distinct system tests.
TEST_CASE("Annotation renderer failure retires resources and allows source restart") {
 auto backend = std::make_shared<FakeImageBackend>();
 OpenedExplore source{backend, {32U, 32U}};
 auto probe = std::make_shared<AnnotationRenderProbe>();
 EventGate events;
 std::atomic_uint64_t failures{0U};
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, probe), borrow_exactly_from(source.system()),
  [&](AnnotationSystem::event_type event) {
   if (std::holds_alternative<AnnotationFailed>(event)) ++failures;
   events.Advance();
  },
  mmltk::testsupport::annotation_render_evidence()};
 probe->fail_open = true;
 static_cast<void>(annotation.Open(mmltk::testsupport::test_annotation_open(source.system().snapshot().frame)));
 REQUIRE(events.Wait([&] { return failures.load() == 1U; }));
 CHECK_FALSE(annotation.snapshot().ready);
 CHECK_FALSE(annotation.BorrowFrame().valid());
 mmltk::testsupport::open_annotation(annotation, events, source.system().snapshot().frame);
 const auto committed = annotation.snapshot().ui;
 annotation.SetInputPeer(1U);
 auto right = mmltk::testsupport::annotation_mouse(annotation, 1U, WorkspaceMouseKind::Press, {3, 4});
 right.button = WorkspaceMouseButton::Right;
 annotation.Input(right);
 probe->fail_render = true;
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}}));
 REQUIRE(events.Wait([&] { return failures.load() == 2U; }));
 CHECK_FALSE(annotation.snapshot().ready);
 CHECK_FALSE(annotation.BorrowFrame().valid());
 CHECK(annotation.snapshot().ui.document_revision >= committed.document_revision);
 mmltk::testsupport::open_annotation(annotation, events, source.system().snapshot().frame);
 complete_annotation_box_drag(annotation, events, 1U);
 CHECK(failures.load() == 2U);
}
TEST_CASE("Annotation unavailable source settles ordered input and commands before queued Open recovery") {
 const auto tool = GENERATE(contracts::AnnotationTool::Box, contracts::AnnotationTool::ColorSample);
 const bool request_stop = GENERATE(false, true);
 auto backend = std::make_shared<FakeImageBackend>();
 MutableVisualSource source{backend, {32U, 32U}};
 auto semantic = annotation_mask_document("test://unavailable-source");
 AnnotationRenderHold render;
 EventGate events;
 std::mutex observations_mutex;
 std::vector<AnnotationFailed> failures;
 std::vector<std::uint64_t> failure_render_counts;
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, render.probe), source.WithDocument(semantic),
  [&](AnnotationSystem::event_type event) {
   if (const auto* failure = std::get_if<AnnotationFailed>(&event)) {
    std::scoped_lock lock(observations_mutex);
    failures.push_back(*failure);
    failure_render_counts.push_back(render.probe->calls.load());
   }
   events.Advance();
  },
  mmltk::testsupport::annotation_render_evidence()};
 auto release = settle_annotation_on_exit(annotation, render.hold->release);
 mmltk::testsupport::open_annotation(annotation, events, source.frame());
 annotation.SetInputPeer(1U);
 auto command = annotation.Edit({.edit = {.value = AnnotationCategoryEdit{contracts::AnnotationText::From("retained history")}}});
 mmltk::testsupport::await_annotation_command(annotation, events, command.revision);
 command = annotation.Edit({.edit = {.value = AnnotationObjectEdit{0U}}});
 mmltk::testsupport::await_annotation_command(annotation, events, command.revision);
 auto retained = hold_annotation_frame(annotation, events, tool);
 const auto prior = annotation.snapshot();
 REQUIRE(prior.ui.can_undo);
 const auto rendered = render.probe->calls.load();
 {
  std::scoped_lock lock(render.probe->mutex);
  render.probe->open_hold = render.hold;
 }
 render.probe->fail_open = true;
 static_cast<void>(annotation.Open(mmltk::testsupport::test_annotation_open(source.frame())));
 REQUIRE(render.entered.wait_for(2s) == std::future_status::ready);
 for (const auto kind : {WorkspaceMouseKind::Press, WorkspaceMouseKind::Motion, WorkspaceMouseKind::Release})
  annotation.Input(mmltk::testsupport::annotation_mouse(annotation, 1U, kind, {4.25F, 5.125F}));
 // Empty and passive captures share admission without manufacturing edits.
 for (const auto entry : mmltk::frameworks::reflection::enum_entries<WorkspaceMouseKind>()) {
  auto mouse = mmltk::testsupport::annotation_mouse(annotation, 1U, entry.value, {});
  mouse.point.reset();
  annotation.Input(mouse);
 }
 command = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
 CHECK(command.busy);
 mmltk::testsupport::ScopedTempDir saved{"mmltk-annotation-source-readiness"};
 const auto destination = saved.path() / "annotation.cbor";
 CHECK(annotation.Save({.destination = destination.string()}).busy);
 CHECK(annotation.Open(mmltk::testsupport::test_annotation_open(source.frame())).busy);
 if (request_stop) CHECK(annotation.Stop().cancellation_requested);
 render.hold->release.set_value();
 REQUIRE(events.Wait([&] {
  const auto state = annotation.snapshot();
  return state.ready && !state.busy && state.input_document_epoch == prior.input_document_epoch + 1U;
 }));
 {
  std::scoped_lock lock(observations_mutex);
  REQUIRE(failures.size() == 4U);
  CHECK(failures.front().detail == "deterministic source preparation failure");
  for (std::size_t index = 0U; index != failures.size(); ++index) {
   CHECK_FALSE(failures[index].snapshot.ready);
   CHECK(failures[index].snapshot.ui == prior.ui);
   CHECK(failure_render_counts[index] == rendered);
   if (index != 0U) CHECK(failures[index].detail == "Annotation source image is unavailable");
  }
 }
 CHECK_FALSE(std::filesystem::exists(destination));
 CHECK(render.probe->samples.load() == 0U);
 REQUIRE(retained.valid());
 retained = {};
 complete_annotation_box_drag(annotation, events, 1U);
 command = annotation.Save({.destination = destination.string()});
 mmltk::testsupport::await_annotation_command(annotation, events, command.revision);
 CHECK(annotation.snapshot().ui.save_status == contracts::AnnotationSaveStatus::Saved);
 CHECK(std::filesystem::exists(destination));
}
TEST_CASE("Annotation rejects an oversized incoming document without changing its existing editor or pixels") {
 auto backend = std::make_shared<FakeImageBackend>();
 MutableVisualSource source{backend, {512U, 256U}};
 auto rejected_document = std::make_shared<VisualDocument>();
 rejected_document->scene.document = contracts::WorkspaceResource::From("test://oversized", 1U);
 rejected_document->scene.categories = {{.value = "object"}};
 SECTION("object capacity") { rejected_document->scene.objects.resize(contracts::kAnnotationObjectCapacity + 1U); }
 SECTION("materialized mask capacity") {
  rejected_document->scene.objects.push_back({.shape = contracts::AnnotationShape::Mask, .box = {{0.0F, 0.0F}, {512.0F, 256.0F}}, .mask = {.present = true}});
  rejected_document->mask_bounds.resize(rejected_document->scene.objects.size(), {0, 0, 1, 1});
  rejected_document->mask_contains = [](std::size_t, float x, float) { return static_cast<unsigned>(x * 512.0F) % 2U == 0U; };
 }
 bool reject_incoming = false;
 EventGate events;
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend),
  [&](const VisualFrame& frame) {
   auto borrowed = source.BorrowExact(frame);
   if (reject_incoming) borrowed.document = rejected_document;
   return borrowed;
  },
  [&events](AnnotationSystem::event_type) { events.Advance(); }, mmltk::testsupport::annotation_render_evidence()};
 mmltk::testsupport::open_annotation(annotation, events, source.frame());
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationCategoryEdit{contracts::AnnotationText::From("kept category")}}}));
 REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}}));
 REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
 mmltk::testsupport::await_annotation_render(annotation, events);
 const auto prior = annotation.snapshot();
 const auto copies = backend->same_copies.load(std::memory_order_acquire);
 const auto pixel = *reinterpret_cast<const std::uint8_t*>(annotation.BorrowFrame().plane(0U).plane().data);
 reject_incoming = true;
 static_cast<void>(annotation.Open(mmltk::testsupport::test_annotation_open(source.frame())));
 REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
 CHECK(annotation.snapshot().ready);
 CHECK(annotation.snapshot().ui == prior.ui);
 CHECK(annotation.snapshot().frame == prior.frame);
 CHECK(backend->same_copies.load(std::memory_order_acquire) == copies);
 REQUIRE(annotation.BorrowFrame().valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(annotation.BorrowFrame().plane(0U).plane().data) == pixel);
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationRedoEdit{}}}));
 REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
 CHECK(annotation.snapshot().ui.document_revision == prior.ui.document_revision + 1U);
 REQUIRE(annotation.snapshot().ui.scene.categories.size() == prior.ui.scene.categories.size() + 1U);
 CHECK(annotation.snapshot().ui.scene.categories.back().value == "kept category");
}
TEST_CASE("Annotation reduces input and settles commands while rendering is held") {
 auto backend = std::make_shared<FakeImageBackend>();
 OpenedExplore source{backend, {32U, 32U}};
 AnnotationRenderHold render;
 EventGate events;
 std::atomic_uint64_t held_scene{0U};
 std::atomic_bool older_completed{false};
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, render.probe), borrow_exactly_from(source.system()), [&](AnnotationSystem::event_type) { events.Advance(); },
  mmltk::testsupport::annotation_render_evidence()};
 auto release = settle_annotation_on_exit(annotation, render.hold->release);
 mmltk::testsupport::open_annotation(annotation, events, source.system().snapshot().frame);
 const auto initial = annotation.snapshot();
 {
  std::scoped_lock lock(render.probe->mutex);
  render.probe->hold = render.hold;
 }
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}}));
 REQUIRE(render.entered.wait_for(2s) == std::future_status::ready);
 held_scene = annotation.snapshot().ui.scene_revision;
 annotation.SetInputPeer(1U);
 for (const auto kind : {WorkspaceMouseKind::Press, WorkspaceMouseKind::Motion, WorkspaceMouseKind::Release}) {
  const float coordinate = kind == WorkspaceMouseKind::Press ? 2.0F : kind == WorkspaceMouseKind::Motion ? 4.0F : 6.0F;
  annotation.Input(mmltk::testsupport::annotation_mouse(annotation, 1U, kind, {coordinate, coordinate + 1.0F}));
 }
 REQUIRE(events.Wait([&] { return annotation.snapshot().ui.document_revision > initial.ui.document_revision; }));
 CHECK(annotation.snapshot().frame == initial.frame);
 const auto committed = annotation.snapshot().ui.document_revision;
 const auto command = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
 mmltk::testsupport::await_annotation_command(annotation, events, command.revision);
 CHECK(annotation.snapshot().ui.document_revision > committed);
 const auto document_epoch = annotation.snapshot().input_document_epoch;
 static_cast<void>(annotation.Open(mmltk::testsupport::test_annotation_open(source.system().snapshot().frame)));
 CHECK(annotation.Stop().cancellation_requested);
 render.hold->release.set_value();
 REQUIRE(events.Wait([&] { return !annotation.snapshot().busy; }));
 CHECK(annotation.snapshot().input_document_epoch == document_epoch);
 mmltk::testsupport::await_annotation_render(annotation, events);
 CHECK(annotation.snapshot().frame.revision > initial.frame.revision);
 CHECK(render.probe->exact_content.load());
}
TEST_CASE("Annotation Open consumes retained prior-document input and accepts the replacement gesture") {
 const auto terminal = GENERATE(WorkspaceMouseKind::Release, WorkspaceMouseKind::Cancel);
 auto backend = std::make_shared<FakeImageBackend>();
 OpenedExplore source{backend, {32U, 32U}};
 AnnotationRenderHold render;
 EventGate events;
 std::atomic_uint64_t failures{0U};
 std::mutex observations_mutex;
 std::vector<AnnotationSnapshot> replacements;
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, render.probe), borrow_exactly_from(source.system()), [&](AnnotationSystem::event_type event) {
                              if (std::holds_alternative<AnnotationFailed>(event)) ++failures;
                              if (const auto* changed = std::get_if<AnnotationChanged>(&event); changed && changed->snapshot.input_document_epoch == 2U) {
                               std::scoped_lock lock(observations_mutex);
                               replacements.push_back(changed->snapshot);
                              }
                              events.Advance();
                             }};
 auto release = settle_annotation_on_exit(annotation, render.hold->release);
 mmltk::testsupport::open_annotation(annotation, events, source.system().snapshot().frame);
 annotation.SetInputPeer(1U);
 auto right = mmltk::testsupport::annotation_mouse(annotation, 1U, WorkspaceMouseKind::Press, {3, 4});
 right.button = WorkspaceMouseButton::Right;
 annotation.Input(right);
 {
  std::scoped_lock lock(render.probe->mutex);
  render.probe->open_hold = render.hold;
 }
 static_cast<void>(annotation.Open(mmltk::testsupport::test_annotation_open(source.system().snapshot().frame)));
 REQUIRE(render.entered.wait_for(2s) == std::future_status::ready);
 for (std::size_t index = 0U; index != 128U; ++index) {
  auto motion = right;
  motion.kind = WorkspaceMouseKind::Motion;
  motion.point = WorkspacePoint{static_cast<float>(index % 16U) + 0.25F, 5.125F};
  annotation.Input(motion);
 }
 right.kind = terminal;
 annotation.Input(right);
 // This no-op tool command is an ordinary FIFO settlement boundary after
 // every old-document record, with no input acknowledgement or test hook.
 static_cast<void>(annotation.Edit({.edit = {.value = AnnotationToolEdit{contracts::AnnotationTool::Select}}}));
 render.hold->release.set_value();
 REQUIRE(events.Wait([&] {
  std::scoped_lock lock(observations_mutex);
  return replacements.size() >= 2U && !replacements.back().busy;
 }));
 {
  std::scoped_lock lock(observations_mutex);
  REQUIRE(replacements.size() == 2U);
  CHECK(replacements.front().ui == replacements.back().ui);
  CHECK(annotation.snapshot().ui == replacements.front().ui);
 }
 CHECK(failures.load() == 0U);
 CHECK(annotation.snapshot().input_document_epoch == 2U);
 complete_annotation_box_drag(annotation, events, 1U);
 CHECK(failures.load() == 0U);
}
TEST_CASE("Annotation color sampling completes before following document commands under output pressure") {
 auto backend = std::make_shared<FakeImageBackend>();
 MutableVisualSource source{backend, {32U, 32U}};
 auto semantic = annotation_mask_document("test://sample-mask");
 auto probe = std::make_shared<AnnotationRenderProbe>();
 auto sample = std::make_shared<MutationCommitProbe>();
 auto entered = sample->committed.get_future();
 EventGate events;
 AnnotationSystem annotation{kDevice, TestAnnotationAlgorithm::CreateRuntime(backend, probe), source.WithDocument(semantic), [&](AnnotationSystem::event_type) { events.Advance(); },
  mmltk::testsupport::annotation_render_evidence()};
 auto release = settle_annotation_on_exit(annotation, sample->release);
 mmltk::testsupport::open_annotation(annotation, events, source.frame());
 auto edit = annotation.Edit({.edit = {.value = AnnotationObjectEdit{0U}}});
 mmltk::testsupport::await_annotation_command(annotation, events, edit.revision);
 auto held = hold_annotation_frame(annotation, events, contracts::AnnotationTool::ColorSample);
 {
  std::scoped_lock lock(probe->mutex);
  probe->sample_hold = sample;
 }
 annotation.SetInputPeer(1U);
 annotation.Input(mmltk::testsupport::annotation_mouse(annotation, 1U, WorkspaceMouseKind::Press, {4, 5}));
 annotation.Input(mmltk::testsupport::annotation_mouse(annotation, 1U, WorkspaceMouseKind::Release, {4, 5}));
 REQUIRE(entered.wait_for(2s) == std::future_status::ready);
 SECTION("ordered document backlog") {
  for (std::size_t index = 0U; index != 64U; ++index) {
   annotation.Input(mmltk::testsupport::annotation_mouse(annotation, 1U, WorkspaceMouseKind::Motion, {static_cast<float>(index) + 0.25F, 5.125F}));
   CHECK(annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}}).busy);
   CHECK(annotation.Edit({.edit = {.value = AnnotationRedoEdit{}}}).busy);
  }
  const auto undo = annotation.Edit({.edit = {.value = AnnotationUndoEdit{}}});
  CHECK(undo.busy);
  sample->release.set_value();
  mmltk::testsupport::await_annotation_command(annotation, events, undo.revision);
  CHECK_FALSE(annotation.snapshot().ui.scene.objects[0].sup.sampling);
  edit = annotation.Edit({.edit = {.value = AnnotationRedoEdit{}}});
  mmltk::testsupport::await_annotation_command(annotation, events, edit.revision);
  CHECK(annotation.snapshot().ui.scene.objects[0].sup.sampling);
  CHECK(annotation.snapshot().ui.scene.objects[0].sup.center.hue == 120.0F);
 }
 SECTION("Stop cancels sampling while later document work remains admissible") {
  CHECK(annotation.Stop().cancellation_requested);
  const auto categories = annotation.snapshot().ui.scene.categories.size();
  edit = annotation.Edit({.edit = {.value = AnnotationCategoryEdit{contracts::AnnotationText::From("after sample")}}});
  CHECK(edit.busy);
  sample->release.set_value();
  mmltk::testsupport::await_annotation_command(annotation, events, edit.revision);
  CHECK_FALSE(annotation.snapshot().ui.scene.objects[0].sup.sampling);
  CHECK(annotation.snapshot().ui.scene.categories.size() == categories + 1U);
 }
 held = {};
}
}  // namespace
}  // namespace mmltk::controller
